#!/usr/bin/env python3
"""
agent-bot v2: Telegram + LINE + Web Chat → agent.v10
- Persistent context: SQLite on Fly.io /data volume (falls back to /tmp)
- Streaming progress: Telegram/LINE get 30s heartbeats; Web gets SSE live feed
- Web chat UI: token-protected, real-time streaming output in browser
"""
import os, re, hmac, hashlib, base64, subprocess, threading, time
import tempfile, json, sqlite3, queue as q_mod
from collections import deque
from flask import Flask, request, jsonify, Response, stream_with_context
import requests

# ─── Config ────────────────────────────────────────────────────────────────

TELEGRAM_TOKEN            = os.getenv('TELEGRAM_BOT_TOKEN', '')
LINE_CHANNEL_SECRET       = os.getenv('LINE_CHANNEL_SECRET', '')
LINE_CHANNEL_ACCESS_TOKEN = os.getenv('LINE_CHANNEL_ACCESS_TOKEN', '')
ANTHROPIC_API_KEY         = os.getenv('ANTHROPIC_API_KEY', '')
AGENT_PATH                = os.getenv('AGENT_PATH', '/app/agent.v10')
AGENT_DIR                 = os.getenv('AGENT_DIR', '/app')
PORT                      = int(os.getenv('PORT', 8080))
WEB_TOKEN                 = os.getenv('WEB_TOKEN', '')   # empty = web chat disabled

DB_PATH = '/data/agent-bot.db' if os.path.isdir('/data') else '/tmp/agent-bot.db'

MAX_TASK_LEN      = 2000
RATE_LIMIT_MAX    = 10
RATE_LIMIT_WIN    = 60
MAX_CONTEXT_TURNS = 10

_auth_raw = os.getenv('AUTHORIZED_USERS', '*')
AUTHORIZED_USERS = set(_auth_raw.split(',')) if _auth_raw and _auth_raw != '*' else None

app = Flask(__name__)

# ─── SQLite (persistent context) ──────────────────────────────────────────

def get_db():
    con = sqlite3.connect(DB_PATH, check_same_thread=False)
    con.execute('PRAGMA journal_mode=WAL')
    con.row_factory = sqlite3.Row
    return con

def init_db():
    with get_db() as con:
        con.execute('''CREATE TABLE IF NOT EXISTS context (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id     TEXT    NOT NULL,
            role        TEXT    NOT NULL,
            text        TEXT    NOT NULL,
            created_at  INTEGER NOT NULL DEFAULT (strftime('%s','now'))
        )''')
        con.execute('CREATE INDEX IF NOT EXISTS idx_ctx_user ON context(user_id, id)')
        con.commit()

def load_context_db(user_id: str) -> list[dict]:
    with get_db() as con:
        rows = con.execute(
            'SELECT role, text FROM context WHERE user_id=? ORDER BY id DESC LIMIT ?',
            (user_id, MAX_CONTEXT_TURNS * 2)
        ).fetchall()
    return [{'role': r['role'], 'text': r['text']} for r in reversed(rows)]

def save_context_db(user_id: str, task: str, result: str):
    with get_db() as con:
        con.execute('INSERT INTO context(user_id,role,text) VALUES(?,?,?)',
                    (user_id, 'user', task[:1000]))
        con.execute('INSERT INTO context(user_id,role,text) VALUES(?,?,?)',
                    (user_id, 'agent', result[:2000]))
        con.commit()

def clear_context_db(user_id: str):
    with get_db() as con:
        con.execute('DELETE FROM context WHERE user_id=?', (user_id,))
        con.commit()

def context_count(user_id: str) -> int:
    with get_db() as con:
        r = con.execute('SELECT COUNT(*)/2 FROM context WHERE user_id=?', (user_id,)).fetchone()
    return r[0] if r else 0

# ─── Per-user in-memory state ──────────────────────────────────────────────

_state_lock = threading.Lock()

class UserState:
    def __init__(self):
        self.proc: subprocess.Popen | None = None
        self.proc_lock   = threading.Lock()
        self.rate_times: deque = deque()
        self.stream_q: q_mod.Queue | None = None   # web SSE queue

_states: dict[str, UserState] = {}

def get_state(uid: str) -> UserState:
    with _state_lock:
        if uid not in _states:
            _states[uid] = UserState()
        return _states[uid]

# ─── Security helpers ──────────────────────────────────────────────────────

def is_authorized(uid) -> bool:
    if AUTHORIZED_USERS is None:
        return True
    return str(uid) in AUTHORIZED_USERS

def check_rate_limit(uid: str) -> bool:
    st = get_state(uid)
    now = time.monotonic()
    with _state_lock:
        while st.rate_times and now - st.rate_times[0] > RATE_LIMIT_WIN:
            st.rate_times.popleft()
        if len(st.rate_times) >= RATE_LIMIT_MAX:
            return False
        st.rate_times.append(now)
        return True

def is_busy(uid: str) -> bool:
    st = get_state(uid)
    with st.proc_lock:
        return st.proc is not None and st.proc.poll() is None

def check_web_token(token: str) -> bool:
    if not WEB_TOKEN:
        return False
    return hmac.compare_digest(token or '', WEB_TOKEN)

# ─── Helpers ───────────────────────────────────────────────────────────────

ANSI = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])')

def strip_ansi(t: str) -> str:
    return ANSI.sub('', t)

def chunk(text: str, size: int) -> list[str]:
    return [text[i:i+size] for i in range(0, len(text), size)]

def build_task(uid: str, task: str) -> str:
    ctx = load_context_db(uid)
    if not ctx:
        return task
    parts = ['[Previous conversation:']
    for c in ctx:
        role = 'User' if c['role'] == 'user' else 'Agent'
        parts.append(f'{role}: {c["text"][:500]}')
    parts.append(']')
    parts.append(task)
    return '\n'.join(parts)

# ─── Agent runner: blocking (Telegram / LINE) ──────────────────────────────

def run_agent(uid: str, task: str, file_path: str | None = None) -> str:
    env = dict(os.environ)
    if ANTHROPIC_API_KEY:
        env['ANTHROPIC_API_KEY'] = ANTHROPIC_API_KEY

    full_task = build_task(uid, task)
    if file_path:
        full_task += f'\n[Attached file: {file_path}]'

    st = get_state(uid)
    try:
        proc = subprocess.Popen(
            [AGENT_PATH, '--quiet', full_task],
            cwd=AGENT_DIR, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, env=env,
        )
        with st.proc_lock:
            st.proc = proc
        try:
            out, err = proc.communicate(timeout=300)
        except subprocess.TimeoutExpired:
            proc.kill(); proc.communicate()
            return '⏱ タイムアウト（5分）'
        raw = (out or err or '').strip() or f'(no output, exit={proc.returncode})'
        result = strip_ansi(raw)
        save_context_db(uid, task, result)
        return result
    except FileNotFoundError:
        return f'❌ agent not found: {AGENT_PATH}'
    except Exception as e:
        return f'❌ Error: {e}'
    finally:
        with st.proc_lock:
            st.proc = None

# ─── Agent runner: streaming (Web SSE) ────────────────────────────────────

def run_agent_stream(uid: str, task: str, file_path: str | None = None):
    """Generator: yields stripped lines, saves context when done."""
    env = dict(os.environ)
    if ANTHROPIC_API_KEY:
        env['ANTHROPIC_API_KEY'] = ANTHROPIC_API_KEY

    full_task = build_task(uid, task)
    if file_path:
        full_task += f'\n[Attached file: {file_path}]'

    st = get_state(uid)
    accumulated: list[str] = []
    try:
        proc = subprocess.Popen(
            [AGENT_PATH, full_task],           # no --quiet → live output
            cwd=AGENT_DIR, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, env=env, bufsize=1,
        )
        with st.proc_lock:
            st.proc = proc
        for line in proc.stdout:
            clean = strip_ansi(line)
            accumulated.append(clean)
            yield clean
        proc.wait()
        save_context_db(uid, task, ''.join(accumulated))
    except FileNotFoundError:
        yield f'❌ agent not found: {AGENT_PATH}\n'
    except Exception as e:
        yield f'❌ Error: {e}\n'
    finally:
        with st.proc_lock:
            st.proc = None

# ─── Progress-heartbeat dispatcher (Telegram / LINE) ──────────────────────

def dispatch_with_progress(uid: str, task: str, file_path: str | None,
                           progress_fn, done_fn):
    """Run agent in background; call progress_fn(elapsed_s) every 30s."""
    done_ev = threading.Event()

    def agent_thread():
        result = run_agent(uid, task, file_path)
        done_ev.set()
        done_fn(result)

    def heartbeat():
        elapsed = 0
        while not done_ev.wait(timeout=30):
            elapsed += 30
            progress_fn(elapsed)

    threading.Thread(target=agent_thread,  daemon=True).start()
    threading.Thread(target=heartbeat,     daemon=True).start()

# ─── Cancel ────────────────────────────────────────────────────────────────

def cancel_agent(uid: str) -> bool:
    st = get_state(uid)
    with st.proc_lock:
        if st.proc and st.proc.poll() is None:
            st.proc.kill()
            return True
    return False

# ─── File downloads ────────────────────────────────────────────────────────

def tg_download(file_id: str) -> str | None:
    try:
        r = requests.get(f'https://api.telegram.org/bot{TELEGRAM_TOKEN}/getFile',
                         params={'file_id': file_id}, timeout=10)
        fp = r.json()['result']['file_path']
        data = requests.get(
            f'https://api.telegram.org/file/bot{TELEGRAM_TOKEN}/{fp}', timeout=30).content
        ext = os.path.splitext(fp)[1] or '.bin'
        tmp = tempfile.NamedTemporaryFile(delete=False, suffix=ext, dir='/tmp')
        tmp.write(data); tmp.close()
        return tmp.name
    except Exception as e:
        print(f'[tg] download error: {e}')
        return None

def line_download(msg_id: str) -> str | None:
    try:
        url = f'https://api-data.line.me/v2/bot/message/{msg_id}/content'
        headers = {'Authorization': f'Bearer {LINE_CHANNEL_ACCESS_TOKEN}'}
        r = requests.get(url, headers=headers, timeout=30)
        ct = r.headers.get('Content-Type', '').split(';')[0]
        ext = {'image/jpeg':'.jpg','image/png':'.png','audio/m4a':'.m4a',
               'video/mp4':'.mp4','application/pdf':'.pdf'}.get(ct, '.bin')
        tmp = tempfile.NamedTemporaryFile(delete=False, suffix=ext, dir='/tmp')
        tmp.write(r.content); tmp.close()
        return tmp.name
    except Exception as e:
        print(f'[line] download error: {e}')
        return None

def rm(path: str | None):
    if path:
        try: os.unlink(path)
        except Exception: pass

# ─── Telegram ──────────────────────────────────────────────────────────────

TG_HELP = (
    'agent.v10 bot\n\n'
    'タスクを送信してください。\n\n'
    'コマンド:\n'
    '  /help   — このメッセージ\n'
    '  /status — 実行状態を確認\n'
    '  /cancel — 実行中タスクをキャンセル\n'
    '  /clear  — 会話コンテキストをリセット\n\n'
    '画像・ファイルも対応。\n'
    '実行中は30秒ごとに進捗報告します。'
)

def tg_send(chat_id, text: str):
    if not TELEGRAM_TOKEN: return
    url = f'https://api.telegram.org/bot{TELEGRAM_TOKEN}/sendMessage'
    for part in chunk(text, 4000):
        try: requests.post(url, json={'chat_id': chat_id, 'text': part}, timeout=15)
        except Exception as e: print(f'[tg] send error: {e}')

def tg_handle(update: dict):
    try:
        msg = update.get('message') or update.get('edited_message')
        if not msg: return
        chat_id = msg['chat']['id']
        uid = str(msg['from']['id'])
        if not is_authorized(uid):
            tg_send(chat_id, '⛔ Unauthorized'); return

        text = (msg.get('text') or msg.get('caption') or '').strip()

        if text in ('/start', '/help'):
            tg_send(chat_id, TG_HELP + f'\n\nあなたのID: {uid}'); return
        if text == '/status':
            busy = is_busy(uid)
            cnt  = context_count(uid)
            tg_send(chat_id,
                '⚙️ タスク実行中（/cancel でキャンセル）' if busy
                else f'✅ アイドル（コンテキスト: {cnt}ターン）')
            return
        if text == '/cancel':
            tg_send(chat_id, '🛑 キャンセルしました。' if cancel_agent(uid)
                              else '（実行中のタスクはありません）'); return
        if text == '/clear':
            clear_context_db(uid)
            tg_send(chat_id, '🗑 コンテキストをクリアしました。'); return

        # file?
        file_id = None
        if 'photo' in msg:
            file_id = msg['photo'][-1]['file_id']; text = text or '画像を確認してください'
        elif 'document' in msg:
            file_id = msg['document']['file_id']; text = text or 'ファイルを確認してください'
        elif 'audio' in msg or 'voice' in msg:
            src = msg.get('audio') or msg.get('voice')
            file_id = src['file_id']; text = text or '音声を確認してください'

        if not text: return
        if not check_rate_limit(uid):
            tg_send(chat_id, f'⚠️ レートリミット（{RATE_LIMIT_WIN}s後に再試行）'); return
        if is_busy(uid):
            tg_send(chat_id, '⚙️ 実行中です。/cancel でキャンセル後に再送してください。'); return
        if len(text) > MAX_TASK_LEN:
            tg_send(chat_id, f'⚠️ 入力が長すぎます（最大{MAX_TASK_LEN}文字）'); return

        preview = text[:80] + ('…' if len(text) > 80 else '')
        tg_send(chat_id, f'⏳ 実行中...\n{preview}')

        def progress(elapsed: int):
            tg_send(chat_id, f'⏳ 実行中... ({elapsed}秒経過)')

        def done(result: str):
            fp = tg_download(file_id) if file_id else None
            tg_send(chat_id, result)
            rm(fp)

        dispatch_with_progress(uid, text, None, progress, done)

    except Exception as e:
        print(f'[tg] handle error: {e}')

def tg_poll():
    if not TELEGRAM_TOKEN:
        print('[tg] disabled'); return
    print('[tg] Starting long-polling...')
    offset = 0
    while True:
        try:
            r = requests.get(
                f'https://api.telegram.org/bot{TELEGRAM_TOKEN}/getUpdates',
                params={'offset': offset, 'timeout': 30}, timeout=35)
            if r.status_code != 200: time.sleep(5); continue
            for upd in r.json().get('result', []):
                offset = max(offset, upd['update_id'] + 1)
                tg_handle(upd)
        except requests.exceptions.Timeout:
            continue
        except Exception as e:
            print(f'[tg] poll error: {e}'); time.sleep(5)

# ─── LINE ──────────────────────────────────────────────────────────────────

LINE_HELP = (
    'agent.v10 bot\n\n'
    'タスクを送信してください。\n\n'
    'コマンド:\n'
    '  /help   — このメッセージ\n'
    '  /status — 実行状態を確認\n'
    '  /cancel — 実行中タスクをキャンセル\n'
    '  /clear  — 会話コンテキストをリセット\n\n'
    '画像・ファイルも対応。\n'
    '実行中は30秒ごとに進捗報告します。'
)

def line_verify_sig(body: bytes, sig: str) -> bool:
    if not LINE_CHANNEL_SECRET:
        print('[line] WARNING: LINE_CHANNEL_SECRET not set — rejecting')
        return False
    mac = hmac.new(LINE_CHANNEL_SECRET.encode(), body, hashlib.sha256).digest()
    return hmac.compare_digest(base64.b64encode(mac).decode(), sig or '')

def line_reply(token: str, text: str):
    if not LINE_CHANNEL_ACCESS_TOKEN or not token: return
    parts = chunk(text, 2000)[:5]
    try:
        requests.post('https://api.line.me/v2/bot/message/reply',
            headers={'Content-Type':'application/json',
                     'Authorization':f'Bearer {LINE_CHANNEL_ACCESS_TOKEN}'},
            json={'replyToken': token,
                  'messages': [{'type':'text','text':p} for p in parts]},
            timeout=10)
    except Exception as e: print(f'[line] reply error: {e}')

def line_push(uid: str, text: str):
    if not LINE_CHANNEL_ACCESS_TOKEN: return
    parts = chunk(text, 2000)[:5]
    try:
        requests.post('https://api.line.me/v2/bot/message/push',
            headers={'Content-Type':'application/json',
                     'Authorization':f'Bearer {LINE_CHANNEL_ACCESS_TOKEN}'},
            json={'to': uid, 'messages': [{'type':'text','text':p} for p in parts]},
            timeout=10)
    except Exception as e: print(f'[line] push error: {e}')

@app.route('/webhook/line', methods=['POST'])
def line_webhook():
    body = request.get_data()
    if not line_verify_sig(body, request.headers.get('X-Line-Signature', '')):
        return 'Invalid signature', 403
    if len(body) > 65536:
        return 'Payload too large', 413
    for ev in (request.json or {}).get('events', []):
        _line_event(ev)
    return jsonify({'status': 'ok'}), 200

def _line_event(ev: dict):
    try:
        if ev.get('type') != 'message': return
        reply_token = ev.get('replyToken', '')
        uid   = ev.get('source', {}).get('userId', '')
        msg   = ev.get('message', {})
        msg_type = msg.get('type')
        msg_id   = msg.get('id', '')

        if not is_authorized(uid):
            line_reply(reply_token, '⛔ Unauthorized'); return

        if msg_type == 'text':
            text = msg.get('text', '').strip()
            if text in ('/help', '/start'):
                line_reply(reply_token, LINE_HELP); return
            if text == '/status':
                busy = is_busy(uid); cnt = context_count(uid)
                line_reply(reply_token,
                    '⚙️ 実行中（/cancel でキャンセル）' if busy
                    else f'✅ アイドル（コンテキスト: {cnt}ターン）')
                return
            if text == '/cancel':
                line_reply(reply_token,
                    '🛑 キャンセルしました。' if cancel_agent(uid)
                    else '（実行中のタスクはありません）')
                return
            if text == '/clear':
                clear_context_db(uid)
                line_reply(reply_token, '🗑 コンテキストをクリアしました。'); return
            _line_dispatch(uid, reply_token, text)

        elif msg_type in ('image', 'video', 'audio', 'file'):
            label = {'image':'画像','video':'動画','audio':'音声','file':'ファイル'}.get(msg_type,'コンテンツ')
            _line_dispatch(uid, reply_token, f'{label}を確認してください', file_id=msg_id)
    except Exception as e:
        print(f'[line] event error: {e}')

def _line_dispatch(uid: str, reply_token: str, task: str, file_id: str | None = None):
    if not check_rate_limit(uid):
        line_reply(reply_token, f'⚠️ レートリミット（{RATE_LIMIT_WIN}s後に再試行）'); return
    if is_busy(uid):
        line_reply(reply_token, '⚙️ 実行中。/cancel でキャンセル後に再送してください。'); return
    if len(task) > MAX_TASK_LEN:
        line_reply(reply_token, f'⚠️ 入力が長すぎます（最大{MAX_TASK_LEN}文字）'); return

    preview = task[:80] + ('…' if len(task) > 80 else '')
    line_reply(reply_token, f'⏳ 実行中...\n{preview}')

    def progress(elapsed: int):
        line_push(uid, f'⏳ 実行中... ({elapsed}秒経過)')

    def done(result: str):
        fp = line_download(file_id) if file_id else None
        line_push(uid, result)
        rm(fp)

    dispatch_with_progress(uid, task, None, progress, done)

# ─── Web Chat & Dashboard ──────────────────────────────────────────────────

@app.route('/api/chat', methods=['POST'])
def api_chat():
    if not check_web_token(request.headers.get('X-Web-Token', '')):
        return 'Unauthorized', 401
    data = request.json or {}
    task = (data.get('task') or '').strip()
    if not task:           return 'Empty task', 400
    if len(task) > MAX_TASK_LEN: return f'Too long (max {MAX_TASK_LEN})', 400
    uid = 'web'
    if is_busy(uid):       return 'Task already running', 409

    st = get_state(uid)
    sq: q_mod.Queue = q_mod.Queue()
    st.stream_q = sq

    def run_stream():
        try:
            for line in run_agent_stream(uid, task):
                sq.put({'type': 'line', 'text': line})
            sq.put({'type': 'done'})
        except Exception as e:
            sq.put({'type': 'error', 'text': str(e)})
        finally:
            st.stream_q = None

    threading.Thread(target=run_stream, daemon=True).start()
    return jsonify({'status': 'started'}), 200

@app.route('/api/chat/stream', methods=['GET'])
def api_chat_stream():
    if not check_web_token(request.args.get('token', '')):
        return 'Unauthorized', 401
    uid = 'web'
    st  = get_state(uid)

    def generate():
        # wait up to 3s for queue to appear (task just started)
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            if st.stream_q is not None:
                break
            time.sleep(0.1)
        sq = st.stream_q
        if sq is None:
            # Task not running — tell client to stop reconnecting (retry:0)
            yield "retry: 0\n"
            yield f"data: {json.dumps({'type':'idle'})}\n\n"
            return
        while True:
            try:
                item = sq.get(timeout=25)
                yield f"data: {json.dumps(item)}\n\n"
                if item.get('type') in ('done', 'error'):
                    # Send retry:0 so EventSource won't auto-reconnect
                    yield "retry: 0\n"
                    break
            except q_mod.Empty:
                # keepalive ping
                yield f"data: {json.dumps({'type':'ping'})}\n\n"

    return Response(
        stream_with_context(generate()),
        content_type='text/event-stream',
        headers={'Cache-Control': 'no-cache', 'X-Accel-Buffering': 'no'},
    )

@app.route('/api/users', methods=['GET'])
def api_users():
    users = []
    with _state_lock:
        for uid, st in _states.items():
            busy = st.proc is not None and st.proc.poll() is None
            users.append({'id': uid, 'context_turns': context_count(uid), 'busy': busy})
    return jsonify({'users': users})

@app.route('/health', methods=['GET'])
def health():
    active = sum(1 for st in _states.values()
                 if st.proc and st.proc.poll() is None)
    return jsonify({
        'status': 'ok',
        'telegram': bool(TELEGRAM_TOKEN),
        'line': bool(LINE_CHANNEL_ACCESS_TOKEN),
        'agent': os.path.exists(AGENT_PATH),
        'active_tasks': active,
        'tracked_users': len(_states),
        'db': DB_PATH,
        'web_chat': bool(WEB_TOKEN),
    }), 200

DASHBOARD_HTML = r'''<!DOCTYPE html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>agent-bot</title>
<style>
:root {
  --bg: #090c10; --surface: #0d1117; --card: #161b22; --border: #21262d;
  --text: #c9d1d9; --dim: #8b949e; --dim2: #484f58;
  --blue: #58a6ff; --green: #3fb950; --red: #f85149; --yellow: #e3b341;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
html, body { height: 100%; }
body {
  background: var(--bg); color: var(--text);
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  display: flex; flex-direction: column; height: 100vh; overflow: hidden;
}
header {
  display: flex; align-items: center; gap: 1rem;
  padding: .75rem 1.25rem; border-bottom: 1px solid var(--border);
  background: var(--surface); flex-shrink: 0;
}
header h1 { font-size: 1rem; font-weight: 600; color: var(--text); letter-spacing: -.01em; }
header h1 span { color: var(--blue); }
.dot { display: inline-block; width: 7px; height: 7px; border-radius: 50%; }
.dot.g { background: var(--green); box-shadow: 0 0 6px var(--green); }
.dot.r { background: var(--red); }
nav { display: flex; border-bottom: 1px solid var(--border); background: var(--surface); flex-shrink: 0; }
nav button {
  background: none; border: none; border-bottom: 2px solid transparent;
  color: var(--dim); font-size: .85rem; padding: .6rem 1.2rem;
  cursor: pointer; transition: color .15s, border-color .15s;
}
nav button.active { color: var(--text); border-bottom-color: var(--blue); }
nav button:hover:not(.active) { color: var(--text); }
.panel { flex: 1; overflow: hidden; display: none; flex-direction: column; }
.panel.active { display: flex; }
#chat-output {
  flex: 1; overflow-y: auto; padding: 1rem 1.25rem;
  font-family: 'SF Mono', 'Fira Code', Monaco, Consolas, monospace;
  font-size: .82rem; line-height: 1.6; background: var(--bg);
  white-space: pre-wrap; word-break: break-all;
  scrollbar-width: thin; scrollbar-color: var(--border) transparent;
}
#chat-output::-webkit-scrollbar { width: 4px; }
#chat-output::-webkit-scrollbar-thumb { background: var(--border); border-radius: 2px; }
.msg-user { color: var(--blue); margin-top: .75rem; }
.msg-agent { color: var(--text); }
.msg-system { color: var(--dim); font-style: italic; }
.msg-error { color: var(--red); }
.msg-user::before { content: '❯ '; color: var(--blue); }
#chat-bar {
  display: flex; align-items: center; gap: .6rem;
  padding: .75rem 1rem; border-top: 1px solid var(--border);
  background: var(--surface); flex-shrink: 0;
}
#chat-input {
  flex: 1; background: var(--card); border: 1px solid var(--border);
  border-radius: 8px; color: var(--text); padding: .55rem .9rem;
  font-size: .9rem; font-family: inherit; outline: none; transition: border-color .15s;
}
#chat-input:focus { border-color: var(--blue); }
#chat-input::placeholder { color: var(--dim2); }
#chat-send {
  background: var(--blue); color: #000; border: none; border-radius: 8px;
  padding: .55rem 1.1rem; font-size: .85rem; font-weight: 600;
  cursor: pointer; transition: opacity .15s; white-space: nowrap;
}
#chat-send:disabled { opacity: .4; cursor: not-allowed; }
#chat-task-status { font-size: .75rem; color: var(--dim); min-width: 60px; text-align: right; }
#token-row {
  display: flex; align-items: center; gap: .5rem;
  padding: .4rem 1rem; border-top: 1px solid var(--border);
  background: var(--card); font-size: .75rem; color: var(--dim2); flex-shrink: 0;
}
#chat-token {
  background: var(--surface); border: 1px solid var(--border); border-radius: 5px;
  color: var(--dim); padding: .2rem .5rem; font-size: .75rem; width: 200px; outline: none;
}
#status-panel { overflow-y: auto; padding: 1.25rem; scrollbar-width: thin; scrollbar-color: var(--border) transparent; }
.s-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: .75rem; margin-bottom: 1.25rem; }
.s-card { background: var(--card); border: 1px solid var(--border); border-radius: 8px; padding: .9rem 1rem; }
.s-label { font-size: .68rem; text-transform: uppercase; letter-spacing: .07em; color: var(--dim); margin-bottom: .3rem; }
.s-value { font-size: 1.7rem; font-weight: 700; line-height: 1; }
.s-sub { font-size: .73rem; color: var(--dim); margin-top: .2rem; }
.s-section { font-size: .72rem; font-weight: 600; text-transform: uppercase; letter-spacing: .07em; color: var(--dim); margin: 1rem 0 .5rem; }
.s-row { display: flex; align-items: center; gap: .6rem; padding: .5rem 0; border-bottom: 1px solid var(--border); font-size: .85rem; }
.s-row:last-child { border-bottom: none; }
.s-row .s-name { flex: 1; }
.badge { font-size: .67rem; padding: .15rem .45rem; border-radius: 8px; font-weight: 600; }
.badge.on  { background: rgba(63,185,80,.12); color: var(--green); border: 1px solid rgba(63,185,80,.25); }
.badge.off { background: rgba(248,81,73,.08); color: var(--red);   border: 1px solid rgba(248,81,73,.2); }
.badge.busy{ background: rgba(227,179,65,.12); color: var(--yellow); border: 1px solid rgba(227,179,65,.25); }
.s-table { width: 100%; border-collapse: collapse; font-size: .82rem; }
.s-table th { text-align: left; padding: .4rem .6rem; color: var(--dim); font-weight: 500; border-bottom: 1px solid var(--border); }
.s-table td { padding: .4rem .6rem; border-bottom: 1px solid var(--card); font-family: monospace; }
.s-table tr:last-child td { border-bottom: none; }
.s-refresh { font-size: .7rem; color: var(--dim2); text-align: right; margin-top: 1rem; }
@keyframes pulse { 0%,100%{opacity:1} 50%{opacity:.35} }
.pulse { animation: pulse 2s infinite; }
</style>
</head>
<body>

<header>
  <span class="dot g" id="hdr-dot"></span>
  <h1>agent<span>-bot</span></h1>
  <span id="hdr-status" style="color:var(--dim);font-size:.78rem">読み込み中...</span>
  <div style="flex:1"></div>
  <span id="hdr-tasks" style="font-size:.75rem;color:var(--dim)"></span>
</header>

<nav>
  <button class="active" onclick="switchTab('chat',this)">Chat</button>
  <button onclick="switchTab('status',this)">Status</button>
</nav>

<div class="panel active" id="tab-chat">
  <div id="chat-output"></div>
  <div id="chat-bar">
    <input id="chat-input" type="text" placeholder="タスクを入力… (Enter で送信)" autocomplete="off">
    <span id="chat-task-status"></span>
    <button id="chat-send">送信</button>
  </div>
  <div id="token-row">
    <span>Token</span>
    <input id="chat-token" type="password" placeholder="WEB_TOKEN を入力">
    <button onclick="clearChat()" style="margin-left:auto;background:none;border:1px solid var(--border);color:var(--dim);border-radius:5px;padding:.15rem .5rem;font-size:.72rem;cursor:pointer">クリア</button>
  </div>
</div>

<div class="panel" id="tab-status">
  <div id="status-panel">
    <div class="s-grid">
      <div class="s-card"><div class="s-label">Status</div><div class="s-value" id="s-status" style="font-size:1.1rem;margin-top:.2rem">—</div><div class="s-sub" id="s-time">—</div></div>
      <div class="s-card"><div class="s-label">Active tasks</div><div class="s-value" id="s-tasks">—</div><div class="s-sub">実行中</div></div>
      <div class="s-card"><div class="s-label">Users</div><div class="s-value" id="s-users">—</div><div class="s-sub">セッション保持</div></div>
      <div class="s-card"><div class="s-label">Rate limit</div><div class="s-value" style="font-size:1.3rem">10</div><div class="s-sub">req / 60s</div></div>
    </div>
    <div class="s-section">Services</div>
    <div class="s-card" style="padding:.5rem .9rem">
      <div class="s-row"><span class="dot" id="d-agent"></span><span class="s-name">agent.v10</span><span class="badge" id="b-agent">—</span></div>
      <div class="s-row"><span class="dot" id="d-tg"></span><span class="s-name">Telegram</span><span class="badge" id="b-tg">—</span></div>
      <div class="s-row"><span class="dot" id="d-line"></span><span class="s-name">LINE webhook</span><span class="badge" id="b-line">—</span></div>
      <div class="s-row"><span class="dot g"></span><span class="s-name">SQLite</span><span class="badge on" id="b-db" style="font-family:monospace;font-size:.65rem">—</span></div>
    </div>
    <div class="s-section" style="margin-top:1.1rem">Active sessions</div>
    <div class="s-card" style="padding:.4rem .6rem">
      <table class="s-table">
        <thead><tr><th>User</th><th>Context</th><th>State</th></tr></thead>
        <tbody id="users-tbody"><tr><td colspan="3" style="color:var(--dim);padding:.5rem .6rem">アイドル</td></tr></tbody>
      </table>
    </div>
    <div class="s-refresh"><span class="pulse">●</span> <span id="nxt">10</span>s 後に更新</div>
  </div>
</div>

<script>
function switchTab(name, btn) {
  document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('nav button').forEach(b => b.classList.remove('active'));
  document.getElementById('tab-' + name).classList.add('active');
  btn.classList.add('active');
  if (name === 'chat') document.getElementById('chat-input').focus();
}

const tok = document.getElementById('chat-token');
tok.value = localStorage.getItem('wt') || '';
tok.oninput = () => localStorage.setItem('wt', tok.value);

document.getElementById('chat-input').addEventListener('keydown', e => {
  if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); sendChat(); }
});
document.getElementById('chat-send').addEventListener('click', sendChat);

function clearChat() {
  document.getElementById('chat-output').innerHTML = '';
}

function badge(el, dotEl, on) {
  if (el) { el.textContent = on ? 'ON' : 'OFF'; el.className = 'badge ' + (on ? 'on' : 'off'); }
  if (dotEl) dotEl.className = 'dot ' + (on ? 'g' : 'r');
}

async function refresh() {
  try {
    const [h, u] = await Promise.all([
      fetch('/health').then(r => r.json()),
      fetch('/api/users').then(r => r.json()).catch(() => ({users:[]})),
    ]);
    const ok = h.status === 'ok';
    document.getElementById('hdr-dot').className = 'dot ' + (ok ? 'g' : 'r');
    document.getElementById('hdr-status').textContent = ok ? location.hostname : 'error';
    document.getElementById('hdr-tasks').textContent = h.active_tasks > 0 ? `${h.active_tasks} task running` : '';
    document.getElementById('s-status').textContent = ok ? '✓ OK' : '✗ ERR';
    document.getElementById('s-status').style.color = ok ? 'var(--green)' : 'var(--red)';
    document.getElementById('s-time').textContent = new Date().toLocaleTimeString('ja-JP');
    document.getElementById('s-tasks').textContent = h.active_tasks ?? 0;
    document.getElementById('s-users').textContent = h.tracked_users ?? 0;
    badge(document.getElementById('b-agent'), document.getElementById('d-agent'), h.agent);
    badge(document.getElementById('b-tg'),    document.getElementById('d-tg'),    h.telegram);
    badge(document.getElementById('b-line'),  document.getElementById('d-line'),  h.line);
    const bdb = document.getElementById('b-db');
    if (bdb) bdb.textContent = (h.db||'').replace('/data/','vol:').replace('/tmp/','tmp:');
    const users = (u.users || []);
    document.getElementById('users-tbody').innerHTML = users.length === 0
      ? '<tr><td colspan="3" style="color:var(--dim);padding:.5rem .6rem">アイドル</td></tr>'
      : users.map(u => `<tr><td>${u.id}</td><td>${u.context_turns}</td><td>${u.busy ? '<span class="badge busy">実行中</span>' : '—'}</td></tr>`).join('');
  } catch(_) {
    document.getElementById('hdr-dot').className = 'dot r';
  }
}

let taskRunning = false;

function appendMsg(text, cls) {
  const out = document.getElementById('chat-output');
  const el = document.createElement('div');
  el.className = 'msg-' + cls;
  el.textContent = text;
  out.appendChild(el);
  out.scrollTop = out.scrollHeight;
}

function setStatus(text, fade) {
  const el = document.getElementById('chat-task-status');
  el.textContent = text;
  if (fade) setTimeout(() => { el.textContent = ''; }, 3000);
}

function finish(ok) {
  taskRunning = false;
  document.getElementById('chat-send').disabled = false;
  setStatus(ok ? '✅ 完了' : '❌ エラー', true);
  refresh();
}

async function sendChat() {
  if (taskRunning) return;
  const btn = document.getElementById('chat-send');
  if (btn.disabled) return;
  const inp   = document.getElementById('chat-input');
  const task  = inp.value.trim();
  const token = tok.value.trim();
  if (!task)  return;
  if (!token) { appendMsg('⚠ Token を入力してください', 'error'); return; }

  taskRunning = true;
  btn.disabled = true;
  inp.value = '';
  appendMsg(task, 'user');
  setStatus('⏳ 実行中…', false);

  try {
    const r = await fetch('/api/chat', {
      method: 'POST',
      headers: {'Content-Type': 'application/json', 'X-Web-Token': token},
      body: JSON.stringify({task}),
    });
    if (!r.ok) { appendMsg('❌ ' + (await r.text()), 'error'); finish(false); return; }
  } catch(e) { appendMsg('❌ 接続エラー: ' + e, 'error'); finish(false); return; }

  try {
    const resp = await fetch('/api/chat/stream?token=' + encodeURIComponent(token));
    if (!resp.ok) { appendMsg('❌ Stream error', 'error'); finish(false); return; }
    const reader = resp.body.getReader();
    const dec = new TextDecoder();
    let buf = '';
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      buf += dec.decode(value, { stream: true });
      const parts = buf.split('\n\n');
      buf = parts.pop();
      for (const part of parts) {
        for (const line of part.split('\n')) {
          if (!line.startsWith('data: ')) continue;
          try {
            const d = JSON.parse(line.slice(6));
            if (d.type === 'line') {
              appendMsg(d.text.replace(/\n$/, ''), 'agent');
            } else if (d.type === 'done') {
              appendMsg('─ 完了 ─', 'system');
              reader.cancel(); finish(true); return;
            } else if (d.type === 'error') {
              appendMsg('❌ ' + d.text, 'error');
              reader.cancel(); finish(false); return;
            } else if (d.type === 'idle') {
              reader.cancel(); finish(true); return;
            }
          } catch(_) {}
        }
      }
    }
    finish(true);
  } catch(e) {
    appendMsg('❌ Stream エラー: ' + e, 'error'); finish(false);
  }
}

let countdown = 10;
setInterval(() => {
  if (--countdown <= 0) { countdown = 10; refresh(); }
  const el = document.getElementById('nxt');
  if (el) el.textContent = countdown;
}, 1000);
refresh();
</script>
</body>
</html>'''

@app.route('/', methods=['GET'])
def dashboard():
    return DASHBOARD_HTML, 200, {'Content-Type': 'text/html; charset=utf-8'}

# ─── Main ──────────────────────────────────────────────────────────────────

if __name__ == '__main__':
    init_db()
    print(f'[bot] DB:       {DB_PATH}')
    print(f'[bot] Telegram: {"on" if TELEGRAM_TOKEN else "off"}')
    print(f'[bot] LINE:     {"on" if LINE_CHANNEL_ACCESS_TOKEN else "off"}')
    print(f'[bot] Web chat: {"on (WEB_TOKEN set)" if WEB_TOKEN else "off (set WEB_TOKEN to enable)"}')
    print(f'[bot] Agent:    {AGENT_PATH} ({"found" if os.path.exists(AGENT_PATH) else "MISSING"})')
    print(f'[bot] Auth:     {_auth_raw}')

    if TELEGRAM_TOKEN:
        threading.Thread(target=tg_poll, daemon=True).start()

    app.run(host='0.0.0.0', port=PORT, threaded=True)

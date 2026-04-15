#!/usr/bin/env python3
"""
mini-agent-c web — streams agent output via SSE.
Features: auto-detect latest binary, /evolve endpoint, auto-evolve scheduler,
          concurrency control, and automatic resource-based emergency stop.
"""
import http.server
import socketserver
import json
import subprocess
import os
import re
import select
import sys
import threading
import time
import urllib.parse
import urllib.request
import urllib.error
import resource
from pathlib import Path

HERE = Path(__file__).parent.resolve()
REPO = HERE.parent
INDEX_HTML = HERE / "index.html"
HISTORY_FILE = REPO / ".agent" / "web_history.jsonl"
EVAL_HISTORY  = REPO / ".agent" / "eval_history.jsonl"
STOP_FILE = REPO / ".agent" / "STOP"

AUTH_TOKEN = os.environ.get("MINI_AGENT_WEB_TOKEN", "")
AUTO_EVOLVE_HOURS = float(os.environ.get("AUTO_EVOLVE_HOURS", "0"))  # 0 = off
MLX_HOST = os.environ.get("MLX_HOST", "127.0.0.1")
MLX_PORT = int(os.environ.get("MLX_PORT", "5001"))
MLX_BASE = f"http://{MLX_HOST}:{MLX_PORT}"

_active: dict[int, subprocess.Popen] = {}   # pid → proc
_active_meta: dict[int, dict] = {}           # pid → {started, task_summary}
_active_lock = threading.Lock()

MAX_CONCURRENT = int(os.environ.get("MAX_CONCURRENT", "2"))

# ── resource thresholds ─────────────────────────────────────────────────────
CPU_KILL_PCT    = float(os.environ.get("CPU_KILL_PCT",    "85"))   # kill oldest agent
CPU_REFUSE_PCT  = float(os.environ.get("CPU_REFUSE_PCT",  "70"))   # reject new requests
MEM_KILL_PCT    = float(os.environ.get("MEM_KILL_PCT",    "90"))   # kill oldest agent


def _get_cpu_pct() -> float:
    """Return system CPU usage % (1-second sample via /proc or sysctl)."""
    try:
        # macOS: use top -l2 delta
        out = subprocess.check_output(
            ["top", "-l", "2", "-n", "0", "-s", "1"],
            stderr=subprocess.DEVNULL, timeout=4
        ).decode()
        for line in reversed(out.splitlines()):
            if "CPU usage" in line:
                # "CPU usage: 12.5% user, 8.3% sys, 79.1% idle"
                idle = float(re.search(r"([\d.]+)%\s+idle", line).group(1))
                return 100.0 - idle
    except Exception:
        pass
    return 0.0


def _get_mem_pct() -> float:
    """Return memory usage % — wired+active / (wired+active+inactive+free)."""
    try:
        out = subprocess.check_output(["vm_stat"], stderr=subprocess.DEVNULL).decode()
        stats = {}
        for line in out.splitlines():
            m = re.match(r"Pages\s+(.+?):\s+(\d+)", line)
            if m:
                stats[m.group(1).strip().lower()] = int(m.group(2))
        wired    = stats.get("wired down", 0)
        active   = stats.get("active", 0)
        inactive = stats.get("inactive", 0)
        free     = stats.get("free", 0) + stats.get("speculative", 0)
        used     = wired + active
        total    = used + inactive + free
        if total > 0:
            return 100.0 * used / total
    except Exception:
        pass
    return 0.0


def _kill_oldest_agent(reason: str):
    """Terminate the oldest running agent process and log the reason."""
    with _active_lock:
        if not _active:
            return
        oldest_pid = min(_active_meta, key=lambda p: _active_meta[p]["started"], default=None)
        if oldest_pid and oldest_pid in _active:
            proc = _active[oldest_pid]
            print(f"[resource-guard] killing PID {oldest_pid}: {reason}", flush=True)
            try:
                proc.terminate()
            except Exception:
                pass


def _resource_guard():
    """Background thread: monitor CPU/memory and kill agents if critical."""
    while True:
        time.sleep(10)
        try:
            with _active_lock:
                n = len(_active)
            if n == 0:
                continue
            cpu = _get_cpu_pct()
            mem = _get_mem_pct()
            if cpu > CPU_KILL_PCT:
                _kill_oldest_agent(f"CPU {cpu:.0f}% > {CPU_KILL_PCT}%")
            elif mem > MEM_KILL_PCT:
                _kill_oldest_agent(f"MEM {mem:.0f}% > {MEM_KILL_PCT}%")
        except Exception:
            pass


threading.Thread(target=_resource_guard, daemon=True, name="resource-guard").start()


def load_env_file() -> dict:
    """Load all KEY=VALUE pairs from ~/.env into a dict."""
    env_file = Path.home() / ".env"
    result = {}
    if not env_file.exists():
        return result
    for line in env_file.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" in line:
            k, v = line.split("=", 1)
            k = k.strip()
            v = v.strip().strip('"').strip("'")
            if k:
                result[k] = v
    return result


def agent_env() -> dict:
    """Build environment for agent subprocess: os.environ + all ~/.env keys."""
    env = os.environ.copy()
    for k, v in load_env_file().items():
        if k not in env:  # don't override existing env vars
            env[k] = v
    return env


# ── binary detection ────────────────────────────────────────────────────────

def find_latest_binary():
    """Return the highest agent.vN binary that is executable."""
    best = None
    for p in REPO.glob("agent.v*"):
        m = re.match(r"agent\.v(\d+)$", p.name)
        if m and p.is_file() and os.access(p, os.X_OK):
            n = int(m.group(1))
            if best is None or n > best[0]:
                best = (n, p)
    return best[1] if best else REPO / "agent.v6"

def find_latest_src_version():
    best = 0
    for p in REPO.glob("agent.v*.c"):
        m = re.match(r"agent\.v(\d+)\.c$", p.name)
        if m:
            best = max(best, int(m.group(1)))
    return best

def get_binary_version(binary: Path):
    m = re.match(r"agent\.v(\d+)$", binary.name)
    return int(m.group(1)) if m else 0


# ── self-evolve ─────────────────────────────────────────────────────────────

EVOLVE_MODEL = os.environ.get("EVOLVE_MODEL", "mlx-community/Qwen3.5-27B-4bit")
EVOLVE_API_BASE = os.environ.get("EVOLVE_API_BASE", "http://127.0.0.1:5001")

def build_evolve_task(src_v: int, tgt_v: int) -> str:
    return f"""Self-improvement task: read agent.v{src_v}.c and create agent.v{tgt_v}.c with improvements.

Steps:
1. Read agent.v{src_v}.c using read_file (it is your own source)
2. Pick 1-2 concrete improvements:
   - Add a useful new tool (e.g. http_get, grep_files, diff_files)
   - Improve retry/error handling
   - Reduce token waste in prompts
   - Fix any edge cases you notice
3. Write the improved code to agent.v{tgt_v}.c
   Add at the top: /* v{tgt_v}: <one-line summary of changes> */
4. Compile with bash:
   cc -O2 -Wall -Wno-unused-result -Wno-comment -std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -o agent.v{tgt_v} agent.v{tgt_v}.c cJSON.c -lcurl -lm
5. Run eval with bash: ./eval.sh ./agent.v{tgt_v}
6. Report: what changed, compile result, eval score/MAX_SCORE

Working directory: {REPO}"""

def run_evolve_stream(send_event, send_end):
    """Run a self-evolution cycle, streaming SSE events."""
    binary = find_latest_binary()
    src_v  = get_binary_version(binary)
    tgt_v  = find_latest_src_version() + 1

    # If compiled binary is behind source, try compiling existing source first
    latest_src = find_latest_src_version()
    if latest_src > src_v:
        tgt_v = latest_src + 1

    send_event("evolve_start", {"src_v": src_v, "tgt_v": tgt_v})

    task = build_evolve_task(max(src_v, latest_src), tgt_v)
    args = [
        str(binary),
        "--model", EVOLVE_MODEL,
        "--budget", "80000",
        "--max-turns", "20",
        "--backend", "openai",
        "--api-base", EVOLVE_API_BASE,
        task,
    ]

    env = agent_env()

    start_ts = time.time()
    try:
        proc = subprocess.Popen(
            args,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=str(REPO),
            env=env,
            bufsize=0,
        )
    except Exception as e:
        send_event("error", {"message": f"spawn failed: {e}"})
        send_end()
        return

    with _active_lock:
        _active[proc.pid] = proc
        _active_meta[proc.pid] = {"started": start_ts, "task_summary": f"evolve v{src_v}→v{tgt_v}"}

    send_event("start", {"pid": proc.pid, "concurrent": len(_active)})

    import fcntl
    for f in (proc.stdout, proc.stderr):
        fd = f.fileno()
        flags = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    stderr_buf = b""
    try:
        while True:
            rlist, _, _ = select.select([proc.stdout, proc.stderr], [], [], 0.2)
            if proc.stdout in rlist:
                try:
                    chunk = os.read(proc.stdout.fileno(), 4096)
                    if chunk:
                        send_event("text", {"content": chunk.decode("utf-8", errors="replace")})
                except BlockingIOError:
                    pass
            if proc.stderr in rlist:
                try:
                    chunk = os.read(proc.stderr.fileno(), 4096)
                    if chunk:
                        stderr_buf += chunk
                        while b"\n" in stderr_buf:
                            line, stderr_buf = stderr_buf.split(b"\n", 1)
                            send_event("log", {"line": line.decode("utf-8", errors="replace")})
                except BlockingIOError:
                    pass
            if proc.poll() is not None:
                for f in (proc.stdout, proc.stderr):
                    try:
                        fd = f.fileno()
                        flags = fcntl.fcntl(fd, fcntl.F_GETFL)
                        fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
                    except Exception:
                        pass
                try:
                    rem = proc.stdout.read()
                    if rem:
                        send_event("text", {"content": rem.decode("utf-8", errors="replace")})
                except Exception:
                    pass
                try:
                    rem = proc.stderr.read()
                    if rem:
                        for line in rem.decode("utf-8", errors="replace").splitlines():
                            send_event("log", {"line": line})
                except Exception:
                    pass
                break
    except Exception as e:
        send_event("error", {"message": str(e)})
    finally:
        with _active_lock:
            _active.pop(proc.pid, None)
            _active_meta.pop(proc.pid, None)

    duration = round(time.time() - start_ts, 2)

    # Check if new binary was produced
    new_binary = REPO / f"agent.v{tgt_v}"
    success = new_binary.exists() and os.access(new_binary, os.X_OK)

    # Parse eval score from output (best effort)
    eval_score = None
    if success:
        try:
            result = subprocess.run(
                ["./eval.sh", f"./agent.v{tgt_v}"],
                capture_output=True, text=True, cwd=str(REPO), timeout=120
            )
            m = re.search(r"score=(\d+)/(\d+)", result.stdout + result.stderr)
            if m:
                eval_score = {"score": int(m.group(1)), "max": int(m.group(2))}
        except Exception:
            pass

    # Record evolution history
    try:
        EVAL_HISTORY.parent.mkdir(parents=True, exist_ok=True)
        with EVAL_HISTORY.open("a") as f:
            f.write(json.dumps({
                "ts": time.time(),
                "src_v": src_v,
                "tgt_v": tgt_v,
                "success": success,
                "eval": eval_score,
                "duration": duration,
            }) + "\n")
    except Exception:
        pass

    send_event("done", {
        "exit_code": proc.returncode,
        "duration_sec": duration,
        "tgt_v": tgt_v,
        "success": success,
        "eval": eval_score,
    })
    send_end()


# ── auto-evolve scheduler ────────────────────────────────────────────────────

def _auto_evolve_worker():
    """Background thread: runs evolution every AUTO_EVOLVE_HOURS hours."""
    interval = AUTO_EVOLVE_HOURS * 3600
    print(f"[evolve] auto-evolve every {AUTO_EVOLVE_HOURS}h", file=sys.stderr)
    while True:
        time.sleep(interval)
        try:
            print("[evolve] starting scheduled evolution...", file=sys.stderr)
            run_evolve_stream(
                lambda e, d: print(f"[evolve] {e}: {d}", file=sys.stderr),
                lambda: None,
            )
        except Exception as e:
            print(f"[evolve] error: {e}", file=sys.stderr)


# ── HTTP handler ─────────────────────────────────────────────────────────────

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        pass

    def _auth_ok(self):
        if not AUTH_TOKEN:
            return True
        return self.headers.get("Authorization", "") == f"Bearer {AUTH_TOKEN}"

    def _reject_if_unauth(self):
        if not self._auth_ok():
            data = b'{"error":"unauthorized"}'
            self.send_response(401)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return True
        return False

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path in ("/", "/index.html"):
            self._serve_file(INDEX_HTML, "text/html; charset=utf-8")
        elif path == "/health":
            binary = find_latest_binary()
            self._json({"ok": True, "binary": binary.name, "exists": binary.exists()})
        elif path == "/manifest.json":
            self._json({
                "name": "mini-agent-c",
                "short_name": "mini-agent",
                "description": "Autonomous self-evolving agent",
                "start_url": "/",
                "display": "standalone",
                "background_color": "#0d0d0f",
                "theme_color": "#0d0d0f",
                "orientation": "portrait",
                "icons": [
                    {"src": "data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><rect width='100' height='100' rx='20' fill='%230d0d0f'/><text y='.9em' font-size='80'>🤖</text></svg>", "sizes": "any", "type": "image/svg+xml"}
                ]
            })
        elif path == "/status":
            if self._reject_if_unauth(): return
            with _active_lock:
                agents = [{"pid": pid, "task": _active_meta.get(pid, {}).get("task_summary", ""),
                           "age_s": round(time.time() - _active_meta.get(pid, {}).get("started", time.time()), 1)}
                          for pid in _active]
            cpu = _get_cpu_pct()
            mem = _get_mem_pct()
            self._json({
                "agents": agents,
                "n_active": len(agents),
                "max_concurrent": MAX_CONCURRENT,
                "cpu_pct": round(cpu, 1),
                "mem_pct": round(mem, 1),
                "cpu_kill_at": CPU_KILL_PCT,
                "mem_kill_at": MEM_KILL_PCT,
            })
        elif path == "/version":
            if self._reject_if_unauth(): return
            self._serve_version()
        elif path == "/history":
            if self._reject_if_unauth(): return
            self._serve_history()
        elif path == "/tools":
            if self._reject_if_unauth(): return
            self._serve_tools()
        elif path == "/speak":
            if self._reject_if_unauth(): return
            self._handle_speak_get()
        elif path.startswith("/mlx/"):
            if self._reject_if_unauth(): return
            self._proxy_mlx("GET", path)
        else:
            self.send_error(404)

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/run":
            if self._reject_if_unauth(): return
            self._handle_run()
        elif path == "/evolve":
            if self._reject_if_unauth(): return
            self._handle_evolve()
        elif path == "/stop":
            if self._reject_if_unauth(): return
            self._handle_stop()
        elif path == "/speak":
            if self._reject_if_unauth(): return
            self._handle_speak_post()
        elif path.startswith("/mlx/"):
            if self._reject_if_unauth(): return
            self._proxy_mlx("POST", path)
        else:
            self.send_error(404)

    # ── responses ──────────────────────────────────────────────────────────

    def _serve_file(self, path, ctype):
        try:
            data = path.read_bytes()
        except FileNotFoundError:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)

    def _json(self, obj, status=200):
        data = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _serve_version(self):
        binary  = find_latest_binary()
        bin_v   = get_binary_version(binary)
        src_v   = find_latest_src_version()
        last_eval = None
        if EVAL_HISTORY.exists():
            for line in reversed(EVAL_HISTORY.read_text().splitlines()):
                try:
                    last_eval = json.loads(line)
                    break
                except Exception:
                    pass
        self._json({
            "binary": binary.name,
            "binary_v": bin_v,
            "src_v": src_v,
            "last_eval": last_eval,
        })

    def _serve_history(self):
        entries = []
        if HISTORY_FILE.exists():
            for l in HISTORY_FILE.read_text().splitlines()[-50:]:
                try: entries.append(json.loads(l))
                except: pass
        self._json({"history": entries})

    def _serve_tools(self):
        tools_dir = REPO / ".agent" / "tools"
        builtin = ["read_file", "write_file", "bash", "list_dir",
                   "save_memory", "recall_memory", "spawn_agent"]
        dynamic = []
        if tools_dir.exists():
            for f in sorted(tools_dir.glob("*.sh")):
                name, desc = f.stem, ""
                try:
                    for line in f.read_text().splitlines()[:30]:
                        if line.startswith("#@name:"): name = line.split(":",1)[1].strip()
                        elif line.startswith("#@description:"): desc = line.split(":",1)[1].strip()
                except: pass
                dynamic.append({"name": name, "description": desc})
        self._json({"builtin": builtin, "dynamic": dynamic})

    # ── MLX proxy ──────────────────────────────────────────────────────────

    def _proxy_mlx(self, method: str, path: str):
        """Reverse-proxy /mlx/<rest> → MLX_BASE/<rest>, streaming-aware."""
        rest = path[len("/mlx"):]  # e.g. /v1/chat/completions
        target = MLX_BASE + rest
        qs = urllib.parse.urlparse(self.path).query
        if qs:
            target += "?" + qs

        body = None
        content_type = self.headers.get("Content-Type", "")
        if method == "POST":
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length) if length else b""

        req = urllib.request.Request(target, data=body, method=method)
        if content_type:
            req.add_header("Content-Type", content_type)

        is_stream = b'"stream": true' in (body or b"") or b'"stream":true' in (body or b"")
        try:
            with urllib.request.urlopen(req, timeout=600) as resp:
                resp_ct = resp.headers.get("Content-Type", "application/json")
                if is_stream or "event-stream" in resp_ct:
                    # streaming: chunked passthrough
                    self.send_response(resp.status)
                    self.send_header("Content-Type", resp_ct)
                    self.send_header("Access-Control-Allow-Origin", "*")
                    self.send_header("Transfer-Encoding", "chunked")
                    self.send_header("Cache-Control", "no-cache")
                    self.end_headers()
                    while True:
                        chunk = resp.read(4096)
                        if not chunk:
                            break
                        try:
                            self.wfile.write(chunk)
                            self.wfile.flush()
                        except BrokenPipeError:
                            break
                else:
                    # non-streaming: buffer and send with Content-Length
                    data = resp.read()
                    self.send_response(resp.status)
                    self.send_header("Content-Type", resp_ct)
                    self.send_header("Content-Length", str(len(data)))
                    self.send_header("Access-Control-Allow-Origin", "*")
                    self.end_headers()
                    self.wfile.write(data)
        except urllib.error.URLError as e:
            self._json({"error": f"MLX proxy error: {e.reason}"}, status=502)
        except Exception as e:
            self._json({"error": str(e)}, status=502)

    # ── TTS ────────────────────────────────────────────────────────────────

    def _get_speak_text(self):
        qs = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        return qs["text"][0][:800] if "text" in qs else None

    def _handle_speak_get(self):
        text = self._get_speak_text()
        if not text:
            self.send_error(400, "text param required")
            return
        self._synthesize_and_send(text)

    def _handle_speak_post(self):
        length = int(self.headers.get("Content-Length", 0))
        try:
            body = json.loads(self.rfile.read(length))
            text  = body.get("text", "")[:800]
            voice = body.get("voice", "ja-JP-NanamiNeural")
            rate  = body.get("rate", "+10%")
        except Exception:
            self.send_error(400, "bad JSON")
            return
        self._synthesize_and_send(text, voice=voice, rate=rate)

    def _synthesize_and_send(self, text, voice="ja-JP-NanamiNeural", rate="+10%"):
        import asyncio, tempfile, edge_tts
        if not text.strip():
            self.send_error(400, "empty text")
            return
        tmp = tempfile.NamedTemporaryFile(suffix=".mp3", delete=False)
        tmp_path = tmp.name
        tmp.close()
        try:
            loop = asyncio.new_event_loop()
            async def gen():
                comm = edge_tts.Communicate(text, voice, rate=rate)
                await comm.save(tmp_path)
            loop.run_until_complete(gen())
            loop.close()
            data = open(tmp_path, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(data)
        except Exception as e:
            self.send_error(500, str(e))
        finally:
            try: os.unlink(tmp_path)
            except: pass

    # ── SSE helpers ────────────────────────────────────────────────────────

    def _send_sse_header(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache, no-transform")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

    def _send_sse_event(self, etype, data):
        try:
            body  = f"event: {etype}\ndata: {json.dumps(data, ensure_ascii=False)}\n\n"
            chunk = body.encode("utf-8")
            self.wfile.write(f"{len(chunk):x}\r\n".encode())
            self.wfile.write(chunk)
            self.wfile.write(b"\r\n")
            self.wfile.flush()
        except BrokenPipeError:
            raise
        except Exception as e:
            print(f"[web] sse write err: {e}", file=sys.stderr)

    def _send_sse_end(self):
        try:
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except Exception:
            pass

    # ── /run ───────────────────────────────────────────────────────────────

    def _handle_run(self):
        length = int(self.headers.get("Content-Length", 0))
        try:
            body = json.loads(self.rfile.read(length))
        except Exception:
            self.send_error(400, "bad JSON")
            return

        task = body.get("task", "").strip()
        if not task:
            self.send_error(400, "task required")
            return

        # Concurrency check
        with _active_lock:
            n_active = len(_active)
        if n_active >= MAX_CONCURRENT:
            try:
                self._send_sse_header()
                self._send_sse_event("error", {
                    "message": f"busy: {n_active}/{MAX_CONCURRENT} agents running. "
                               "Wait for one to finish or stop them first."
                })
                self._send_sse_end()
            except Exception:
                pass
            return

        # Resource check before spawning
        cpu = _get_cpu_pct()
        if cpu > CPU_REFUSE_PCT:
            try:
                self._send_sse_header()
                self._send_sse_event("error", {
                    "message": f"system overloaded: CPU {cpu:.0f}% > {CPU_REFUSE_PCT}%. Try again shortly."
                })
                self._send_sse_end()
            except Exception:
                pass
            return

        model     = body.get("model", "mlx-community/Qwen3.5-122B-A10B-4bit")
        budget    = int(body.get("budget", 80000))   # MLX needs more tokens
        max_turns = int(body.get("max_turns", 15))
        sandbox   = bool(body.get("sandbox", False))
        stream    = bool(body.get("stream", True))
        plan      = bool(body.get("plan", False))
        no_memory = bool(body.get("no_memory", False))
        backend   = body.get("backend", "openai")
        api_base  = body.get("api_base", "http://127.0.0.1:5001")
        fallback  = body.get("fallback", True)   # fall back to Haiku if MLX fails

        binary = find_latest_binary()
        args = [str(binary), "--model", model, "--budget", str(budget), "--max-turns", str(max_turns)]
        if stream and backend != "openai": args.append("--stream")
        if sandbox:   args.append("--sandbox")
        if plan:      args.append("--plan")
        if no_memory: args.append("--no-memory")
        if backend == "openai":
            args += ["--backend", "openai", "--api-base", api_base]
            if "127.0.0.1" not in api_base and "localhost" not in api_base:
                args += ["--allow-remote-backend"]
        args.append(task)

        if STOP_FILE.exists():
            STOP_FILE.unlink(missing_ok=True)

        try:
            self._send_sse_header()
        except Exception:
            return

        start_ts = time.time()
        try:
            proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    cwd=str(REPO), env=agent_env(), bufsize=0)
        except Exception as e:
            self._send_sse_event("error", {"message": f"spawn failed: {e}"})
            self._send_sse_end()
            return

        with _active_lock:
            _active[proc.pid] = proc
            _active_meta[proc.pid] = {"started": start_ts, "task_summary": task[:80]}

        self._send_sse_event("start", {"pid": proc.pid, "args": args[1:],
                                        "concurrent": len(_active)})

        try:
            import fcntl
            for f in (proc.stdout, proc.stderr):
                fd = f.fileno()
                fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK)

            stderr_buf = b""
            while True:
                rlist, _, _ = select.select([proc.stdout, proc.stderr], [], [], 0.2)
                if proc.stdout in rlist:
                    try:
                        chunk = os.read(proc.stdout.fileno(), 4096)
                        if chunk:
                            self._send_sse_event("text", {"content": chunk.decode("utf-8", errors="replace")})
                    except BlockingIOError:
                        pass
                if proc.stderr in rlist:
                    try:
                        chunk = os.read(proc.stderr.fileno(), 4096)
                        if chunk:
                            stderr_buf += chunk
                            while b"\n" in stderr_buf:
                                line, stderr_buf = stderr_buf.split(b"\n", 1)
                                self._send_sse_event("log", {"line": line.decode("utf-8", errors="replace")})
                    except BlockingIOError:
                        pass
                if proc.poll() is not None:
                    for f in (proc.stdout, proc.stderr):
                        try:
                            fd = f.fileno()
                            fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) & ~os.O_NONBLOCK)
                        except Exception:
                            pass
                    try:
                        rem = proc.stdout.read()
                        if rem: self._send_sse_event("text", {"content": rem.decode("utf-8", errors="replace")})
                    except Exception: pass
                    try:
                        rem = proc.stderr.read()
                        if rem:
                            for line in rem.decode("utf-8", errors="replace").splitlines():
                                self._send_sse_event("log", {"line": line})
                    except Exception: pass
                    break

            duration = round(time.time() - start_ts, 2)
            exit_code = proc.returncode
            self._send_sse_event("done", {"exit_code": exit_code, "duration_sec": duration,
                                          "model": model, "backend": backend})

            # Fallback to Haiku if MLX failed (exit 2 = budget exceeded, exit 1 = other error)
            if exit_code != 0 and fallback and backend == "openai":
                self._send_sse_event("log", {"line":
                    f"[fallback] {model} failed (exit={exit_code}), retrying with claude-haiku-4-5"})
                with _active_lock:
                    _active.pop(proc.pid, None)
                    _active_meta.pop(proc.pid, None)
                fb_start = time.time()
                fb_args = [str(binary), "--model", "claude-haiku-4-5",
                           "--budget", str(budget), "--max-turns", str(max_turns)]
                if no_memory: fb_args.append("--no-memory")
                fb_args.append(task)
                try:
                    fb_proc = subprocess.Popen(fb_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                               cwd=str(REPO), env=agent_env(), bufsize=0)
                    with _active_lock:
                        _active[fb_proc.pid] = fb_proc
                        _active_meta[fb_proc.pid] = {"started": fb_start, "task_summary": f"[haiku-fb] {task[:60]}"}
                    self._send_sse_event("start", {"pid": fb_proc.pid, "model": "claude-haiku-4-5",
                                                   "fallback": True, "concurrent": len(_active)})
                    import fcntl as _fcntl
                    for f in (fb_proc.stdout, fb_proc.stderr):
                        fl = _fcntl.fcntl(f.fileno(), _fcntl.F_GETFL)
                        _fcntl.fcntl(f.fileno(), _fcntl.F_SETFL, fl | os.O_NONBLOCK)
                    while True:
                        r, _, _ = select.select([fb_proc.stdout, fb_proc.stderr], [], [], 0.1)
                        for s in r:
                            try:
                                chunk = s.read(4096)
                                if chunk:
                                    for line in chunk.decode("utf-8", errors="replace").splitlines():
                                        if s is fb_proc.stdout:
                                            if line.startswith("[text]"):
                                                self._send_sse_event("text", {"content": line[6:]})
                                            else:
                                                self._send_sse_event("log", {"line": line})
                                        else:
                                            self._send_sse_event("log", {"line": line})
                            except Exception:
                                pass
                        if fb_proc.poll() is not None and not r:
                            break
                    fb_dur = round(time.time() - fb_start, 2)
                    self._send_sse_event("done", {"exit_code": fb_proc.returncode,
                                                  "duration_sec": fb_dur, "model": "claude-haiku-4-5",
                                                  "fallback": True})
                    with _active_lock:
                        _active.pop(fb_proc.pid, None)
                        _active_meta.pop(fb_proc.pid, None)
                except Exception as e:
                    self._send_sse_event("error", {"message": f"fallback failed: {e}"})
                return  # skip the finally _active.pop (already done above)

        except BrokenPipeError:
            try: proc.terminate()
            except: pass
        finally:
            with _active_lock:
                _active.pop(proc.pid, None)
                _active_meta.pop(proc.pid, None)
            self._send_sse_end()
            try:
                HISTORY_FILE.parent.mkdir(parents=True, exist_ok=True)
                with HISTORY_FILE.open("a") as f:
                    f.write(json.dumps({
                        "ts": time.time(), "task": task[:1000],
                        "model": model, "backend": backend,
                        "binary": binary.name,
                        "exit": proc.returncode,
                        "duration": round(time.time() - start_ts, 2),
                    }, ensure_ascii=False) + "\n")
            except Exception as e:
                print(f"[web] history err: {e}", file=sys.stderr)

    # ── /evolve ────────────────────────────────────────────────────────────

    def _handle_evolve(self):
        try:
            self._send_sse_header()
        except Exception:
            return
        try:
            run_evolve_stream(self._send_sse_event, self._send_sse_end)
        except BrokenPipeError:
            pass

    # ── /stop ──────────────────────────────────────────────────────────────

    def _handle_stop(self):
        STOP_FILE.parent.mkdir(parents=True, exist_ok=True)
        STOP_FILE.touch()
        killed = []
        with _active_lock:
            for pid, proc in list(_active.items()):
                try: proc.terminate(); killed.append(pid)
                except: pass
        def cleanup():
            time.sleep(2)
            STOP_FILE.unlink(missing_ok=True)
        threading.Thread(target=cleanup, daemon=True).start()
        self._json({"ok": True, "killed": killed})


class ThreadingServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    binary = find_latest_binary()
    if not binary.exists():
        print(f"[web] building {binary.name}...", file=sys.stderr)
        subprocess.run(["make", binary.name], cwd=REPO, check=True)

    port = int(os.environ.get("PORT", "7878"))
    bind = os.environ.get("BIND", "127.0.0.1")
    print(f"[mini-agent-c web] http://{bind}:{port}  binary={binary.name}", file=sys.stderr)

    if AUTO_EVOLVE_HOURS > 0:
        t = threading.Thread(target=_auto_evolve_worker, daemon=True)
        t.start()

    server = ThreadingServer((bind, port), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[web] shutting down", file=sys.stderr)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
voice_agent.py — Mac-local voice-first agent

Architecture:
  Microphone → VAD → Whisper STT → mini-agent-c (MLX 122B) → macOS say TTS
  Telegram bot                   ↗                           → Telegram reply

Requirements (already installed):
  sounddevice, faster_whisper, webrtcvad, numpy

Usage:
  python3 voice_agent.py                 # voice only
  python3 voice_agent.py --telegram      # voice + Telegram

Speak naturally — silence detection ends your utterance.
The agent responds via speech and prints the text.
"""

import sys
import os
import threading
import queue
import time
import json
import subprocess
import struct
import argparse
import asyncio
from pathlib import Path

import numpy as np
import sounddevice as sd
import webrtcvad
import requests
from faster_whisper import WhisperModel

# ── Config ───────────────────────────────────────────────────────────────────
AGENT_URL     = "http://127.0.0.1:7878"
AGENT_TOKEN   = "b9HWZJmh_xoFUHbIHSJuMDT9f4PN7t2Z"
AGENT_MODEL   = "mlx-community/Qwen3.5-122B-A10B-4bit"
AGENT_BACKEND = "openai"
AGENT_API_BASE= "http://127.0.0.1:5001"

WHISPER_MODEL = "small"          # small=fast, medium/large-v3=accurate
SAMPLE_RATE   = 16000
FRAME_MS      = 30               # VAD frame size in ms
FRAME_SAMPLES = int(SAMPLE_RATE * FRAME_MS / 1000)
VAD_MODE      = 1                # 0-3: aggressiveness (1=least, 3=most)
SILENCE_SECS  = 0.7             # seconds of silence to end utterance (snappy)
MIN_SPEECH_SECS = 0.3           # ignore utterances shorter than this
BARGE_IN_FRAMES = 4             # voiced frames needed to trigger barge-in
BARGE_IN_ENERGY = 300           # int16 RMS threshold for barge-in detection

TTS_VOICE     = "ja-JP-NanamiNeural"  # edge-tts neural voice
TTS_RATE      = "+10%"                # edge-tts speed offset

HISTORY_FILE  = Path.home() / ".mini-agent" / "voice_history.jsonl"

# ── Globals ───────────────────────────────────────────────────────────────────
print("🔄 Loading Whisper model...", flush=True)
whisper = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
print(f"✅ Whisper {WHISPER_MODEL} ready", flush=True)

vad = webrtcvad.Vad(VAD_MODE)
audio_q: queue.Queue = queue.Queue()
speaking = threading.Event()
conversation_history: list[dict] = []  # [{role, content}]

# ── Audio capture ─────────────────────────────────────────────────────────────
def audio_callback(indata, frames, time_info, status):
    if status:
        pass
    audio_q.put(bytes(indata))

def record_utterance() -> np.ndarray | None:
    """Block until user speaks, then record until silence. Returns PCM array or None."""
    frames_voiced    = []
    frames_buffer    = []   # ring buffer for pre-roll
    num_silent       = 0
    num_voiced       = 0
    silence_frames   = int(SILENCE_SECS * 1000 / FRAME_MS)
    min_voiced_frames= int(MIN_SPEECH_SECS * 1000 / FRAME_MS)
    in_speech        = False

    print("🎤 Listening...", end="\r", flush=True)

    while True:
        chunk = audio_q.get()
        # VAD expects exactly FRAME_SAMPLES*2 bytes (16-bit PCM)
        if len(chunk) < FRAME_SAMPLES * 2:
            continue
        frame = chunk[:FRAME_SAMPLES * 2]

        # Amplify audio 8x for low-volume mics before VAD
        pcm = np.frombuffer(frame, dtype=np.int16).astype(np.int32)
        pcm = np.clip(pcm * 8, -32768, 32767).astype(np.int16)
        frame = pcm.tobytes()

        try:
            is_speech = vad.is_speech(frame, SAMPLE_RATE)
        except Exception:
            continue

        if is_speech:
            if not in_speech:
                in_speech = True
                print("🔴 Recording...  ", end="\r", flush=True)
                # prepend pre-roll buffer
                frames_voiced.extend(frames_buffer[-8:])
            frames_voiced.append(frame)
            frames_buffer.clear()
            num_voiced += 1
            num_silent = 0
        else:
            if in_speech:
                frames_voiced.append(frame)
                num_silent += 1
                if num_silent >= silence_frames:
                    break
            else:
                frames_buffer.append(frame)
                if len(frames_buffer) > 16:
                    frames_buffer.pop(0)

    if num_voiced < min_voiced_frames:
        print("🎤 Listening...", end="\r", flush=True)
        return None

    pcm = b"".join(frames_voiced)
    arr = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
    return arr


# ── Whisper STT ───────────────────────────────────────────────────────────────
def transcribe(audio: np.ndarray) -> str:
    segments, info = whisper.transcribe(
        audio, beam_size=5, language="ja",
        vad_filter=True, vad_parameters={"min_silence_duration_ms": 500}
    )
    text = " ".join(s.text.strip() for s in segments).strip()
    return text


# ── Agent call ────────────────────────────────────────────────────────────────
VOICE_SYSTEM = (
    "You are a concise voice assistant. Respond in 1-3 short sentences suitable for speech. "
    "No markdown, no bullet points, no code blocks. Just natural spoken Japanese. "
    "If a task requires tools, run them silently and summarize results conversationally."
)

def call_agent(user_text: str, source: str = "voice", stream_tts: bool = False) -> str:
    """Send text to mini-agent-c, stream response, optionally pipe to TTS in real-time."""
    conversation_history.append({"role": "user", "content": user_text, "source": source})

    # Build task with recent context
    context_lines = []
    if len(conversation_history) > 2:
        for m in conversation_history[-5:-1]:
            role = "User" if m["role"] == "user" else "Assistant"
            context_lines.append(f"{role}: {m['content'][:200]}")
    context = "\n".join(context_lines)
    task = f"{VOICE_SYSTEM}\n\n{context}\n\nUser: {user_text}" if context else f"{VOICE_SYSTEM}\n\nUser: {user_text}"

    headers = {"Content-Type": "application/json"}
    if AGENT_TOKEN:
        headers["Authorization"] = f"Bearer {AGENT_TOKEN}"

    body = {
        "task": task,
        "model": AGENT_MODEL,
        "backend": AGENT_BACKEND,
        "api_base": AGENT_API_BASE,
        "budget": 60000,
        "max_turns": 6,
        "stream": True,
        "sandbox": False,
        "plan": False,
        "no_memory": False,
    }

    response_text = ""
    tts_buf = ""  # buffer for sentence-by-sentence TTS

    def _flush_tts_buf(buf: str, force: bool = False) -> str:
        """Flush complete sentences from buf to TTS queue. Returns remainder."""
        if not stream_tts:
            return buf
        while True:
            m = _re.search(r'[。！？!?\.\…]\s*', buf)
            if not m:
                break
            sentence = _clean_for_tts(buf[:m.end()])
            buf = buf[m.end():]
            if sentence and len(sentence) > 1:
                _tts_queue.put(sentence)
        if force and buf.strip():
            rem = _clean_for_tts(buf.strip())
            if rem and len(rem) > 1:
                _tts_queue.put(rem)
            buf = ""
        return buf

    try:
        with requests.post(f"{AGENT_URL}/run", json=body, headers=headers,
                           stream=True, timeout=300) as resp:
            for line in resp.iter_lines(decode_unicode=True):
                if not line.startswith("data: "):
                    continue
                try:
                    obj = json.loads(line[6:])
                except Exception:
                    continue

                chunk = None
                if "content" in obj:
                    chunk = obj["content"]
                    if chunk.startswith("[assistant] "):
                        chunk = chunk[len("[assistant] "):]
                elif "line" in obj:
                    ln = obj["line"]
                    if ln.startswith("[assistant]"):
                        chunk = ln[len("[assistant]"):].lstrip()
                    elif ln.startswith("[tool]") or ln.startswith("=== turn"):
                        print(f"\n  {ln}", flush=True)

                if chunk:
                    response_text += chunk
                    print(chunk, end="", flush=True)
                    tts_buf += chunk
                    tts_buf = _flush_tts_buf(tts_buf)

        # flush any remaining text to TTS
        _flush_tts_buf(tts_buf, force=True)

    except Exception as e:
        response_text = f"エラーが発生しました: {e}"
        if stream_tts:
            _tts_queue.put("エラーが発生しました。")

    print()

    conversation_history.append({"role": "agent", "content": response_text})
    _save_history(user_text, response_text, source)
    return response_text


# ── TTS ───────────────────────────────────────────────────────────────────────
import re as _re
import tempfile as _tempfile
import edge_tts as _edge_tts

_tts_queue: queue.Queue = queue.Queue()
_tts_player_started = False
_current_afplay: subprocess.Popen | None = None  # track active playback process

def _clean_for_tts(text: str) -> str:
    """Remove markdown and emoji for cleaner speech."""
    text = _re.sub(r'\*+', '', text)
    text = _re.sub(r'`+[^`]*`+', '', text)
    text = _re.sub(r'#+\s*', '', text)
    text = _re.sub(r'\[([^\]]+)\]\([^\)]+\)', r'\1', text)  # [text](url) → text
    text = _re.sub(r'[✅🔄⚡📄💊📝🤖👤🎤🔴⬜]', '', text)
    text = _re.sub(r'\s+', ' ', text).strip()
    return text

def _sentence_split(text: str) -> list[str]:
    """Split text into speakable sentences."""
    parts = _re.split(r'(?<=[。！？!?\.…])\s*', text)
    return [p.strip() for p in parts if p.strip() and len(p.strip()) > 1]

def _tts_worker():
    """Background thread: dequeue sentences → generate → play sequentially."""
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    async def _speak_sentence(sentence: str):
        global _current_afplay
        with _tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as f:
            tmp = f.name
        try:
            comm = _edge_tts.Communicate(sentence, TTS_VOICE, rate=TTS_RATE)
            await comm.save(tmp)
            proc = subprocess.Popen(["afplay", tmp])
            _current_afplay = proc
            proc.wait()
            _current_afplay = None
        except Exception:
            subprocess.run(["say", "-v", "Kyoko", sentence], timeout=15)
        finally:
            try: os.unlink(tmp)
            except: pass

    while True:
        sentence = _tts_queue.get()
        if sentence is None:
            break
        speaking.set()
        try:
            loop.run_until_complete(_speak_sentence(sentence))
        except Exception:
            pass
        if _tts_queue.empty():
            speaking.clear()

def _ensure_tts_player():
    global _tts_player_started
    if not _tts_player_started:
        t = threading.Thread(target=_tts_worker, daemon=True)
        t.start()
        _tts_player_started = True

def speak(text: str):
    """Enqueue text for seamless neural TTS playback."""
    _ensure_tts_player()
    clean = _clean_for_tts(text)
    if not clean:
        return
    if len(clean) > 600:
        clean = clean[:600] + "、以降はテキストをご覧ください。"
    for sentence in _sentence_split(clean):
        _tts_queue.put(sentence)

def tts_stop():
    """Immediately stop TTS playback and clear the queue (barge-in)."""
    global _current_afplay
    # Clear pending sentences
    while not _tts_queue.empty():
        try: _tts_queue.get_nowait()
        except: pass
    # Kill active afplay
    if _current_afplay and _current_afplay.poll() is None:
        _current_afplay.terminate()
        _current_afplay = None
    speaking.clear()

def flush_audio_queue():
    """Drain stale audio captured while agent was speaking."""
    while not audio_q.empty():
        try: audio_q.get_nowait()
        except: pass

def speak_stream(text_iter):
    """Speak text as it streams in — fires sentences as they complete."""
    _ensure_tts_player()
    buf = ""
    for chunk in text_iter:
        buf += chunk
        # fire each completed sentence immediately
        while True:
            m = _re.search(r'[。！？!?\.\…]\s*', buf)
            if not m:
                break
            sentence = _clean_for_tts(buf[:m.end()])
            buf = buf[m.end():]
            if sentence and len(sentence) > 1:
                _tts_queue.put(sentence)
    # speak any remaining
    if buf.strip():
        rem = _clean_for_tts(buf.strip())
        if rem and len(rem) > 1:
            _tts_queue.put(rem)


# ── History ───────────────────────────────────────────────────────────────────
def _save_history(user: str, agent: str, source: str):
    HISTORY_FILE.parent.mkdir(parents=True, exist_ok=True)
    with HISTORY_FILE.open("a") as f:
        f.write(json.dumps({
            "ts": time.time(),
            "source": source,
            "user": user[:500],
            "agent": agent[:1000],
        }, ensure_ascii=False) + "\n")


# ── Telegram bot ──────────────────────────────────────────────────────────────
def start_telegram_bot(token: str):
    """Poll Telegram and respond to messages."""
    import urllib.request
    import urllib.parse

    base = f"https://api.telegram.org/bot{token}"
    offset = 0

    def tg_get(method, params=None):
        url = f"{base}/{method}"
        if params:
            url += "?" + urllib.parse.urlencode(params)
        with urllib.request.urlopen(url, timeout=30) as r:
            return json.loads(r.read())

    def tg_send(chat_id, text):
        data = urllib.parse.urlencode({"chat_id": chat_id, "text": text[:4000]}).encode()
        urllib.request.urlopen(f"{base}/sendMessage", data=data, timeout=10)

    print(f"🤖 Telegram bot started", flush=True)
    while True:
        try:
            result = tg_get("getUpdates", {"offset": offset, "timeout": 20, "limit": 5})
            for update in result.get("result", []):
                offset = update["update_id"] + 1
                msg = update.get("message", {})
                text = msg.get("text", "").strip()
                chat_id = msg.get("chat", {}).get("id")
                if text and chat_id:
                    print(f"\n📱 Telegram [{chat_id}]: {text}", flush=True)
                    response = call_agent(text, source="telegram")
                    tg_send(chat_id, response)
                    # also speak if not already speaking
                    if not speaking.is_set():
                        threading.Thread(target=speak, args=(response,), daemon=True).start()
        except Exception as e:
            print(f"[telegram error] {e}", flush=True)
            time.sleep(5)


# ── Main voice loop ───────────────────────────────────────────────────────────
def _barge_in_monitor():
    """
    While TTS is playing, watch for user voice (barge-in).
    If BARGE_IN_FRAMES consecutive voiced frames detected → stop TTS.
    """
    voiced_count = 0
    while speaking.is_set():
        if audio_q.empty():
            time.sleep(0.02)
            continue
        chunk = audio_q.get()
        frame = chunk[:FRAME_SAMPLES * 2]
        # energy-based check (fast, no VAD overhead)
        pcm = np.frombuffer(frame, dtype=np.int16)
        rms = int(np.sqrt(np.mean(pcm.astype(np.float32) ** 2)))
        if rms > BARGE_IN_ENERGY:
            voiced_count += 1
            if voiced_count >= BARGE_IN_FRAMES:
                print("\n⚡ Barge-in detected!", flush=True)
                tts_stop()
                return
        else:
            voiced_count = 0


def voice_loop():
    """Seamless voice catchball loop with barge-in and mic muting during TTS."""
    print("\n🎙️  Voice agent ready. Speak freely — silence ends your turn.")
    print("    Talk while agent speaks to interrupt (barge-in).")
    print("    Ctrl+C to quit\n")
    _ensure_tts_player()

    with sd.RawInputStream(
        samplerate=SAMPLE_RATE,
        blocksize=FRAME_SAMPLES,
        dtype="int16",
        channels=1,
        callback=audio_callback,
    ):
        while True:
            try:
                # ── Wait for TTS to finish (or barge-in) ──
                if speaking.is_set():
                    barge_thread = threading.Thread(target=_barge_in_monitor, daemon=True)
                    barge_thread.start()
                    barge_thread.join()
                    time.sleep(0.15)  # brief cooldown after TTS ends

                # Flush audio captured during agent speech
                flush_audio_queue()

                # ── Listen for user utterance ──
                print("🎤 ", end="\r", flush=True)
                audio = record_utterance()
                if audio is None:
                    continue

                # ── Transcribe ──
                print("✏️  ", end="\r", flush=True)
                user_text = transcribe(audio)
                if not user_text or len(user_text) < 2:
                    continue

                print(f"\n👤 {user_text}")
                print("🤖 ", end="", flush=True)

                # ── Agent + streaming TTS ──
                speaking.set()  # mark as "busy" early so barge-in can trigger
                call_agent(user_text, source="voice", stream_tts=True)

            except KeyboardInterrupt:
                print("\n\n👋 Goodbye!")
                tts_stop()
                break
            except Exception as e:
                print(f"\n[error] {e}", flush=True)


# ── Entry point ───────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Voice-first local agent")
    parser.add_argument("--telegram", metavar="TOKEN",
                        help="Telegram bot token (also reads TELEGRAM_BOT_TOKEN env var)")
    parser.add_argument("--model", default=None, help="LLM model name")
    parser.add_argument("--whisper", default=WHISPER_MODEL,
                        choices=["tiny", "base", "small", "medium", "large-v3"],
                        help="Whisper model size")
    parser.add_argument("--voice", default=None, help="macOS voice name")
    parser.add_argument("--english", action="store_true", help="Use English (Samantha voice)")
    args = parser.parse_args()

    global AGENT_MODEL, TTS_VOICE
    if args.model:
        AGENT_MODEL = args.model
    if args.voice:
        TTS_VOICE = args.voice
    if args.english:
        TTS_VOICE = "Samantha"

    tg_token = args.telegram or os.environ.get("TELEGRAM_BOT_TOKEN", "")
    if tg_token:
        t = threading.Thread(target=start_telegram_bot, args=(tg_token,), daemon=True)
        t.start()

    voice_loop()

if __name__ == "__main__":
    main()

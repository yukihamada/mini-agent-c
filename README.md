# ⚡ Evo — Autonomous Self-Evolving Agent

[![Build](https://github.com/yukihamada/mini-agent-c/actions/workflows/build.yml/badge.svg)](https://github.com/yukihamada/mini-agent-c/actions/workflows/build.yml)
[![GitHub](https://img.shields.io/badge/github-mini--agent--c-181717?logo=github)](https://github.com/yukihamada/mini-agent-c)
[![Release](https://img.shields.io/github/v/release/yukihamada/mini-agent-c?logo=github)](https://github.com/yukihamada/mini-agent-c/releases)
[![Web UI](https://img.shields.io/badge/web-mini--agent.yukihamada.jp-10b981?logo=safari)](https://mini-agent.yukihamada.jp)
[![Swift](https://img.shields.io/badge/server-Swift-fa7343?logo=swift)](web/server.swift)
[![iOS](https://img.shields.io/badge/iOS-Evo_App-000?logo=apple)](ios/)
[![MLX](https://img.shields.io/badge/LLM-MLX_Qwen3.5-blueviolet)](https://github.com/ml-explore/mlx)

A lightweight autonomous AI agent written in **C**, powered by local **MLX LLMs** (Qwen3.5 122B/27B/9B). Features a beautiful chat web UI, iOS app, self-evolution, and 16+ tools for system operations, cloud services, and more.

---

## ✨ Features

| | |
|---|---|
| 🧠 **Local LLM** | Qwen3.5 122B / 27B / 9B / 35B via MLX on Apple Silicon |
| 🌐 **Web UI** | Real-time SSE streaming chat — accessible from anywhere via Cloudflare Tunnel |
| 📱 **iOS App** | Native WKWebView app "Evo" — update UI without App Store releases |
| ⚡ **Self-Evolution** | Agent reads its own C source, improves it, compiles, and evaluates |
| 🔧 **16+ Tools** | File ops, bash, memory, Fly.io, Telegram, Gmail, GitHub, M5 Mac SSH |
| 🛡️ **Resource Guard** | Auto-kills agents at CPU >92% / RAM >90% |
| 🔄 **Fallback** | MLX failure → Claude Haiku (Anthropic) |
| 🔒 **Auth** | Bearer token + safe-area-aware mobile UI |

---

## 🏗️ Architecture

```
iPhone / Browser
      │ HTTPS
      ▼
Cloudflare Tunnel (mini-agent.yukihamada.jp)
      │
      ▼
Swift HTTP Server (port 7878)          ← web/server.swift
      │ SSE stream
      ▼
agent.v11 (C binary)                   ← agent.v11.c
      │ OpenAI-compatible API
      ▼
MLX LLM Server (port 5001)             ← mlx_lm.server on M5 Mac
  Qwen3.5-122B / 27B / 9B
```

---

## 🚀 Quick Start

```bash
git clone https://github.com/yukihamada/mini-agent-c
cd mini-agent-c

# Build agent + Swift server
make agent.v11 server
```

### Start MLX LLM server (Apple Silicon required)

```bash
pip install mlx-lm
mlx_lm.server --model mlx-community/Qwen3.5-9B-4bit --port 5001
```

### Start the web server (with auto-restart)

```bash
MINI_AGENT_WEB_TOKEN=your_token \
CPU_REFUSE_PCT=85 CPU_KILL_PCT=92 \
make run
```

Or for one-shot CLI use:

```bash
make agent.v11 server
```

Open `http://localhost:7878` — or expose via Cloudflare Tunnel for remote access.

### 4. CLI usage

```bash
# Run with local MLX
./agent.v11 --model mlx-community/Qwen3.5-9B-4bit \
            --backend openai \
            --api-base http://127.0.0.1:5001 \
            "Show disk usage and top CPU processes"

# Run with Anthropic (cloud)
ANTHROPIC_API_KEY=sk-... ./agent.v11 "List all Fly.io apps"
```

---

## 🌐 Web UI

Real-time streaming chat interface built as a PWA (Progressive Web App).

- **URL**: [mini-agent.yukihamada.jp](https://mini-agent.yukihamada.jp)
- Glass-blur header, animated avatar, markdown rendering (code blocks, bold, lists)
- iOS safe-area aware, 44px touch targets, keyboard-dismissal on send
- Settings sheet: model selector, auth token, budget, Evolve button
- Add to Home Screen for full-screen PWA experience

---

## 📱 iOS App (Evo)

`ios/` — WKWebView wrapper around the web UI.

```
Bundle ID:  com.enablerdao.evo
Name:       Evo
Deployment: iOS 17.0+
```

The iOS app injects the auth token via JavaScript, so the web UI can be updated without releasing a new App Store version.

```bash
cd ios
xcodegen generate
open Evo.xcodeproj
```

---

## ⚡ Self-Evolution

The agent can improve its own C source code:

1. **POST /evolve** — triggers an evolution cycle
2. Agent reads `agent.vN.c`, identifies improvements
3. Writes `agent.v(N+1).c` with changes
4. Compiles: `cc -O2 ... agent.v(N+1).c cJSON.c -lcurl -lm`
5. Runs `eval.sh` to score the new binary
6. Records result in `.agent/eval_history.jsonl`

---

## 🔧 Tools

### Built-in (C)
| Tool | Description |
|------|-------------|
| `read_file` | Read any file |
| `write_file` | Create/overwrite files |
| `bash` | Execute shell commands |
| `list_dir` | List directory contents |
| `save_memory` | Persist notes to `~/.mini-agent/memory.md` |
| `recall_memory` | Read persistent memory |
| `spawn_agent` | Spawn a child agent (depth-limited) |

### Dynamic (.agent/tools/*.sh)
| Tool | Description |
|------|-------------|
| `fly_ops` | Deploy/logs/status/secrets for any Fly.io app |
| `telegram_send` | Send Telegram message to @yukihamada_ai_bot |
| `gmail_ops` | Search/read/send Gmail via gog CLI |
| `github_ops` | PR/issue/CI management via gh CLI |
| `m5_exec` | SSH command execution on M5 Mac |
| `self_improve` | Status/evolve/eval-history operations |
| `power_info` | macOS battery/power information |
| `http_fetch` | HTTP GET/POST requests |

---

## 📁 Project Structure

```
mini-agent-c/
├── agent.v11.c          # Current agent source (C)
├── cJSON.c / cJSON.h    # JSON library
├── eval.sh              # Agent evaluation script
├── Makefile             # Build rules
├── web/
│   ├── server.swift     # HTTP server (Swift)
│   ├── index.html       # Web UI (PWA)
│   └── server.py        # Python server (legacy)
├── ios/
│   ├── Sources/App/     # SwiftUI WKWebView app
│   └── project.yml      # XcodeGen spec
└── .agent/
    ├── tools/           # Dynamic shell tools
    ├── web_history.jsonl
    └── eval_history.jsonl
```

---

## ⚙️ Environment Variables

| Var | Default | Description |
|-----|---------|-------------|
| `MINI_AGENT_WEB_TOKEN` | `` | Bearer auth token |
| `PORT` | `7878` | HTTP server port |
| `MLX_HOST` | `127.0.0.1` | MLX server host |
| `MLX_PORT` | `5001` | MLX server port |
| `MAX_CONCURRENT` | `2` | Max parallel agents |
| `CPU_REFUSE_PCT` | `85` | Reject new requests above this CPU% |
| `CPU_KILL_PCT` | `92` | Kill oldest agent above this CPU% |
| `MEM_KILL_PCT` | `90` | Kill oldest agent above this RAM% |
| `AUTO_EVOLVE_HOURS` | `0` | Auto-evolve every N hours (0=off) |

---

## 🤖 Agent Versions

| Version | Key Addition |
|---------|-------------|
| v1 | Basic tool-use loop |
| v6 | Dynamic tools, spawn_agent, memory |
| v8 | grep_files, glob_files, streaming bash |
| v9 | Parallel tools, git, diff_files, notify |
| v10 | HTTP requests, checkpoint/undo, clipboard |
| v11 | Self-evolution, resource guard, MLX support |

---

MIT — built by [yukihamada](https://github.com/yukihamada)

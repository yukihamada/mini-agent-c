# Security Policy

## Reporting a vulnerability

Please **do not** open a public GitHub issue for security vulnerabilities.

Email: [mail@yukihamada.jp](mailto:mail@yukihamada.jp)

Include:
- Description of the vulnerability
- Steps to reproduce
- Potential impact

You'll get a response within 48 hours.

## Scope

| Component | Notes |
|-----------|-------|
| `agent.vN.c` — bash tool | The `bash` tool executes arbitrary shell commands. This is intentional — the agent is designed for local/trusted use. |
| `web/server.swift` — HTTP server | Auth via `MINI_AGENT_WEB_TOKEN`. Always set a strong token in production. |
| MLX proxy (`/mlx/*`) | Forwards requests to local MLX server — do not expose without auth. |

## Deployment hardening

```bash
# Always set an auth token
export MINI_AGENT_WEB_TOKEN=$(openssl rand -hex 32)

# Bind to localhost only (default)
# Expose via Cloudflare Tunnel — never expose port 7878 directly

# Limit CPU so the host stays usable
CPU_REFUSE_PCT=80 CPU_KILL_PCT=90
```

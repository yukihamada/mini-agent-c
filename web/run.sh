#!/bin/bash
# web/run.sh — auto-restart wrapper for server-swift
# Usage: ./web/run.sh
# Restarts the server automatically if it crashes.

cd "$(dirname "$0")/.."

SERVER="./web/server-swift"

if [ ! -x "$SERVER" ]; then
    echo "[run.sh] Building server-swift..."
    swiftc -O web/server.swift -o web/server-swift
fi

# ── Check MLX server ─────────────────────────────────────────────────────────
MLX_HOST="${MLX_HOST:-127.0.0.1}"
MLX_PORT="${MLX_PORT:-5001}"
if curl -sf "http://${MLX_HOST}:${MLX_PORT}/health" &>/dev/null || \
   curl -sf "http://${MLX_HOST}:${MLX_PORT}/v1/models" &>/dev/null; then
    echo "[run.sh] MLX server detected at http://${MLX_HOST}:${MLX_PORT}"
else
    echo "[run.sh] WARNING: MLX server not found at http://${MLX_HOST}:${MLX_PORT}"
    echo "[run.sh]          Local model inference will not be available."
    echo "[run.sh]          Start with: mlx_lm.server --model <model>"
fi

# ── Print external URL if Cloudflare Tunnel is configured ────────────────────
if [ -n "${CLOUDFLARE_TUNNEL:-}" ]; then
    echo "[run.sh] Cloudflare Tunnel: https://${CLOUDFLARE_TUNNEL}"
fi

echo "[run.sh] Starting server (auto-restart enabled)"
while true; do
    "$SERVER" "$@"
    EXIT=$?
    if [ $EXIT -eq 0 ]; then
        echo "[run.sh] Server exited cleanly (code 0). Stopping."
        break
    fi
    echo "[run.sh] Server crashed (code $EXIT). Restarting in 2s..."
    sleep 2
done

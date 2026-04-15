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

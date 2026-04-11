#!/bin/bash
# deploy.sh — push mini-agent-c to OpenClaw fleet VPS
#
# Usage:
#   ./deploy.sh                   # deploy to all IPs in $FLEET_IPS
#   ./deploy.sh <ip>              # deploy to one
#   ./deploy.sh --rollback        # stop + uninstall
#   ./deploy.sh --test <ip>       # one-shot heartbeat run (no install)
#
# Requires env: ANTHROPIC_API_KEY, TG_BOT_TOKEN, optionally TG_CHAT_ID.

set -euo pipefail
cd "$(dirname "$0")"

REPO_ROOT=$(cd ../.. && pwd)
BINARY="$REPO_ROOT/agent.v4"
CJSON_C="$REPO_ROOT/cJSON.c"

FLEET_IPS="${FLEET_IPS:-46.225.134.252 46.225.172.3 46.225.229.16 178.104.60.154}"
SSH_USER="${SSH_USER:-root}"
REMOTE_DIR="/opt/mini-agent"

: "${ANTHROPIC_API_KEY:?not set}"
: "${TG_BOT_TOKEN:?not set — get from https://t.me/BotFather}"

MODE="install"
case "${1:-}" in
    --rollback) MODE="rollback"; shift ;;
    --test)     MODE="test"; shift ;;
esac
TARGETS="${*:-$FLEET_IPS}"

build_remote_source() {
    # Fleet is Linux x86_64, we're on macOS. We need to compile ON the target
    # (easiest) or cross-compile. Simpler: ship the .c and compile on each VPS.
    tar czf /tmp/mini-agent-src.tgz \
        -C "$REPO_ROOT" agent.v3.c agent.v4.c cJSON.c cJSON.h Makefile \
        -C "$(pwd)" tools systemd
}

install_one() {
    local ip="$1"
    echo "[deploy] ===== $ip ====="
    ssh -o StrictHostKeyChecking=accept-new "$SSH_USER@$ip" "mkdir -p $REMOTE_DIR/.agent/tools /var/log/mini-agent /etc/mini-agent"

    scp /tmp/mini-agent-src.tgz "$SSH_USER@$ip:$REMOTE_DIR/"
    ssh "$SSH_USER@$ip" "cd $REMOTE_DIR && tar xzf mini-agent-src.tgz && rm mini-agent-src.tgz"

    # Compile on remote (needs libcurl-dev, gcc)
    ssh "$SSH_USER@$ip" "cd $REMOTE_DIR && \
        (command -v gcc >/dev/null || apt-get update -qq && apt-get install -y gcc libcurl4-openssl-dev) && \
        gcc -O2 -Wall -Wno-comment -std=c99 -D_POSIX_C_SOURCE=200809L \
            -o agent.v4 agent.v4.c cJSON.c -lcurl -lm"

    # Install tools
    ssh "$SSH_USER@$ip" "cp $REMOTE_DIR/tools/*.sh $REMOTE_DIR/.agent/tools/ && chmod +x $REMOTE_DIR/.agent/tools/*.sh"

    # Env file (secrets)
    ssh "$SSH_USER@$ip" "cat > /etc/mini-agent/env" <<EOF
ANTHROPIC_API_KEY=$ANTHROPIC_API_KEY
TG_BOT_TOKEN=$TG_BOT_TOKEN
TG_CHAT_ID=${TG_CHAT_ID:-1136442501}
HOME=$REMOTE_DIR
EOF
    ssh "$SSH_USER@$ip" "chmod 600 /etc/mini-agent/env"

    # systemd
    scp systemd/mini-agent-heartbeat.service "$SSH_USER@$ip:/etc/systemd/system/"
    scp systemd/mini-agent-heartbeat.timer   "$SSH_USER@$ip:/etc/systemd/system/"
    ssh "$SSH_USER@$ip" "systemctl daemon-reload && \
        systemctl enable --now mini-agent-heartbeat.timer && \
        systemctl list-timers mini-agent-heartbeat.timer --no-pager"

    echo "[deploy] $ip: installed"
}

rollback_one() {
    local ip="$1"
    echo "[rollback] $ip"
    ssh "$SSH_USER@$ip" "systemctl disable --now mini-agent-heartbeat.timer 2>/dev/null || true; \
        rm -f /etc/systemd/system/mini-agent-heartbeat.{service,timer} /etc/mini-agent/env; \
        systemctl daemon-reload; \
        rm -rf $REMOTE_DIR"
}

test_one() {
    local ip="$1"
    echo "[test] one-shot run on $ip (not installing)"
    scp "$BINARY" "$SSH_USER@$ip:/tmp/agent.test" 2>/dev/null || {
        echo "(direct binary copy failed — fleet is likely different arch; use 'install' mode)"
        return 1
    }
    ssh "$SSH_USER@$ip" "ANTHROPIC_API_KEY='$ANTHROPIC_API_KEY' /tmp/agent.test --max-turns 3 --budget 10000 --quiet 'echo hello from mini-agent-c'"
}

case "$MODE" in
    install)
        build_remote_source
        for ip in $TARGETS; do install_one "$ip"; done
        rm -f /tmp/mini-agent-src.tgz
        ;;
    rollback)
        for ip in $TARGETS; do rollback_one "$ip"; done
        ;;
    test)
        for ip in $TARGETS; do test_one "$ip"; done
        ;;
esac

echo "[deploy] done"

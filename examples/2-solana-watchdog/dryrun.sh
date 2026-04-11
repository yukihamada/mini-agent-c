#!/bin/bash
# dryrun.sh — test the watchdog locally against public Solana RPC
# No VPS touch. Uses your local agent.v4 binary.
set -euo pipefail
cd "$(dirname "$0")"

REPO_ROOT=$(cd ../.. && pwd)
BINARY="$REPO_ROOT/agent.v4"

if [ ! -x "$BINARY" ]; then
    echo "Building $BINARY..."
    (cd "$REPO_ROOT" && make agent.v4 2>/dev/null || \
     cc -O2 -Wall -Wno-comment -std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE \
        -o agent.v4 agent.v4.c cJSON.c -lcurl -lm)
fi

if [ -z "${ANTHROPIC_API_KEY:-}" ] && [ -f "$HOME/.env" ]; then
    export ANTHROPIC_API_KEY=$(grep '^ANTHROPIC_API_KEY' "$HOME/.env" | sed 's/^ANTHROPIC_API_KEY="\(.*\)"$/\1/')
fi
: "${ANTHROPIC_API_KEY:?not set}"
: "${SOLANA_RPC_URL:=https://api.mainnet-beta.solana.com}"
: "${WALLET_ADDRESS:=BeWJUYAp1rijxcNmYe9hvKCt9UvA4sHJvPKVGqrd5gmo}"   # pasha wallet for dryrun

# Mock TG so we don't actually send
export TG_BOT_TOKEN="${TG_BOT_TOKEN:-dryrun_dummy}"
export TG_CHAT_ID="${TG_CHAT_ID:-0}"

# Create a fake workspace with our tools loaded
WORKDIR=$(mktemp -d -t solana-wd.XXXXXX)
trap 'rm -rf "$WORKDIR"' EXIT

mkdir -p "$WORKDIR/.agent/tools"
cp tools/*.sh "$WORKDIR/.agent/tools/"
chmod +x "$WORKDIR/.agent/tools/"*.sh

# Fake log files so bot_log has something to read
mkdir -p "$WORKDIR/logs"
export LOG_DIR="$WORKDIR/logs"
cat > "$WORKDIR/logs/liq-marginfi.log" <<'EOF'
2026-04-11 13:00:01 INFO starting scan
2026-04-11 13:00:02 INFO scanned 1523 accounts
2026-04-11 13:00:05 INFO liquidation executed: 45 SOL
2026-04-11 13:00:10 INFO idle
EOF
cat > "$WORKDIR/logs/liq-kamino.log" <<'EOF'
2026-04-11 13:00:00 INFO starting
2026-04-11 13:00:03 WARN rpc slow (450ms)
2026-04-11 13:00:08 INFO idle
EOF
cat > "$WORKDIR/logs/liq-drift.log" <<'EOF'
2026-04-11 13:00:02 INFO scanning
2026-04-11 13:00:04 ERROR failed to fetch oracle
2026-04-11 13:00:05 ERROR failed to fetch oracle
2026-04-11 13:00:06 ERROR failed to fetch oracle
2026-04-11 13:00:07 ERROR failed to fetch oracle
2026-04-11 13:00:08 INFO retry succeeded
EOF

PROMPT=$(cat watchdog.prompt)

cd "$WORKDIR"
echo "[dryrun] running watchdog in $WORKDIR..."
"$BINARY" --max-turns 12 --budget 20000 --quiet "$PROMPT"
echo
echo "[dryrun] audit log:"
cat .agent/audit.log 2>/dev/null | tail -20

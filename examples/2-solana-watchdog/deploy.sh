#!/bin/bash
# deploy.sh — push watchdog to vm061603 (82.24.88.134)
#
# Usage:
#   ./deploy.sh               # install + enable cron
#   ./deploy.sh --rollback    # remove
#   ./deploy.sh --run-once    # one manual run on remote

set -euo pipefail
cd "$(dirname "$0")"

: "${ANTHROPIC_API_KEY:?}"
: "${TG_BOT_TOKEN:?}"
: "${WALLET_ADDRESS:?solana gas wallet pubkey required}"
: "${VM_HOST:=82.24.88.134}"
: "${VM_USER:=root}"
: "${SOLANA_RPC_URL:=https://api.mainnet-beta.solana.com}"
REMOTE_DIR="/opt/watchdog"

REPO_ROOT=$(cd ../.. && pwd)

MODE="install"
[ "${1:-}" = "--rollback" ] && MODE="rollback"
[ "${1:-}" = "--run-once" ] && MODE="runonce"

case "$MODE" in
    install)
        echo "[deploy] building on remote..."
        ssh -o StrictHostKeyChecking=accept-new "$VM_USER@$VM_HOST" "mkdir -p $REMOTE_DIR/.agent/tools /etc/watchdog"

        # ship source
        tar czf /tmp/watchdog-src.tgz \
            -C "$REPO_ROOT" agent.v4.c cJSON.c cJSON.h \
            -C "$(pwd)" tools watchdog.prompt
        scp /tmp/watchdog-src.tgz "$VM_USER@$VM_HOST:$REMOTE_DIR/"
        rm -f /tmp/watchdog-src.tgz

        ssh "$VM_USER@$VM_HOST" bash <<REMOTE
set -e
cd $REMOTE_DIR
tar xzf watchdog-src.tgz && rm watchdog-src.tgz
command -v gcc >/dev/null || (apt-get update -qq && apt-get install -y gcc libcurl4-openssl-dev python3 >/dev/null)
gcc -O2 -Wall -Wno-comment -std=c99 -D_POSIX_C_SOURCE=200809L \
    -o agent agent.v4.c cJSON.c -lcurl -lm
cp tools/*.sh .agent/tools/
chmod +x .agent/tools/*.sh
REMOTE

        # env file
        ssh "$VM_USER@$VM_HOST" "cat > /etc/watchdog/env" <<EOF
ANTHROPIC_API_KEY=$ANTHROPIC_API_KEY
TG_BOT_TOKEN=$TG_BOT_TOKEN
TG_CHAT_ID=${TG_CHAT_ID:-1136442501}
SOLANA_RPC_URL=$SOLANA_RPC_URL
WALLET_ADDRESS=$WALLET_ADDRESS
HOME=$REMOTE_DIR
EOF
        ssh "$VM_USER@$VM_HOST" "chmod 600 /etc/watchdog/env"

        # wrapper that sources env then runs agent
        ssh "$VM_USER@$VM_HOST" "cat > $REMOTE_DIR/run-watchdog.sh" <<'REMOTE'
#!/bin/bash
set -e
cd /opt/watchdog
set -a; . /etc/watchdog/env; set +a
exec ./agent --max-turns 10 --budget 15000 --quiet "$(cat watchdog.prompt)"
REMOTE
        ssh "$VM_USER@$VM_HOST" "chmod +x $REMOTE_DIR/run-watchdog.sh"

        # cron — every 5 minutes
        ssh "$VM_USER@$VM_HOST" "( crontab -l 2>/dev/null | grep -v run-watchdog.sh ; echo '*/5 * * * * /opt/watchdog/run-watchdog.sh >> /var/log/watchdog.log 2>&1' ) | crontab -"

        # one-shot test
        echo "[deploy] one-shot test run on remote..."
        ssh "$VM_USER@$VM_HOST" "$REMOTE_DIR/run-watchdog.sh"
        echo "[deploy] done. logs: ssh $VM_USER@$VM_HOST tail -f /var/log/watchdog.log"
        ;;

    rollback)
        ssh "$VM_USER@$VM_HOST" bash <<'REMOTE'
( crontab -l 2>/dev/null | grep -v run-watchdog.sh ) | crontab -
rm -rf /opt/watchdog /etc/watchdog
REMOTE
        echo "[deploy] rolled back"
        ;;

    runonce)
        ssh "$VM_USER@$VM_HOST" "/opt/watchdog/run-watchdog.sh"
        ;;
esac

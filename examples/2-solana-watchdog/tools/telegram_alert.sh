#!/bin/bash
#@name: telegram_alert
#@description: Send an alert message to the Telegram channel (only call on real anomalies)
#@arg: severity:info|warn|critical
#@arg: msg:Body text
set -e
: "${TG_BOT_TOKEN:?not set}"
SEV="$1"
MSG="$2"
CHAT_ID="${TG_CHAT_ID:-1136442501}"
ICON="ℹ️"
case "$SEV" in
    info)     ICON="ℹ️" ;;
    warn)     ICON="⚠️" ;;
    critical) ICON="🚨" ;;
esac
HOST=$(hostname -s 2>/dev/null || echo "?")
TEXT="${ICON} [watchdog@${HOST}] ${MSG}"
curl -sS --max-time 10 \
  "https://api.telegram.org/bot${TG_BOT_TOKEN}/sendMessage" \
  --data-urlencode "chat_id=${CHAT_ID}" \
  --data-urlencode "text=${TEXT}" \
  | head -c 300
echo

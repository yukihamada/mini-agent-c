#!/bin/bash
#@name: telegram_report
#@description: Send a message to the fleet telegram bot (@yukihamada_ai_bot)
#@arg: msg:Plain text message body (max ~3000 chars)
set -e
: "${TG_BOT_TOKEN:?TG_BOT_TOKEN not set}"
CHAT_ID="${TG_CHAT_ID:-1136442501}"
HOSTNAME=$(hostname -s 2>/dev/null || echo "?")
BODY="[$HOSTNAME] $1"
curl -sS --max-time 10 \
  "https://api.telegram.org/bot${TG_BOT_TOKEN}/sendMessage" \
  --data-urlencode "chat_id=${CHAT_ID}" \
  --data-urlencode "text=${BODY}" \
  | head -c 500
echo

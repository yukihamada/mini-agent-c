#!/bin/bash
#@name: telegram_send
#@description: Send a Telegram message to the owner (Yuki Hamada, chat_id 1136442501)
#@arg: message:Message text to send (supports Markdown)
set -e

MESSAGE="$*"
if [[ -z "$MESSAGE" ]]; then
    echo "Usage: telegram_send.sh <message>"
    exit 1
fi

# Load token from env or ~/.env
if [[ -z "$TELEGRAM_BOT_TOKEN" ]]; then
    TELEGRAM_BOT_TOKEN=$(grep "^TELEGRAM_BOT_TOKEN" ~/.env 2>/dev/null | cut -d= -f2- | tr -d '"')
fi

if [[ -z "$TELEGRAM_BOT_TOKEN" ]]; then
    echo "Error: TELEGRAM_BOT_TOKEN not found in environment or ~/.env"
    exit 1
fi

CHAT_ID=1136442501

RESPONSE=$(curl -sS "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendMessage" \
    -d "chat_id=${CHAT_ID}" \
    -d "parse_mode=Markdown" \
    --data-urlencode "text=${MESSAGE}")

if echo "$RESPONSE" | python3 -c "import sys,json; d=json.load(sys.stdin); exit(0 if d.get('ok') else 1)" 2>/dev/null; then
    echo "Message sent to owner successfully"
else
    echo "Response: $RESPONSE"
fi

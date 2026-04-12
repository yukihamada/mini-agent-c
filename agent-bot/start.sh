#!/bin/bash
# Load environment variables and start agent-bot

set -e

# Load .env if exists
if [ -f /Users/yuki/.env ]; then
    echo "Loading environment from /Users/yuki/.env"
    export $(cat /Users/yuki/.env | grep -v '^#' | xargs)
fi

# Check required variables
if [ -z "$TELEGRAM_BOT_TOKEN" ] && [ -z "$LINE_CHANNEL_ACCESS_TOKEN" ]; then
    echo "⚠️  WARNING: Neither TELEGRAM_BOT_TOKEN nor LINE_CHANNEL_ACCESS_TOKEN set"
    echo "At least one messaging platform should be configured"
fi

# Navigate to script directory
cd "$(dirname "$0")"

# Activate virtual environment if exists
if [ -d "venv" ]; then
    source venv/bin/activate
fi

# Start bot
echo "Starting agent-bot..."
python3 bot.py

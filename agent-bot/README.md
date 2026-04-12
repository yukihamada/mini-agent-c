# Agent-Bot

Telegram and LINE interface for agent.v10 autonomous agent.

## Features

- 🤖 **Telegram Bot**: Long-polling (works behind NAT, no webhook needed)
- 💬 **LINE Bot**: Webhook-based messaging
- 🔒 **Authorization**: Whitelist users via `AUTHORIZED_USERS` env var
- 🚀 **Simple Deployment**: Works locally or on Fly.io

## Architecture

```
User (Telegram/LINE) → agent-bot → agent.v10 → response
```

## Local Setup

### 1. Install Dependencies

```bash
cd agent-bot
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

### 2. Configure Environment

Create `/Users/yuki/.env`:

```bash
# Telegram Bot
TELEGRAM_BOT_TOKEN=your_telegram_bot_token_from_@BotFather

# LINE Bot (optional)
LINE_CHANNEL_SECRET=your_line_channel_secret
LINE_CHANNEL_ACCESS_TOKEN=your_line_channel_access_token

# Authorization (comma-separated user IDs, leave empty to allow all)
AUTHORIZED_USERS=123456789,987654321

# Port (default: 5000)
PORT=5000
```

### 3. Get Your Telegram Bot Token

1. Message [@BotFather](https://t.me/BotFather) on Telegram
2. Send `/newbot`
3. Follow instructions, get your token
4. Add to `.env` as `TELEGRAM_BOT_TOKEN`

### 4. Get Your User ID

Start the bot locally, send `/start`, bot will reply with your user ID.

### 5. Start Bot

```bash
chmod +x start.sh
./start.sh
```

## LINE Setup (Optional)

### 1. Create LINE Channel

1. Go to [LINE Developers Console](https://developers.line.biz/)
2. Create a Provider and Messaging API Channel
3. Get Channel Secret and Channel Access Token
4. Add to `.env`

### 2. Set Webhook URL

- Local testing: Use [ngrok](https://ngrok.com/): `ngrok http 5000`
- Production: `https://agent-bot.fly.dev/webhook/line`

Set in LINE Console → Messaging API → Webhook URL

## Fly.io Deployment

### 1. Install Fly CLI

```bash
brew install flyctl
fly auth login
```

### 2. Build agent.v10 for Linux

Before deploying, you need a Linux binary of agent.v10:

```bash
# On macOS, cross-compile for Linux (if you have Docker)
docker run --rm -v "$PWD":/workspace -w /workspace gcc:latest \
  gcc -o agent.v10.linux agent.c -lcurl

# Or build on a Linux machine
# Then copy to agent-bot/agent.v10
```

**Important**: Copy the Linux binary to `agent-bot/agent.v10` before deploying.

### 3. Create App

```bash
cd agent-bot
fly apps create agent-bot --region nrt
```

### 4. Set Secrets

```bash
fly secrets set TELEGRAM_BOT_TOKEN=your_token
fly secrets set LINE_CHANNEL_SECRET=your_secret
fly secrets set LINE_CHANNEL_ACCESS_TOKEN=your_token
fly secrets set AUTHORIZED_USERS=123456789,987654321
```

### 5. Deploy

```bash
fly deploy
```

### 6. Check Status

```bash
fly status
fly logs
```

Your LINE webhook URL: `https://agent-bot.fly.dev/webhook/line`

## Usage

### Telegram

1. Find your bot on Telegram (search @your_bot_name)
2. Send `/start` to initialize
3. Send any text message as a task for agent.v10
4. Bot replies with agent output

### LINE

1. Add bot as friend (via QR code in LINE Developer Console)
2. Send any text message as a task
3. Bot replies with agent output

## Authorization

Set `AUTHORIZED_USERS` to a comma-separated list of user IDs:

```bash
AUTHORIZED_USERS=123456789,987654321
```

To find your user ID:
- **Telegram**: Send `/start` to bot, it will show your ID
- **LINE**: Check server logs when you message the bot

Leave empty to allow all users (not recommended for production).

## Troubleshooting

### Telegram not responding
- Check `TELEGRAM_BOT_TOKEN` is correct
- Check bot has permission to read messages
- Check logs: `fly logs` or local console

### LINE webhook fails
- Verify webhook URL is set correctly in LINE Console
- Check `LINE_CHANNEL_ACCESS_TOKEN` and `LINE_CHANNEL_SECRET`
- Enable webhook in LINE Console
- Verify SSL certificate (Fly.io provides this automatically)

### Agent timeout
- Default timeout is 5 minutes
- Adjust in `bot.py` if needed

### Unauthorized error
- Add your user ID to `AUTHORIZED_USERS`
- Send `/start` on Telegram to see your ID

## Development

### Test Syntax

```bash
python3 -m py_compile bot.py
```

### Run Locally

```bash
./start.sh
```

### View Logs

```bash
# Local: Check terminal
# Fly.io: 
fly logs
fly logs -f  # Follow mode
```

## Security Notes

1. **Never commit** `.env` or tokens to git
2. Use `AUTHORIZED_USERS` to restrict access
3. Fly.io secrets are encrypted
4. LINE webhook requires HTTPS (Fly.io provides)

## Architecture Details

### Telegram (Long-polling)
- Runs in background thread
- Polls Telegram API every 30s
- Works behind NAT/firewall
- No webhook/public URL needed

### LINE (Webhook)
- Flask server on port 5000
- Requires public HTTPS URL
- Fly.io provides SSL automatically

### Agent Execution
- Runs `agent.v10` as subprocess
- 5-minute timeout
- Working directory: `/Users/yuki/workspace/mini-agent-c/`
- Output truncated at 4000 chars (Telegram limit)

## License

Same as mini-agent-c project.

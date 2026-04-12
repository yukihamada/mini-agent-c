# Agent-Bot Quick Start

## 🎯 What is this?

Access **agent.v10** via Telegram or LINE from anywhere!

## 📱 Setup Telegram Bot (5 minutes)

### 1. Create Bot
- Message [@BotFather](https://t.me/BotFather)
- Send: `/newbot`
- Follow prompts, get token

### 2. Configure
Add to `/Users/yuki/.env`:
```bash
TELEGRAM_BOT_TOKEN=1234567890:ABCdefGHIjklMNOpqrsTUVwxyz
AUTHORIZED_USERS=  # Leave empty initially
```

### 3. Install & Run
```bash
cd /Users/yuki/workspace/mini-agent-c/agent-bot
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
./start.sh
```

### 4. Get Your User ID
- Find your bot on Telegram
- Send `/start`
- Bot replies with your ID (e.g., 123456789)

### 5. Lock it Down
Update `/Users/yuki/.env`:
```bash
AUTHORIZED_USERS=123456789
```
Restart bot.

## 💬 Usage

Just message your bot with any task:
```
What files are in the current directory?
```

Bot replies with agent.v10 output!

## 🌐 Deploy to Fly.io (Public Access)

### 1. Build Linux Binary
```bash
cd /Users/yuki/workspace/mini-agent-c
docker run --rm -v "$PWD":/workspace -w /workspace gcc:latest \
  gcc -o agent-bot/agent.v10 agent.c -lcurl
```

### 2. Deploy
```bash
cd agent-bot
fly launch --name agent-bot --region nrt
fly secrets set TELEGRAM_BOT_TOKEN=your_token
fly secrets set AUTHORIZED_USERS=123456789
fly deploy
```

Done! Bot runs 24/7 in Tokyo datacenter.

## 🔧 Common Issues

**Bot not responding?**
- Check token is correct
- Check bot is running (`./start.sh`)
- Check you're authorized

**"Unauthorized" error?**
- Get your ID with `/start`
- Add to `AUTHORIZED_USERS`
- Restart bot

**Agent timeout?**
- Task takes >5 min
- Check agent.v10 is executable
- Check path in bot.py

## 📚 Full Documentation

See [README.md](README.md) for:
- LINE integration
- Advanced configuration
- Troubleshooting
- Security notes

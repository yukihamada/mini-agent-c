# OpenClaw fleet — mini-agent-c worker

Context: OpenAI Codex subscription canceled (2026-04). Fleet VPS (Hachi/Kuro/Ichi/Ni) are idle.
This pack revives them with `mini-agent-c` as a lightweight worker (~93KB binary, no Node.js/Python runtime).

## What each agent will do

Every 30 minutes, triggered by systemd timer:
1. Collect host health (disk, CPU, memory, load)
2. Check any local services (fly apps, docker containers, systemd units)
3. Report anomalies to the fleet telegram bot (`@yukihamada_ai_bot`, chat_id=1136442501)
4. Audit log → `/var/log/mini-agent/audit.log`

Budget: 15K tokens per run (~$0.05). ~30/day = ~$1.50/month/VPS. 4 VPS = ~$6/month.

## Fleet

| Name | IP | Role |
|---|---|---|
| Hachi | 46.225.134.252 | claude-side |
| Kuro | 46.225.172.3 | openai-side (was Codex) |
| Ichi | 46.225.229.16 | openai-side (was Codex) |
| Ni | 178.104.60.154 | openai-side (was Codex) |

## Deploy

```bash
# Set these once in your shell
export ANTHROPIC_API_KEY=sk-ant-...
export TG_BOT_TOKEN=...          # telegram bot
export FLEET_IPS="46.225.134.252 46.225.172.3 46.225.229.16 178.104.60.154"

# Deploy to all VPS
./deploy.sh

# Or one at a time
./deploy.sh 46.225.134.252
```

`deploy.sh` will:
1. scp agent.v4 + tools/ + systemd unit files to each VPS
2. chmod, enable timer, start
3. run a one-shot test

## Rollback

```bash
./deploy.sh --rollback  # stops timer, removes files
```

## Monitor

```bash
ssh root@46.225.134.252 journalctl -u mini-agent-heartbeat.timer -f
ssh root@46.225.134.252 tail -f /var/log/mini-agent/audit.log
```

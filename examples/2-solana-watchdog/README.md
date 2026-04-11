# Solana liquidation bot watchdog (vm061603)

Context: Liquidation bots (marginfi/kamino/drift) run on vm061603 (82.24.88.134).
This pack deploys mini-agent-c as a 5-minute watchdog that monitors RPC health,
bot logs, wallet balances and alerts on Telegram when something's wrong.

## What it monitors

Every 5 minutes:
1. **RPC health** — current slot vs. cluster tip, detect RPC lag
2. **Bot logs** — tail the last 50 lines of each bot's log, count ERROR/WARN
3. **Wallet SOL balance** — ensure gas wallet isn't drained
4. **Position exposure** — optional: sanity check on recent liquidation count
5. **Alert conditions** — telegram_alert if any of:
   - RPC slot lag > 20
   - > 3 ERROR lines in last 5 min for any bot
   - SOL balance < 0.2
   - No recent activity (bot stuck)

## Audit trail

Every decision the watchdog makes is logged to `/opt/watchdog/.agent/audit.log` in JSONL —
timestamp, tool called, input, result preview. **This is the legal/operational record
of what the agent observed and what it did.** Rotate daily.

## Deploy

```bash
export ANTHROPIC_API_KEY=sk-ant-...
export VM_HOST=82.24.88.134
export VM_USER=root
export VM_PASS='NugSXfZmXmTXDyRG'   # from memory/solana_bots.md — prefer ssh key though
export TG_BOT_TOKEN=...
export TG_CHAT_ID=1136442501
export SOLANA_RPC_URL=https://api.mainnet-beta.solana.com   # or your paid RPC
export WALLET_ADDRESS=<gas wallet pubkey>

./deploy.sh
```

## Test locally first

```bash
# Dry-run the watchdog prompt without touching the VPS
./dryrun.sh
```

## Rollback

```bash
./deploy.sh --rollback
```

## Why this matters

- **Audit log** proves compliance: "we observed X at time T, decided Y, took action Z"
- **Token budget** is hard-capped at 15K/run → ~$0.05 → ~$4.50/day at 5min interval
- **Path confinement** prevents agent from touching anything outside `/opt/watchdog/`
- **Dangerous command denylist** blocks `sudo`, `rm -rf`, `dd` even if the model hallucinates
- **Kill switch** — `touch /opt/watchdog/.agent/STOP` stops the next run cleanly

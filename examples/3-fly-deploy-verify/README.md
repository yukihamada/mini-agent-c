# Fly.io deploy verifier

Post-deploy health check for Fly.io apps: after `fly deploy`, run mini-agent-c to
probe endpoints, check HTTP status + response shape, and on failure dump recent logs.

## Standalone usage

```bash
export ANTHROPIC_API_KEY=sk-ant-...
# Verify a single app
./verify.sh enablerdao https://enablerdao.com

# Multiple endpoints
./verify.sh enablerdao https://enablerdao.com https://enablerdao.com/api/health
```

Exit code:
- 0 = all endpoints healthy
- 1 = one or more failed (logs dumped)

## GitHub Actions integration

Ready-to-drop workflows (copy to `.github/workflows/` of each repo):

| File | Target repo | Fly app | Domains |
|---|---|---|---|
| `enablerdao-verify.yml` | enablerdao | enablerdao | enablerdao.com |
| `claudeterm-verify.yml` | claudeterm | claudeterm | chatweb.ai, teai.io, api.chatweb.ai |
| `jiuflow-ssr-verify.yml` | jiuflow-ssr | jiuflow-ssr | jiuflow.art |
| `fly-verify.yml` | (generic template) | any | parameterize |

Each workflow:
1. Triggers after `workflow_run: "Deploy"` succeeds (or manual `workflow_dispatch`)
2. Installs `mini-agent-c` + `flyctl` from GitHub
3. Runs `verify.sh <app> <url...>` against the listed endpoints
4. On failure, notifies via Slack/Telegram (if webhook/token secrets set)

### Required repo secrets

- `ANTHROPIC_API_KEY` — for the Claude agent
- `FLY_API_TOKEN` — so `fly logs` works in the failure diagnosis step
- `SLACK_WEBHOOK` or `TG_BOT_TOKEN`+`TG_CHAT_ID` — optional notification

### To activate

```bash
# From each product repo:
curl -sL https://raw.githubusercontent.com/yukihamada/mini-agent-c/master/examples/3-fly-deploy-verify/.github/workflows/enablerdao-verify.yml \
  -o .github/workflows/deploy-verify.yml
git add .github/workflows/deploy-verify.yml
git commit -m "ci: post-deploy verify via mini-agent-c"
git push
```

## What it does

1. `http_check <url>` — curl with timeout, return status+headers+first 4KB body
2. If any endpoint !2xx: `fly_logs <app>` — last 100 log lines
3. If failure: agent analyzes the logs, tries to identify the root cause, outputs a diagnosis
4. Exit 1 on any failure

Budget cap: 30K tokens (~$0.10/run).

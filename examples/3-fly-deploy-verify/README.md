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

Drop `.github/workflows/fly-verify.yml` into your repo and it will auto-run after deploy.
Customize the app name and endpoints at the top of the file.

## What it does

1. `http_check <url>` — curl with timeout, return status+headers+first 4KB body
2. If any endpoint !2xx: `fly_logs <app>` — last 100 log lines
3. If failure: agent analyzes the logs, tries to identify the root cause, outputs a diagnosis
4. Exit 1 on any failure

Budget cap: 30K tokens (~$0.10/run).

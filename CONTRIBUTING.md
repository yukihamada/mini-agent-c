# Contributing to Evo

## Ways to contribute

- **Report bugs** — open an issue with the bug report template
- **Suggest tools** — new `.agent/tools/*.sh` scripts via PR
- **Improve the agent** — fork, write `agent.vN.c`, run `./eval.sh` to score it, open a PR
- **Web UI** — `web/index.html` and `web/server.swift`

## Development setup

```bash
git clone https://github.com/yukihamada/mini-agent-c
cd mini-agent-c

# Build agent + server
make agent.v11 server

# Run tests
./eval.sh ./agent.v11
```

## Adding a dynamic tool

Create `.agent/tools/my_tool.sh` with these header comments:

```bash
#!/bin/bash
#@name: my_tool
#@description: One line description shown in the UI
# Usage: my_tool <action> [args...]
```

The agent discovers tools automatically at startup.

## Self-evolution workflow

```bash
# Start from the latest version
cp agent.v11.c agent.v12.c

# Edit agent.v12.c — add new capability or fix

# Build and score
make agent.v12
./eval.sh ./agent.v12

# Compare with v11
./eval.sh ./agent.v11
```

Only open a PR for a version that scores ≥ the previous version.

## Commit style

```
feat: short description (≤ 72 chars)
fix: ...
chore: ...
```

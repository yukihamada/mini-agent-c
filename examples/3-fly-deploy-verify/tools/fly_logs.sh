#!/bin/bash
#@name: fly_logs
#@description: Fetch recent Fly.io logs for an app (requires flyctl installed and FLY_API_TOKEN or interactive auth)
#@arg: app:Fly app name (e.g. enablerdao)
#@arg: lines:Number of log lines (e.g. 100)
APP="$1"
N="${2:-100}"
if ! command -v fly >/dev/null 2>&1 && ! command -v flyctl >/dev/null 2>&1; then
    echo "ERROR: flyctl not installed"
    exit 1
fi
FLY_CMD=$(command -v fly || command -v flyctl)
"$FLY_CMD" logs -a "$APP" --no-tail 2>&1 | tail -n "$N"

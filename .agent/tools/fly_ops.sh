#!/bin/bash
#@name: fly_ops
#@description: Fly.io operations: deploy, logs, status, secrets, SSH for any app
#@arg: command:deploy|logs|status|secrets-list|secrets-set|ssh-cmd
#@arg: app:Fly.io app name (e.g. pasha-app, claudeterm, jiuflow-ssr)
#@arg: extra:extra args (for deploy: blank; for logs: --no-tail; for secrets-set: KEY=VAL; for ssh-cmd: command string)
set -e

CMD="$1"
APP="$2"
EXTRA="${@:3}"

if [[ -z "$CMD" || -z "$APP" ]]; then
    echo "Usage: fly_ops.sh <command> <app-name> [extra...]"
    echo "Commands: deploy, logs, status, secrets-list, secrets-set, ssh-cmd"
    echo ""
    echo "Key apps:"
    echo "  claudeterm   → chatweb.ai"
    echo "  pasha-app    → pasha.run"
    echo "  jiuflow-ssr  → jiuflow.art"
    echo "  enablerdao   → enablerdao.com"
    echo "  koe-live     → koe.live"
    echo "  miseban-ai   → misebanai.com"
    echo "  banto-web    → banto.work frontend"
    echo "  yukihamada-jp → yukihamada.jp"
    exit 1
fi

FLY=/opt/homebrew/bin/fly

case "$CMD" in
    deploy)
        echo "Deploying $APP..."
        $FLY deploy --remote-only -a "$APP" $EXTRA
        ;;
    logs)
        $FLY logs -a "$APP" --no-tail 2>&1 | tail -50
        ;;
    status)
        $FLY status -a "$APP"
        ;;
    secrets-list)
        $FLY secrets list -a "$APP"
        ;;
    secrets-set)
        $FLY secrets set $EXTRA -a "$APP"
        ;;
    ssh-cmd)
        $FLY ssh console -a "$APP" --command "$EXTRA"
        ;;
    *)
        echo "Unknown command: $CMD"
        exit 1
        ;;
esac

#!/bin/bash
#@name: self_improve
#@description: Trigger mini-agent self-evolution or check evolution status. Can trigger /evolve endpoint or check current binary/source versions.
#@arg: command:status|evolve|eval-history
set -e

CMD="${1:-status}"
AGENT_URL="https://mini-agent.yukihamada.jp"
AUTH_TOKEN="b9HWZJmh_xoFUHbIHSJuMDT9f4PN7t2Z"

case "$CMD" in
    status)
        echo "=== mini-agent version status ==="
        curl -sS "$AGENT_URL/version" -H "Authorization: Bearer $AUTH_TOKEN" | \
            python3 -m json.tool 2>/dev/null || echo "Server not reachable"
        echo ""
        echo "=== local binaries ==="
        ls -la /Users/yuki/workspace/mini-agent-c/agent.v* 2>/dev/null | grep -v ".c$" | sort -V
        echo ""
        echo "=== source files ==="
        ls -la /Users/yuki/workspace/mini-agent-c/agent.v*.c 2>/dev/null | sort -V
        ;;
    evolve)
        echo "Triggering self-evolution..."
        curl -sS -N -X POST "$AGENT_URL/evolve" \
            -H "Authorization: Bearer $AUTH_TOKEN" \
            -H "Content-Type: application/json" \
            -d '{}' 2>&1 | while IFS= read -r line; do
            echo "$line"
        done
        ;;
    eval-history)
        HIST=/Users/yuki/workspace/mini-agent-c/.agent/eval_history.jsonl
        if [[ -f "$HIST" ]]; then
            echo "=== Evaluation History ==="
            tail -20 "$HIST" | python3 -c "
import sys, json
for line in sys.stdin:
    try:
        d = json.loads(line)
        print(f\"v{d.get('src_v','?')}→v{d.get('tgt_v','?')} score={d.get('score','?')} at {d.get('ts','?')[:19]}\")
    except:
        pass
"
        else
            echo "No eval history yet"
        fi
        ;;
    *)
        echo "Usage: self_improve.sh <status|evolve|eval-history>"
        exit 1
        ;;
esac

#!/bin/bash
# bench_patterns.sh — run mini-agent-c v5 in various patterns,
# measure CPU / RAM / latency / tokens, and check NOU's safety posture.
#
# Usage:
#   ./bench_patterns.sh                  # run all patterns against localhost NOU + anthropic
#   ./bench_patterns.sh --nou-host m5.local:4004
#
# Output: CSV to .agent/bench.csv + summary to stdout

set -u
cd "$(dirname "$0")"

NOU_HOST="${NOU_HOST:-localhost:4004}"
for a in "$@"; do
    case "$a" in
        --nou-host) shift; NOU_HOST="$1"; shift ;;
    esac
done

if [ -z "${ANTHROPIC_API_KEY:-}" ] && [ -f "$HOME/.env" ]; then
    export ANTHROPIC_API_KEY=$(grep '^ANTHROPIC_API_KEY' "$HOME/.env" | sed 's/^ANTHROPIC_API_KEY="\(.*\)"$/\1/')
fi

BINARY="./agent.v5"
[ -x "$BINARY" ] || make agent.v5 >/dev/null 2>&1

OUT_CSV=".agent/bench.csv"
mkdir -p .agent
echo "pattern,backend,model,task,wall_sec,peak_rss_mb,in_tokens,out_tokens,exit,ok" > "$OUT_CSV"

TASK_SIMPLE="Reply with exactly: HI"
TASK_CALC="Use bash to compute 13*17 and reply with ONLY the number"
TASK_FILE="Create /tmp/bench_$$.txt with content 'bench ok', then read it back with bash 'cat', reply OK"

run_pattern() {
    local name="$1" backend="$2" model="$3" task="$4" extra="${5:-}"
    local args
    if [ "$backend" = "openai" ]; then
        args="--backend openai --api-base http://$NOU_HOST"
    else
        args=""
    fi
    local t0=$(python3 -c 'import time; print(time.perf_counter())')
    local log=$(mktemp)
    local rc=0
    # drop --quiet so [done] line with totals is captured in stderr
    /usr/bin/time -l "$BINARY" $args --model "$model" $extra \
        --max-turns 8 --budget 30000 --no-memory "$task" \
        > "$log.out" 2>"$log" || rc=$?
    local t1=$(python3 -c 'import time; print(time.perf_counter())')
    local wall=$(python3 -c "print(f'{$t1-$t0:.2f}')")

    local rss_kb=$(grep -E 'maximum resident set size' "$log" | awk '{print $1}' | tail -1)
    [ -z "$rss_kb" ] && rss_kb=0
    local rss_mb=$((rss_kb / 1048576))

    local out_text=$(cat "$log.out")
    local in_tok=$(grep 'total_in=' "$log" | tail -1 | sed 's/.*total_in=\([0-9]*\).*/\1/')
    local out_tok=$(grep 'total_out=' "$log" | tail -1 | sed 's/.*total_out=\([0-9]*\).*/\1/')
    [ -z "$in_tok" ] && in_tok=0
    [ -z "$out_tok" ] && out_tok=0

    local ok="?"
    case "$task" in
        *"HI"*) echo "$out_text" | grep -qiE '(^|[^a-z])HI([^a-z]|$)' && ok=y || ok=n ;;
        *"13*17"*) echo "$out_text" | grep -q '221' && ok=y || ok=n ;;
        *"bench ok"*) echo "$out_text" | grep -qi 'OK' && ok=y || ok=n ;;
    esac

    echo "$name,$backend,$model,\"${task:0:40}\",$wall,$rss_mb,$in_tok,$out_tok,$rc,$ok" >> "$OUT_CSV"
    printf "%-14s %-9s %-16s wall=%6ss rss=%3sMB tok=%4s/%4s ok=%s\n" \
        "$name" "$backend" "${model:0:16}" "$wall" "$rss_mb" "$in_tok" "$out_tok" "$ok"
    rm -f "$log" "$log.out"
}

echo "=== pre-flight: probe NOU ==="
HEALTH=$(curl -sS --max-time 5 "http://$NOU_HOST/health" 2>/dev/null || echo '{}')
echo "NOU health: $HEALTH"
LOCAL_MODELS=$(curl -sS --max-time 5 "http://$NOU_HOST/v1/models" 2>/dev/null | python3 -c 'import sys,json; d=json.load(sys.stdin); print(",".join(m["id"] for m in d.get("data",[])))' 2>/dev/null || echo "")
echo "NOU models: $LOCAL_MODELS"
NOU_FIRST_MODEL=$(echo "$LOCAL_MODELS" | cut -d, -f1)
[ -z "$NOU_FIRST_MODEL" ] && NOU_FIRST_MODEL="qwen2.5:0.5b"

echo ""
echo "=== running patterns ==="
# Pattern 1: NOU local — simple echo
run_pattern "nou-echo" "openai" "$NOU_FIRST_MODEL" "$TASK_SIMPLE"

# Pattern 2: NOU local — tool call (bash calc)
run_pattern "nou-calc" "openai" "$NOU_FIRST_MODEL" "$TASK_CALC"

# Pattern 3: NOU local — file ops
run_pattern "nou-file" "openai" "$NOU_FIRST_MODEL" "$TASK_FILE"

# Pattern 4: Anthropic — simple echo (baseline)
if [ -n "${ANTHROPIC_API_KEY:-}" ]; then
    run_pattern "anth-echo" "anthropic" "claude-haiku-4-5" "$TASK_SIMPLE"
    run_pattern "anth-calc" "anthropic" "claude-haiku-4-5" "$TASK_CALC"
    run_pattern "anth-file" "anthropic" "claude-sonnet-4-5" "$TASK_FILE"
fi

# Pattern 5: 2 parallel NOU agents (resource pressure)
echo ""
echo "=== parallel (2 concurrent NOU agents) ==="
PIDS=()
for i in 1 2; do
    ( run_pattern "par-$i" "openai" "$NOU_FIRST_MODEL" "$TASK_SIMPLE" ) &
    PIDS+=($!)
done
for p in "${PIDS[@]}"; do wait "$p"; done

echo ""
echo "=== summary (from $OUT_CSV) ==="
column -t -s, "$OUT_CSV" 2>/dev/null || cat "$OUT_CSV"

echo ""
echo "=== system snapshot ==="
uptime
vm_stat | head -5
ps aux | grep -iE 'nou|ollama|agent\.v5' | grep -v grep | head -10

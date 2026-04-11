#!/bin/bash
# evolve.sh - safe eval-driven self-evolution loop.
#
# Flow:
#   1. current = agent.v3 (baseline)
#   2. score baseline via eval.sh, save as baseline_score
#   3. create branch evolve-N
#   4. invoke current agent with self-evolution prompt -> produces agent.v4.c
#   5. build agent.v4
#   6. eval agent.v4 -> candidate_score
#   7. if candidate_score >= baseline_score AND builds cleanly: git commit, promote to current
#      else: git reset --hard, discard candidate
#   8. repeat up to N generations
#
# Safety:
#  - never runs without --yes or interactive confirm for each generation
#  - every step commits to git so rollback is trivial
#  - eval.sh must pass baseline before evolution starts
#  - candidate must match OR EXCEED baseline (tie-break on build success)

set -u
GENERATIONS="${1:-1}"
AUTO_YES="${2:-}"

cd "$(dirname "$0")"

if [ -z "${ANTHROPIC_API_KEY:-}" ]; then
    if [ -f "$HOME/.env" ]; then
        export ANTHROPIC_API_KEY=$(grep '^ANTHROPIC_API_KEY' "$HOME/.env" | sed 's/^ANTHROPIC_API_KEY="\(.*\)"$/\1/')
    fi
fi

if [ ! -d .git ]; then
    echo "[evolve] initializing git repo"
    git init -q
    git add -A
    git commit -qm "evolve: initial snapshot" || true
fi

ask() {
    if [ "$AUTO_YES" = "--yes" ]; then return 0; fi
    read -r -p "$1 [y/N] " a
    [ "$a" = "y" ] || [ "$a" = "Y" ]
}

BASELINE="./agent.v3"
if [ ! -x "$BASELINE" ]; then
    echo "[evolve] baseline binary missing, building"
    make agent.v3 >/dev/null 2>&1 || cc -O2 -Wall -std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -o agent.v3 agent.v3.c cJSON.c -lcurl -lm
fi

echo "[evolve] scoring baseline..."
BASE_RES=$(./eval.sh "$BASELINE")
echo "[evolve] baseline: $BASE_RES"
BASE_SCORE=$(echo "$BASE_RES" | sed -n 's/.*score=\([0-9]*\).*/\1/p')

if [ -z "$BASE_SCORE" ] || [ "$BASE_SCORE" -lt 1 ]; then
    echo "[evolve] REFUSING: baseline score ($BASE_SCORE) too low, aborting for safety"
    exit 2
fi

CURRENT_SRC="agent.v3.c"
CURRENT_BIN="agent.v3"
CURRENT_SCORE="$BASE_SCORE"

for gen in $(seq 1 "$GENERATIONS"); do
    NEXT_SRC="agent.v$((3 + gen)).c"
    NEXT_BIN="agent.v$((3 + gen))"
    BRANCH="evolve-v$((3 + gen))"

    echo ""
    echo "=================================================="
    echo "[evolve] generation $gen: $CURRENT_SRC -> $NEXT_SRC"
    echo "=================================================="

    if ! ask "Proceed with generation $gen?"; then
        echo "[evolve] skipped"
        break
    fi

    git checkout -q -B "$BRANCH" || true

    PROMPT="You are running as $CURRENT_BIN. Evolve yourself to $NEXT_BIN.

Steps (you MUST follow in order):
1. read_file $CURRENT_SRC to see your current source
2. Design ONE concrete improvement. Good choices:
   - Add an SSE streaming mode (parse \`event:\`/\`data:\` lines, print text_delta as it arrives)
   - Improve error messages in tool_bash (show which ulimit fired)
   - Add a new builtin tool 'search_text' that greps a pattern in files under CWD
   - Add retry with exponential backoff instead of fixed 2s
   Pick ONE, don't bite more than you can finish.
3. write_file $NEXT_SRC with the FULL improved source. Keep all existing features.
   Do NOT remove safety checks (path_is_safe, check_dangerous, audit, budget, STOP file).
4. bash: cc -O2 -Wall -std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -o $NEXT_BIN $NEXT_SRC cJSON.c -lcurl -lm
5. If it fails to compile, read the error and fix $NEXT_SRC, rebuild. Up to 3 iterations.
6. bash: ./$NEXT_BIN --max-turns 3 --budget 20000 --quiet 'Create a file called selftest.txt with content ok' then check the file exists.
7. Reply with a summary: what changed, what you tested.

Constraints:
- NEVER touch $CURRENT_SRC (your own running source)
- NEVER modify .agent/sandbox.sb or eval.sh
- NEVER call save_memory (keep noise out of persistent memory during evolution)
- Do NOT use sudo, rm -rf, or network posts
- Budget: keep it under ~50 bash calls total"

    "./$CURRENT_BIN" --max-turns 30 --budget 200000 "$PROMPT" 2>&1 | tee ".agent/evolve_gen${gen}.log"

    if [ ! -f "$NEXT_SRC" ]; then
        echo "[evolve] FAIL: $NEXT_SRC not created"
        git checkout -q - 2>/dev/null || git checkout -q main 2>/dev/null || true
        continue
    fi

    if [ ! -x "$NEXT_BIN" ]; then
        echo "[evolve] trying to build $NEXT_BIN manually"
        cc -O2 -Wall -std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -o "$NEXT_BIN" "$NEXT_SRC" cJSON.c -lcurl -lm 2>&1 | tail -20
    fi

    if [ ! -x "$NEXT_BIN" ]; then
        echo "[evolve] FAIL: $NEXT_BIN not built, reverting"
        git checkout -q -- "$NEXT_SRC" 2>/dev/null || true
        rm -f "$NEXT_SRC"
        git checkout -q - 2>/dev/null || git checkout -q main 2>/dev/null || true
        continue
    fi

    echo "[evolve] scoring candidate $NEXT_BIN..."
    CAND_RES=$(./eval.sh "./$NEXT_BIN")
    echo "[evolve] candidate: $CAND_RES"
    CAND_SCORE=$(echo "$CAND_RES" | sed -n 's/.*score=\([0-9]*\).*/\1/p')

    if [ -z "$CAND_SCORE" ] || [ "$CAND_SCORE" -lt "$CURRENT_SCORE" ]; then
        echo "[evolve] REGRESSION: $CAND_SCORE < $CURRENT_SCORE, reverting"
        rm -f "$NEXT_SRC" "$NEXT_BIN"
        git checkout -q main 2>/dev/null || git checkout -q master 2>/dev/null || true
        git branch -D "$BRANCH" 2>/dev/null || true
        continue
    fi

    echo "[evolve] PROMOTE: $CAND_SCORE >= $CURRENT_SCORE"
    git add -A
    git commit -qm "evolve: $CURRENT_BIN -> $NEXT_BIN (score $CURRENT_SCORE -> $CAND_SCORE)"
    git tag "v$((3 + gen))" 2>/dev/null || true

    CURRENT_SRC="$NEXT_SRC"
    CURRENT_BIN="$NEXT_BIN"
    CURRENT_SCORE="$CAND_SCORE"
done

echo ""
echo "[evolve] final: $CURRENT_BIN score=$CURRENT_SCORE"

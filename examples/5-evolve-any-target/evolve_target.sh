#!/bin/bash
# evolve_target.sh — generic eval-driven evolution of ANY file.
#
# Usage: ./evolve_target.sh <target> <eval-cmd> <goal> [generations]
#
# Safety:
#  - Git commits every promotion (easy rollback: git log / git reset)
#  - Regressions are auto-reverted
#  - Agent runs with budget caps, path confinement, denylist
#  - Target must be inside current repo (no escapes)

set -euo pipefail
cd "$(dirname "$0")"

TARGET="${1:?usage: $0 <target-file> <eval-cmd> <goal> [gens]}"
EVAL_CMD="${2:?need eval command}"
GOAL="${3:?need goal description}"
GENERATIONS="${4:-3}"

REPO_ROOT=$(cd ../.. && pwd)
BINARY="$REPO_ROOT/agent.v4"

if [ ! -x "$BINARY" ]; then
    (cd "$REPO_ROOT" && make agent.v4 >/dev/null)
fi

if [ -z "${ANTHROPIC_API_KEY:-}" ] && [ -f "$HOME/.env" ]; then
    export ANTHROPIC_API_KEY=$(grep '^ANTHROPIC_API_KEY' "$HOME/.env" | sed 's/^ANTHROPIC_API_KEY="\(.*\)"$/\1/')
fi
: "${ANTHROPIC_API_KEY:?}"

[ -f "$TARGET" ] || { echo "target not found: $TARGET"; exit 1; }
TARGET_ABS=$(cd "$(dirname "$TARGET")" && pwd)/$(basename "$TARGET")

# Git init if needed (local evolution only)
if [ ! -d .git ] && [ ! -d "$REPO_ROOT/.git" ]; then
    echo "[evolve] initializing local git"
    (cd "$REPO_ROOT" && git init -q && git add -A && git commit -qm "baseline" || true)
fi

parse_score() {
    # extract last "score=N" from input, fall back to 0
    grep -oE 'score=[0-9]+' | tail -1 | sed 's/score=//' || echo "0"
}

echo "[evolve] scoring baseline ($TARGET)..."
BASE_RES=$($EVAL_CMD "$TARGET" 2>&1)
echo "[evolve] baseline: $BASE_RES"
BASE_SCORE=$(echo "$BASE_RES" | parse_score)
[ -z "$BASE_SCORE" ] && BASE_SCORE=0

CURRENT_SCORE="$BASE_SCORE"

for gen in $(seq 1 "$GENERATIONS"); do
    # Preserve the extension so eval harnesses that care about it (importlib, etc) keep working.
    T_DIR=$(dirname "$TARGET")
    T_BASE=$(basename "$TARGET")
    T_EXT="${T_BASE##*.}"
    T_STEM="${T_BASE%.*}"
    CANDIDATE="${T_DIR}/${T_STEM}.gen${gen}.${T_EXT}"
    echo ""
    echo "=== generation $gen: $TARGET -> $CANDIDATE ==="

    cp "$TARGET" "$CANDIDATE"

    PROMPT="You are optimizing a file via surgical edits.

TARGET: $CANDIDATE
GOAL: $GOAL
EVAL COMMAND: $EVAL_CMD $CANDIDATE

Rules:
1. First, read the target with read_file to understand the current state.
2. Optionally run the eval once on the current candidate to see baseline output via bash.
3. Design ONE concrete improvement that should increase the score.
4. Apply it via bash (sed/awk/python). Keep the edit small and targeted.
5. Run the eval with bash: $EVAL_CMD $CANDIDATE — check that the new score is higher.
6. If the score went DOWN or stayed the same, try ONE more adjustment.
7. If after 2 attempts the score isn't improving, give up gracefully.
8. Respond with a one-line summary: 'score: X -> Y, change: <what you did>'.

Hard constraints:
- NEVER touch any file except $CANDIDATE
- NEVER call save_memory
- NEVER use sudo, rm -rf, or network posts
- Budget yourself: <15 bash calls"

    # Tolerate non-zero exit from agent (max-turns halt, etc) — we care about the candidate file
    { "$BINARY" --max-turns 15 --budget 80000 "$PROMPT" 2>&1 || true; } | tee "/tmp/evolve_gen${gen}.log"

    if [ ! -f "$CANDIDATE" ]; then
        echo "[evolve] FAIL: candidate missing"
        continue
    fi

    echo "[evolve] scoring generation $gen..."
    CAND_RES=$($EVAL_CMD "$CANDIDATE" 2>&1)
    echo "[evolve] candidate: $CAND_RES"
    CAND_SCORE=$(echo "$CAND_RES" | parse_score)
    [ -z "$CAND_SCORE" ] && CAND_SCORE=0

    if [ "$CAND_SCORE" -gt "$CURRENT_SCORE" ]; then
        echo "[evolve] PROMOTE: $CURRENT_SCORE -> $CAND_SCORE"
        cp "$CANDIDATE" "$TARGET"
        CURRENT_SCORE="$CAND_SCORE"
        (cd "$REPO_ROOT" && git add -A 2>/dev/null && \
         git commit -qm "evolve-target: $(basename $TARGET) gen${gen} score $BASE_SCORE -> $CAND_SCORE" 2>/dev/null || true)
    else
        echo "[evolve] NO IMPROVEMENT: $CAND_SCORE <= $CURRENT_SCORE — discarding"
        rm -f "$CANDIDATE"
    fi
done

echo ""
echo "[evolve] final $TARGET: score=$CURRENT_SCORE (baseline was $BASE_SCORE)"

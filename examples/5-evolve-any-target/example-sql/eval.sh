#!/bin/bash
# eval.sh <target.sql> — score = (100000 / median_us) clamped
# Also verifies result set matches reference.
set -e
TARGET="${1:-target.sql}"
DB="/tmp/evolve_sql.db"
REF="/tmp/evolve_sql_ref.txt"

if [ ! -f "$DB" ]; then
    bash "$(dirname "$0")/setup_db.sh" >/dev/null
fi

# Generate reference result set from baseline target.sql (first run only)
if [ ! -f "$REF" ]; then
    sqlite3 "$DB" < "$(dirname "$0")/target.sql" > "$REF"
fi

# Correctness check
CURRENT=$(sqlite3 "$DB" < "$TARGET" 2>&1)
REFERENCE=$(cat "$REF")
if [ "$CURRENT" != "$REFERENCE" ]; then
    echo "CORRECTNESS_FAIL: result set differs from reference"
    echo "score=0"
    exit 0
fi

# Benchmark: run 7 times, pick median. Supports multi-statement files:
# any statement before the final SELECT (e.g. CREATE INDEX) runs via
# executescript() untimed, then the SELECT is timed in isolation.
TIMES=()
for i in 1 2 3 4 5 6 7; do
    T=$(python3 - "$TARGET" "$DB" <<'PY'
import sqlite3, time, re, sys
target_path, db_path = sys.argv[1], sys.argv[2]
with open(target_path) as f:
    raw = f.read()
# Strip line comments (-- ...) before splitting
lines = [l for l in raw.split('\n') if not l.strip().startswith('--')]
text = '\n'.join(lines)
# Split on ; followed by newline or end
parts = [s.strip() for s in re.split(r';\s*(?=\n|$)', text) if s.strip()]
# Find final SELECT
select_idx = -1
for i, s in enumerate(parts):
    if re.match(r'\s*SELECT\b', s, re.IGNORECASE):
        select_idx = i
if select_idx < 0:
    # No SELECT found, time the whole thing
    c = sqlite3.connect(db_path)
    t0 = time.perf_counter_ns()
    c.executescript(raw)
    print((time.perf_counter_ns() - t0) // 1000)
else:
    c = sqlite3.connect(db_path)
    # Run pre-statements untimed
    for s in parts[:select_idx]:
        try:
            c.executescript(s + ';')
        except Exception as e:
            print(f"ERR: {e}", file=sys.stderr)
    final = parts[select_idx]
    t0 = time.perf_counter_ns()
    list(c.execute(final))
    print((time.perf_counter_ns() - t0) // 1000)
PY
)
    TIMES+=("$T")
done
MEDIAN=$(printf '%s\n' "${TIMES[@]}" | sort -n | awk 'NR==4')
# score: inverse of median_us, normalized so ~1s baseline -> ~10, ~100ms -> ~100
SCORE=$(( 10000000 / (MEDIAN + 1) ))
echo "median_us=$MEDIAN runs=${TIMES[*]} score=$SCORE"

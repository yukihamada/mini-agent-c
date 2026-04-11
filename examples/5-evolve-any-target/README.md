# evolve-target: retarget the evolution loop at ANY file

Generalization of the agent's self-evolution: point `evolve_target.sh` at an arbitrary
file + eval script, and it will iteratively improve the file while the eval score
only ever goes up (regressions are git-reset).

## Usage

```bash
./evolve_target.sh <target-file> <eval-command> <goal-description> [generations]
```

Example:
```bash
./evolve_target.sh \
  example-sql/target.sql \
  "./example-sql/eval.sh" \
  "Optimize this SQL query for performance while keeping results identical" \
  3
```

## How it works

1. Score baseline: run `<eval-command> <target-file>` → parse `score=N`
2. For each generation:
   a. Create a candidate copy of the target
   b. Invoke the agent with goal + surgical-edit instructions
   c. Score the candidate
   d. If score > baseline: git commit + promote
   e. Else: revert
3. Report the final winning version

## Eval contract

Your eval script must:
- Accept `<target-file>` as argv[1]
- Print one line ending in `score=N` (where N is an integer, higher = better)
- Exit 0 on successful eval (regardless of score)

Example eval.sh:
```bash
#!/bin/bash
TARGET="$1"
# ... run tests / measure ... 
echo "score=42"
```

## Example: SQL optimization (included)

`example-sql/` has:
- `target.sql` — a deliberately-slow query
- `eval.sh` — runs query against a tiny sqlite DB, measures time in µs, scores inversely
- `setup_db.sh` — creates the test DB with 10,000 rows

Run it:
```bash
./example-sql/setup_db.sh
./evolve_target.sh example-sql/target.sql "./example-sql/eval.sh" \
  "Rewrite this SQL to run faster on the sqlite DB at /tmp/evolve_sql.db. The result set must remain byte-identical." \
  2
```

#!/bin/bash
# eval.sh <target.py> — run 10 unit tests, score = # passing
set -e
TARGET="${1:?}"
python3 - "$TARGET" <<'PY'
import sys, importlib.util, traceback
spec = importlib.util.spec_from_file_location("target", sys.argv[1])
mod = importlib.util.module_from_spec(spec)
try:
    spec.loader.exec_module(mod)
except Exception as e:
    print(f"LOAD_ERROR: {e}")
    print("score=0")
    sys.exit(0)

tests = [
    ("hello world", 2),
    ("one", 1),
    ("", 0),
    (None, 0),
    ("  leading and trailing  ", 3),
    ("multiple   spaces   between", 3),
    ("\ttab\tseparated\twords", 3),
    ("mixed\n\nlines\nhere", 3),
    ("single", 1),
    ("a b c d e f g h i j", 10),
]
passed = 0
for i, (inp, expected) in enumerate(tests):
    try:
        got = mod.word_count(inp)
        if got == expected:
            passed += 1
        else:
            print(f"  test {i}: input={inp!r} expected={expected} got={got}")
    except Exception as e:
        print(f"  test {i}: input={inp!r} raised {type(e).__name__}: {e}")
print(f"passed={passed}/10 score={passed}")
PY

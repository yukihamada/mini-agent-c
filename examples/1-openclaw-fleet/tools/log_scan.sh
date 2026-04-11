#!/bin/bash
#@name: log_scan
#@description: Scan recent journalctl entries for errors/warnings
#@arg: minutes:Lookback window in minutes (e.g. 30)
MIN="${1:-30}"
if ! command -v journalctl >/dev/null 2>&1; then
    echo "(journalctl not available)"
    exit 0
fi
echo "== errors last ${MIN}min =="
journalctl --since "${MIN} min ago" -p err --no-pager -q 2>/dev/null | tail -30
echo "== warnings last ${MIN}min (count only) =="
journalctl --since "${MIN} min ago" -p warning --no-pager -q 2>/dev/null | wc -l

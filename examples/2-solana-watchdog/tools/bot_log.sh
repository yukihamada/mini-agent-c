#!/bin/bash
#@name: bot_log
#@description: Tail a liquidation bot log and count errors/warnings
#@arg: bot:marginfi|kamino|drift
#@arg: lines:how many lines to tail (e.g. 100)
BOT="$1"
N="${2:-100}"
case "$BOT" in
    marginfi|kamino|drift) ;;
    *) echo "ERROR: unknown bot '$BOT' (allowed: marginfi|kamino|drift)"; exit 1 ;;
esac
# Adjust these paths to your actual log locations
LOGFILE="${LOG_DIR:-/var/log}/liq-${BOT}.log"
if [ ! -f "$LOGFILE" ]; then
    # try alt locations
    for alt in "/opt/$BOT/bot.log" "/opt/liquidator/${BOT}.log" "/root/${BOT}.log"; do
        [ -f "$alt" ] && LOGFILE="$alt" && break
    done
fi
if [ ! -f "$LOGFILE" ]; then
    echo "ERROR: no log file found for $BOT"
    exit 1
fi
TOTAL=$(tail -n "$N" "$LOGFILE" 2>/dev/null | wc -l)
ERR=$(tail -n "$N" "$LOGFILE" 2>/dev/null | grep -ic -E 'error|fatal|panic' || true)
WARN=$(tail -n "$N" "$LOGFILE" 2>/dev/null | grep -ic -E 'warn' || true)
LAST_LINE=$(tail -n 1 "$LOGFILE" 2>/dev/null)
echo "bot=$BOT logfile=$LOGFILE lines=$TOTAL errors=$ERR warnings=$WARN"
echo "last: $LAST_LINE"
echo "--- recent errors ---"
tail -n "$N" "$LOGFILE" 2>/dev/null | grep -iE 'error|fatal|panic' | tail -5

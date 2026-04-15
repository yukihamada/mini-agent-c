#!/bin/bash
#@name: power_info
#@description: Show battery status and estimated electricity cost since session start
# Returns battery percentage, charging status, remaining time, and ¥/kWh cost estimate

# Battery info
BATT=$(pmset -g batt 2>/dev/null)
echo "$BATT" | head -1

BATT_LINE=$(echo "$BATT" | grep "InternalBattery")
if [ -n "$BATT_LINE" ]; then
    PCT=$(echo "$BATT_LINE" | grep -oE '[0-9]+%')
    STATUS=$(echo "$BATT_LINE" | grep -oE '(discharging|charging|finishing charge)')
    REMAIN=$(echo "$BATT_LINE" | grep -oE '[0-9]+:[0-9]+ remaining')
    echo "Battery: $PCT ($STATUS${REMAIN:+, $REMAIN})"
else
    echo "Battery: AC only (no battery detected)"
fi

# Electricity estimate: M5 Mac under LLM load ~25W, Japan rate ¥31/kWh
# SESSION_START_EPOCH is optionally set by the agent
if [ -n "$SESSION_START_EPOCH" ]; then
    NOW=$(date +%s)
    ELAPSED_MIN=$(( (NOW - SESSION_START_EPOCH) / 60 ))
    KWH=$(echo "scale=4; 25 * $ELAPSED_MIN / 60 / 1000" | bc 2>/dev/null || echo "?")
    COST=$(echo "scale=2; $KWH * 31" | bc 2>/dev/null || echo "?")
    echo "Electricity: ~¥$COST (${ELAPSED_MIN}min × 25W × ¥31/kWh)"
fi

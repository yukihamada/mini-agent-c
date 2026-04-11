#!/bin/bash
#@name: service_status
#@description: Check systemd/docker service state on this host
#@arg: filter:Optional name filter (empty = all)
FILTER="${1:-}"
echo "== systemd failed =="
systemctl --failed --no-pager --no-legend 2>/dev/null | head -20 || echo "(systemctl not available)"
echo "== systemd running ($FILTER) =="
if [ -n "$FILTER" ]; then
    systemctl list-units --type=service --state=running --no-pager --no-legend 2>/dev/null \
      | grep -i "$FILTER" | head -20
else
    systemctl list-units --type=service --state=running --no-pager --no-legend 2>/dev/null | head -10
fi
echo "== docker ps =="
docker ps --format 'table {{.Names}}\t{{.Status}}' 2>/dev/null | head -20 || echo "(docker not available)"

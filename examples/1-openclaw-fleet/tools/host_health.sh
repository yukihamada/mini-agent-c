#!/bin/bash
#@name: host_health
#@description: Get disk/cpu/memory/load for this host in one dense report
echo "== disk =="
df -h / 2>/dev/null | awk 'NR<=2'
echo "== memory =="
free -h 2>/dev/null || vm_stat | head -5
echo "== load =="
uptime
echo "== cpu (top5) =="
ps aux --sort=-%cpu 2>/dev/null | head -6 || ps -A -o pid,pcpu,comm | sort -k2 -nr | head -6
echo "== mem (top5) =="
ps aux --sort=-%mem 2>/dev/null | head -6 || ps -A -o pid,pmem,comm | sort -k2 -nr | head -6

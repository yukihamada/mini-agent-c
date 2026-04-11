#!/bin/bash
#@name: http_get
#@description: Fetch a URL via curl and return the first 4KB of response
#@arg: url:HTTPS URL to fetch
set -e
URL="$1"
if [[ ! "$URL" =~ ^https:// ]]; then
    echo "Error: only https:// URLs allowed"
    exit 1
fi
curl -sS --max-time 15 -L "$URL" | head -c 4096

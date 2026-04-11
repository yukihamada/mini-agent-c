#!/bin/bash
#@name: http_check
#@description: Fetch a URL, return status code, headers, and first 4KB body. Use for post-deploy smoke tests.
#@arg: url:Full URL (http:// or https://)
URL="$1"
[ -z "$URL" ] && { echo "ERROR: no url"; exit 1; }
RESP=$(curl -sS -L --max-time 15 -w '\n__HTTP_CODE__%{http_code}\n__TIME__%{time_total}\n__SIZE__%{size_download}\n' \
    -D - "$URL" 2>&1 || echo "__CURL_ERROR__")
echo "$RESP" | head -c 4096

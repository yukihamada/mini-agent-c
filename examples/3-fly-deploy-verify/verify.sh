#!/bin/bash
# verify.sh <app-name> <url1> [url2 ...]
set -euo pipefail
cd "$(dirname "$0")"

APP="${1:?usage: $0 <app-name> <url...>}"
shift
URLS="$*"
[ -z "$URLS" ] && { echo "usage: $0 <app> <url1> [url2 ...]"; exit 1; }

REPO_ROOT=$(cd ../.. && pwd)
BINARY="$REPO_ROOT/agent.v4"
[ -x "$BINARY" ] || (cd "$REPO_ROOT" && make agent.v4 >/dev/null)

if [ -z "${ANTHROPIC_API_KEY:-}" ] && [ -f "$HOME/.env" ]; then
    export ANTHROPIC_API_KEY=$(grep '^ANTHROPIC_API_KEY' "$HOME/.env" | sed 's/^ANTHROPIC_API_KEY="\(.*\)"$/\1/')
fi
: "${ANTHROPIC_API_KEY:?}"

# temp workspace with tools
WORKDIR=$(mktemp -d -t fly-verify.XXXXXX)
trap 'rm -rf "$WORKDIR"' EXIT
mkdir -p "$WORKDIR/.agent/tools"
cp tools/*.sh "$WORKDIR/.agent/tools/"
chmod +x "$WORKDIR/.agent/tools/"*.sh
cd "$WORKDIR"

PROMPT="You are a post-deploy verifier for the Fly.io app '$APP'.

Endpoints to check: $URLS

Steps:
1. For each URL, call http_check and parse the status code.
2. A URL is HEALTHY if: HTTP status is 2xx or 3xx, and response body is non-empty.
3. If ALL endpoints are healthy: respond with exactly 'VERIFY_OK: $APP' and nothing else.
4. If ANY endpoint fails:
   - Call fly_logs '$APP' 80 to fetch recent logs
   - Analyze the logs for errors, crashes, panic, 5xx patterns
   - Respond with 'VERIFY_FAIL: $APP' on the first line,
     then a one-paragraph root-cause diagnosis with the specific error quoted.

Do not call any other tools. Be terse."

OUTPUT=$("$BINARY" --max-turns 10 --budget 30000 --quiet "$PROMPT" 2>&1)
echo "$OUTPUT"
if echo "$OUTPUT" | grep -q "VERIFY_OK:"; then
    exit 0
else
    exit 1
fi

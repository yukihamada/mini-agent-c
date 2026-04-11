#!/bin/bash
#@name: rpc_health
#@description: Check Solana RPC current slot and compare to public mainnet tip to detect lag
set -e
: "${SOLANA_RPC_URL:=https://api.mainnet-beta.solana.com}"
PUBLIC="https://api.mainnet-beta.solana.com"

rpc_slot() {
    curl -sS --max-time 8 "$1" \
        -H 'content-type: application/json' \
        -d '{"jsonrpc":"2.0","id":1,"method":"getSlot"}' \
      | python3 -c 'import sys,json; print(json.load(sys.stdin).get("result",-1))'
}

LOCAL=$(rpc_slot "$SOLANA_RPC_URL")
TIP=$(rpc_slot "$PUBLIC")
if [ -z "$LOCAL" ] || [ -z "$TIP" ] || [ "$LOCAL" = "-1" ]; then
    echo "ERROR: rpc unreachable (local=$LOCAL tip=$TIP)"
    exit 1
fi
LAG=$((TIP - LOCAL))
echo "local_slot=$LOCAL public_slot=$TIP lag=$LAG rpc=$SOLANA_RPC_URL"

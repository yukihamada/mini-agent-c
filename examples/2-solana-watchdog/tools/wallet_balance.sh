#!/bin/bash
#@name: wallet_balance
#@description: Get SOL balance of the gas wallet (public read-only query)
#@arg: address:Solana pubkey (optional, defaults to $WALLET_ADDRESS env)
set -e
ADDR="${1:-$WALLET_ADDRESS}"
: "${ADDR:?no address}"
: "${SOLANA_RPC_URL:=https://api.mainnet-beta.solana.com}"
RESULT=$(curl -sS --max-time 8 "$SOLANA_RPC_URL" \
    -H 'content-type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getBalance\",\"params\":[\"$ADDR\"]}")
LAMPORTS=$(echo "$RESULT" | python3 -c 'import sys,json; print(json.load(sys.stdin).get("result",{}).get("value",0))' 2>/dev/null)
if [ -z "$LAMPORTS" ] || [ "$LAMPORTS" = "0" ]; then
    echo "address=$ADDR balance_sol=0 raw=$RESULT"
    exit 0
fi
SOL=$(python3 -c "print(f'{$LAMPORTS/1e9:.4f}')")
echo "address=$ADDR balance_sol=$SOL lamports=$LAMPORTS"

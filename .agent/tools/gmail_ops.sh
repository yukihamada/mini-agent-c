#!/bin/bash
#@name: gmail_ops
#@description: Gmail operations via gog CLI: search, read, send email for owner (yuki@hamada.tokyo)
#@arg: command:search|get|send
#@arg: args:For search: query string. For get: message ID. For send: --to addr --subject text --body text
set -e

CMD="$1"
shift

GOG=/opt/homebrew/bin/gog
ACCOUNT="yuki@hamada.tokyo"

case "$CMD" in
    search)
        QUERY="$*"
        if [[ -z "$QUERY" ]]; then
            QUERY="is:unread newer_than:1d"
        fi
        $GOG gmail search "$QUERY" --account "$ACCOUNT" --plain
        ;;
    get)
        MSG_ID="$1"
        if [[ -z "$MSG_ID" ]]; then
            echo "Usage: gmail_ops.sh get <messageId>"
            exit 1
        fi
        $GOG gmail get "$MSG_ID" --account "$ACCOUNT"
        ;;
    send)
        # Pass all remaining args to gog
        $GOG gmail send --account "$ACCOUNT" "$@"
        ;;
    unread)
        $GOG gmail search "is:unread is:important" --account "$ACCOUNT" --plain | head -30
        ;;
    *)
        echo "Usage: gmail_ops.sh <search|get|send|unread> [args...]"
        echo ""
        echo "Examples:"
        echo "  gmail_ops.sh search 'from:example.com'"
        echo "  gmail_ops.sh unread"
        echo "  gmail_ops.sh get <messageId>"
        echo "  gmail_ops.sh send --to 'to@example.com' --subject 'Hello' --body 'World'"
        exit 1
        ;;
esac

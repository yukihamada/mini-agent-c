#!/bin/bash
#@name: sqlite_ops
#@description: SQLite database operations — query, insert, schema inspection
# Usage: sqlite_ops <db_path> <sql_query>
# Examples: sqlite_ops ./data.db "SELECT * FROM users LIMIT 5"
#           sqlite_ops ./data.db ".tables"

set -euo pipefail

DB_PATH="${1:-}"
SQL_QUERY="${2:-}"

# Check sqlite3 is installed
if ! command -v sqlite3 &>/dev/null; then
    echo "ERROR: sqlite3 is not installed. Install with: brew install sqlite3" >&2
    exit 1
fi

# Validate arguments
if [ -z "$DB_PATH" ]; then
    echo "ERROR: db_path is required." >&2
    echo "Usage: sqlite_ops <db_path> <sql_query>" >&2
    echo "Examples:" >&2
    echo "  sqlite_ops ./data.db \"SELECT * FROM users LIMIT 5\"" >&2
    echo "  sqlite_ops ./data.db \".tables\"" >&2
    exit 1
fi

if [ -z "$SQL_QUERY" ]; then
    echo "ERROR: sql_query is required." >&2
    echo "Usage: sqlite_ops <db_path> <sql_query>" >&2
    exit 1
fi

# Check the database file exists (allow creation for new DBs only with explicit intent)
if [ ! -f "$DB_PATH" ]; then
    echo "WARNING: Database file '$DB_PATH' does not exist. It will be created if the query writes data." >&2
fi

# Run the query with column headers enabled for SELECT-like output
sqlite3 -column -header "$DB_PATH" "$SQL_QUERY"

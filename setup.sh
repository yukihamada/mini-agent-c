#!/bin/bash
# setup.sh — one-shot setup for mini-agent-c
# Checks dependencies, builds binaries, configures environment.
# Usage: ./setup.sh

set -euo pipefail

cd "$(dirname "$0")"

RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[1;33m'
BLD='\033[1m'
RST='\033[0m'

ok()   { printf "${GRN}[ok]${RST}  %s\n" "$1"; }
warn() { printf "${YLW}[warn]${RST} %s\n" "$1"; }
fail() { printf "${RED}[fail]${RST} %s\n" "$1"; }

echo ""
printf "${BLD}=== mini-agent-c setup ===${RST}\n"
echo ""

# ── 1. Check required tools ──────────────────────────────────────────────────
echo "Checking required tools..."
MISSING=0

check_tool() {
    local tool="$1"
    local hint="${2:-}"
    if command -v "$tool" &>/dev/null; then
        ok "$tool  ($(command -v "$tool"))"
    else
        fail "$tool not found${hint:+ — $hint}"
        MISSING=$((MISSING + 1))
    fi
}

check_tool cc      "install Xcode CLT: xcode-select --install"
check_tool swiftc  "install Xcode CLT: xcode-select --install"
check_tool curl    "brew install curl"
check_tool sqlite3 "brew install sqlite3"
check_tool docker  "install Docker Desktop from https://docker.com" || true  # docker is optional

if command -v docker &>/dev/null; then
    if docker info &>/dev/null 2>&1; then
        ok "docker daemon running"
    else
        warn "docker installed but daemon is not running — start Docker Desktop"
    fi
fi

if [ "$MISSING" -gt 0 ]; then
    echo ""
    fail "$MISSING required tool(s) missing. Please install them and re-run setup.sh."
    exit 1
fi

echo ""

# ── 2. Build agent.v11 if not present ────────────────────────────────────────
echo "Checking agent binary..."
if [ ! -x "./agent.v11" ]; then
    echo "  Building agent.v11..."
    cc -O2 -Wall -Wno-unused-result -Wno-comment -std=c99 \
        -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE \
        -o agent.v11 agent.v11.c cJSON.c -lcurl -lm
    ok "agent.v11 built"
else
    ok "agent.v11 already exists"
fi

# ── 3. Build web/server-swift if not present ────────────────────────────────
echo "Checking web server binary..."
if [ ! -x "./web/server-swift" ]; then
    echo "  Building web/server-swift (this may take ~30s)..."
    swiftc -O web/server.swift -o web/server-swift
    ok "web/server-swift built"
else
    ok "web/server-swift already exists"
fi

# ── 4. Create .agent/tools/ directory ───────────────────────────────────────
if [ ! -d ".agent/tools" ]; then
    mkdir -p .agent/tools
    ok ".agent/tools/ created"
else
    ok ".agent/tools/ exists"
fi

# Make sure dynamic tool scripts are executable
find .agent/tools -name "*.sh" -exec chmod +x {} \; 2>/dev/null && true

# ── 5. Generate token if not already set ────────────────────────────────────
TOKEN_FILE=".agent/.web_token"
if [ -n "${MINI_AGENT_WEB_TOKEN:-}" ]; then
    TOKEN="$MINI_AGENT_WEB_TOKEN"
    ok "MINI_AGENT_WEB_TOKEN already set in environment"
elif [ -f "$TOKEN_FILE" ]; then
    TOKEN="$(cat "$TOKEN_FILE")"
    ok "Loaded existing token from $TOKEN_FILE"
else
    TOKEN="$(LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 32 2>/dev/null || openssl rand -hex 16)"
    echo "$TOKEN" > "$TOKEN_FILE"
    chmod 600 "$TOKEN_FILE"
    ok "Generated new token → saved to $TOKEN_FILE"
fi

echo ""
printf "${BLD}=== Setup complete! ===${RST}\n"
echo ""
printf "  Run with:  ${BLD}MINI_AGENT_WEB_TOKEN=%s make run${RST}\n" "$TOKEN"
echo ""
echo "  Or:        MINI_AGENT_WEB_TOKEN=$TOKEN ./web/run.sh"
echo ""
echo "  Token file: $TOKEN_FILE  (chmod 600, git-ignored)"
echo ""

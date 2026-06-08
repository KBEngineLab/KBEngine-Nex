#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN_DIR="$SCRIPT_DIR/../../kbe/bin/server"

if [[ ! -d "$BIN_DIR" ]]; then
    echo "❌ 目录不存在: $BIN_DIR"
    exit 1
fi

BINS=(baseapp baseappmgr bots cellapp cellappmgr dbmgr interfaces kbcmd logger loginapp machine)

echo "🔧 批量移除 quarantine: $BIN_DIR"
for name in "${BINS[@]}"; do
    bin="$BIN_DIR/$name"
    if [[ -f "$bin" ]]; then
        echo "  处理: $name"
        sudo xattr -rd com.apple.quarantine "$bin" 2>/dev/null || true
    fi
done

echo "✅ 完成。"

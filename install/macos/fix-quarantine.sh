#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KBE_DIR="$SCRIPT_DIR/../../kbe"

if [[ ! -d "$KBE_DIR" ]]; then
    echo "❌ 目录不存在: $KBE_DIR"
    exit 1
fi

echo "🔧 递归移除整个 kbe 目录的 quarantine 属性..."
sudo xattr -rd com.apple.quarantine "$KBE_DIR"

echo "✅ 完成。"

#!/usr/bin/env bash
set -euo pipefail

CONFIGURATION="${1:-Release}"
ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SOURCE_DIR="$ROOT_DIR/kbe/src"
VCPKG_ROOT="$ROOT_DIR/kbe/vcpkg"
export VCPKG_ROOT

require_command() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "[ERROR] 未找到 $1，请先安装 Git、CMake、Ninja 和 C++ 编译器。" >&2
        exit 1
    }
}

for tool in git cmake ninja; do require_command "$tool"; done
if [ "$CONFIGURATION" != "Debug" ] && [ "$CONFIGURATION" != "Release" ]; then
    echo "[ERROR] 配置必须是 Debug 或 Release。" >&2
    exit 2
fi


if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
    repository="${KBE_VCPKG_REPOSITORY:-https://github.com/microsoft/vcpkg.git}"
    echo "[INFO] 克隆 vcpkg 到 $VCPKG_ROOT"
    git clone "$repository" "$VCPKG_ROOT"
    if [ -n "${KBE_VCPKG_REF:-}" ]; then
        git -C "$VCPKG_ROOT" checkout "$KBE_VCPKG_REF"
    fi
    "$VCPKG_ROOT/bootstrap-vcpkg.sh"
fi



cd "$SOURCE_DIR"
cmake --fresh --preset linux-ninja
cmake --build "$SOURCE_DIR/out/build/linux-ninja" --config "$CONFIGURATION" \
    --target kbe_runtime kbe_servers kbe_tests --parallel

echo "[SUCCESS] Task completed."
echo "[OUTPUT] CMake build directory: kbe/src/out/build/linux-ninja"
echo "[OUTPUT] Runtime directory: kbe/bin/server"
if [ -z "${CI:-}" ] && [ -t 0 ] && [ -t 1 ]; then
    read -n 1 -s -r -p "Press any key to exit..."
    echo
fi

#!/bin/sh
set -eu

CONFIGURATION="${1:-Release}"
ROOT_DIR="$(CDPATH= cd "$(dirname "$0")/.." && pwd)"
SOURCE_DIR="$ROOT_DIR/kbe/src"
VCPKG_ROOT="$ROOT_DIR/kbe/vcpkg"
export VCPKG_ROOT

error() {
    echo "[ERROR] $*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || error "未找到必需工具: $1"
}

ensure_xcode_command_line_tools() {
    command -v xcode-select >/dev/null 2>&1 || error "未找到 xcode-select，请先安装 Xcode Command Line Tools。"
    if ! xcode-select -p >/dev/null 2>&1; then
        echo "[INFO] 请运行 xcode-select --install，完成安装后重新执行本脚本。" >&2
        exit 1
    fi

    require_command xcrun
    xcrun --find clang >/dev/null 2>&1 || error "Xcode Command Line Tools 中缺少 clang。"
    xcrun --find clang++ >/dev/null 2>&1 || error "Xcode Command Line Tools 中缺少 clang++。"
}

install_brew_dependency() {
    kbe_brew_formula="$1"
    if brew list --formula "$kbe_brew_formula" >/dev/null 2>&1; then
        echo "[INFO] $kbe_brew_formula 已安装"
        return
    fi

    echo "[INFO] 通过 Homebrew 安装 $kbe_brew_formula"
    brew install "$kbe_brew_formula"
}

install_system_dependencies() {
    require_command brew

    echo "[INFO] 检查 KBEngine 和 vcpkg 所需的宿主工具。"
    for kbe_system_formula in cmake ninja autoconf automake libtool pkg-config bison flex m4 perl libtirpc; do
        install_brew_dependency "$kbe_system_formula"
    done
}

configure_homebrew_environment() {
    if ! command -v brew >/dev/null 2>&1; then
        return
    fi

    # Homebrew 的部分构建工具是 keg-only，显式加入 PATH，确保 vcpkg 不会误用系统旧版本。
    # Some Homebrew build tools are keg-only; prepend them so vcpkg does not select older system tools.
    for kbe_environment_formula in bison flex libtool m4 perl; do
        if kbe_formula_prefix="$(brew --prefix "$kbe_environment_formula" 2>/dev/null)"; then
            export PATH="$kbe_formula_prefix/bin:$PATH"
        fi
    done

    if kbe_formula_prefix="$(brew --prefix libtirpc 2>/dev/null)"; then
        export PKG_CONFIG_PATH="$kbe_formula_prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
        export CPPFLAGS="-I$kbe_formula_prefix/include ${CPPFLAGS:-}"
        export LDFLAGS="-L$kbe_formula_prefix/lib ${LDFLAGS:-}"
    fi

}

verify_system_dependencies() {
    for kbe_required_tool in git curl zip unzip tar patch file cmake ninja pkg-config make autoconf automake glibtoolize bison flex m4 perl; do
        require_command "$kbe_required_tool"
    done

    pkg-config --exists libtirpc || error "pkg-config 无法找到 Homebrew 的 libtirpc。"
}

ensure_vcpkg() {
    kbe_vcpkg_repository="${KBE_VCPKG_REPOSITORY:-https://github.com/microsoft/vcpkg.git}"
    kbe_vcpkg_cloned=0

    if [ ! -d "$VCPKG_ROOT/.git" ]; then
        if [ -e "$VCPKG_ROOT" ]; then
            error "$VCPKG_ROOT 已存在但不是 Git 工作区，请移走该目录后重试。"
        fi

        echo "[INFO] 克隆 vcpkg 到 $VCPKG_ROOT"
        git clone "$kbe_vcpkg_repository" "$VCPKG_ROOT"
        kbe_vcpkg_cloned=1
    fi

    # 只在首次克隆时检出指定版本，避免安装脚本覆盖已有 vcpkg 工作区的本地状态。
    # Checkout is limited to the initial clone so an existing vcpkg worktree is never overwritten.
    if [ "$kbe_vcpkg_cloned" -eq 1 ] && [ -n "${KBE_VCPKG_REF:-}" ]; then
        git -C "$VCPKG_ROOT" checkout "$KBE_VCPKG_REF"
    fi

    if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
        [ -x "$VCPKG_ROOT/bootstrap-vcpkg.sh" ] || error "vcpkg 缺少 bootstrap-vcpkg.sh。"
        "$VCPKG_ROOT/bootstrap-vcpkg.sh"
    fi
}

cmake_meets_minimum_version() {
    kbe_cmake_version="$("$1" --version | awk 'NR == 1 { print $3 }')"
    awk -v version="$kbe_cmake_version" 'BEGIN {
        count = split(version, parts, ".")
        valid = count >= 2 && parts[1] ~ /^[0-9]+$/ && parts[2] ~ /^[0-9]+$/
        supported = parts[1] > 3 || (parts[1] == 3 && parts[2] >= 25)
        exit !(valid && supported)
    }'
}

if [ "$(uname -s)" != "Darwin" ]; then
    error "该脚本仅支持 macOS，请在 Linux 上运行 install_linux.sh。"
fi

if [ "$CONFIGURATION" != "Debug" ] && [ "$CONFIGURATION" != "Release" ]; then
    error "配置必须是 Debug 或 Release。"
fi

ensure_xcode_command_line_tools
if [ "${KBE_SKIP_SYSTEM_DEPS:-0}" != "1" ]; then
    if ! command -v brew >/dev/null 2>&1; then
        error "未安装 Homebrew，请先按 https://brew.sh/ 的说明安装后重试。"
    fi
    install_system_dependencies
else
    echo "[INFO] 已跳过 Homebrew 依赖安装，将仅执行依赖验证。"
fi

configure_homebrew_environment
verify_system_dependencies
ensure_vcpkg

CMAKE_COMMAND="$(command -v cmake)"
cmake_meets_minimum_version "$CMAKE_COMMAND" || error "CMake 版本必须不低于 3.25。"

cd "$SOURCE_DIR"
"$CMAKE_COMMAND" --fresh --preset macos-ninja
"$CMAKE_COMMAND" --build "$SOURCE_DIR/out/build/macos-ninja" --config "$CONFIGURATION" \
    --target kbe_servers kbe_tests --parallel

echo "[SUCCESS] Task completed."
echo "[OUTPUT] CMake build directory: kbe/src/out/build/macos-ninja"
echo "[OUTPUT] Runtime directory: kbe/bin/server"
if [ -z "${CI:-}" ] && [ -t 0 ] && [ -t 1 ]; then
    printf "Press Enter to exit..."
    IFS= read -r kbe_unused_input || true
fi

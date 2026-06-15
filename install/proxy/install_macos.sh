#!/bin/sh
set -e


# =========================================
# Build type
# =========================================
KBE_CONFIG=${1:-Release}
echo "[INFO] Using build type: $KBE_CONFIG"

# =========================================
# Basic tools and dependencies (macOS)
# =========================================
if ! command -v git >/dev/null 2>&1; then
    echo "[ERROR] Git is not installed."
    echo "[INFO] Please install Git first, then rerun this script."
    echo "[INFO] Official site: https://git-scm.com/download/mac"
    echo "[INFO] Tutorial: https://docs.github.com/en/get-started/git-basics/set-up-git"
    exit 1
fi

if ! command -v brew >/dev/null 2>&1; then
    echo "[ERROR] Homebrew is not installed."
    echo "[INFO] Please install Homebrew first, then rerun this script."
    echo "[INFO] Official site: https://brew.sh/"
    echo "[INFO] Install guide: https://docs.brew.sh/Installation"
    exit 1
fi

if ! command -v xcode-select >/dev/null 2>&1; then
    echo "[ERROR] xcode-select is not available."
    echo "[INFO] Please install Xcode Command Line Tools first, then rerun this script."
    echo "[INFO] Run: xcode-select --install"
    echo "[INFO] Apple docs: https://developer.apple.com/download/all/"
    exit 1
fi

if ! xcode-select -p >/dev/null 2>&1; then
    echo "[ERROR] Xcode Command Line Tools are not installed."
    echo "[INFO] Please install them first, then rerun this script."
    echo "[INFO] Run: xcode-select --install"
    echo "[INFO] Apple docs: https://developer.apple.com/download/all/"
    exit 1
fi

install_brew_dep() {
    PKG="$1"
    if brew list --formula "$PKG" >/dev/null 2>&1; then
        echo "[INFO] $PKG is already installed"
        return 0
    fi

    echo "[INFO] Installing $PKG via Homebrew..."
    brew install "$PKG"
}

install_brew_dep cmake
install_brew_dep ninja
install_brew_dep autoconf
install_brew_dep automake
install_brew_dep libtool
install_brew_dep pkg-config
install_brew_dep libtirpc
install_brew_dep zstd

BREW_PREFIX="$(brew --prefix)"
ZSTD_PREFIX="$(brew --prefix zstd)"

export LDFLAGS="-L$ZSTD_PREFIX/lib ${LDFLAGS:-}"
export CPPFLAGS="-I$ZSTD_PREFIX/include ${CPPFLAGS:-}"

check_required_tool() {
    TOOL_NAME="$1"
    if ! command -v "$TOOL_NAME" >/dev/null 2>&1; then
        echo "[ERROR] $TOOL_NAME was not found in PATH after Homebrew installation."
        echo "[INFO] Please ensure Homebrew bin directory is in PATH, then rerun."
        echo "[INFO] Example for Apple Silicon: export PATH=\"/opt/homebrew/bin:$PATH\""
        exit 1
    fi
}

check_required_tool cmake
check_required_tool ninja
check_required_tool pkg-config

# =========================================
# GitCode 可访问性检查
# =========================================
echo "[检测] 尝试访问代理仓库..."
if ! command -v git >/dev/null 2>&1; then
    echo "[WARN] 未安装 git，稍后会自动安装"
fi

if ! git ls-remote https://gitcode.com/KBEngineLab/kbe-vcpkg-proxy.git >/dev/null 2>&1; then
    echo "[ERROR] 无法访问 代理仓库（GitCode），请确保网络可用"
    exit 1
fi
echo "[成功] 代理仓库（GitCode）可访问"


# =========================================
# vcpkg 安装
# =========================================
VCPKG_DIR="$HOME/kbe-vcpkg-gitcode"
echo "[INFO] 检查vcpkg目录..."

if [ ! -d "$VCPKG_DIR" ] || [ ! -f "$VCPKG_DIR/bootstrap-vcpkg.sh" ]; then
    echo "[INFO] 克隆 vcpkg"
    git clone https://gitcode.com/KBEngineLab/kbe-vcpkg-proxy.git "$VCPKG_DIR"
else
    echo "[INFO] vcpkg 已存在: $VCPKG_DIR"
fi


git -C "$VCPKG_DIR" reset --hard HEAD
git -C "$VCPKG_DIR" pull


# =========================================
# downloads目录处理
# =========================================
DOWNLOADS_PATH="$VCPKG_DIR/downloads"
echo "[INFO] 检查downloads目录..."

if [ ! -d "$DOWNLOADS_PATH" ]; then
    echo "[INFO] Downloads目录不存在，克隆仓库..."
    git clone https://gitcode.com/KBEngineLab/kbe-vcpkg-proxy-macos-download.git "$DOWNLOADS_PATH"
else
    echo "[INFO] Downloads目录已存在，检查.git目录..."
    if [ ! -d "$DOWNLOADS_PATH/.git" ]; then
        echo "[INFO] .git目录不存在，删除并重新克隆..."
        rm -rf "$DOWNLOADS_PATH"
        git clone https://gitcode.com/KBEngineLab/kbe-vcpkg-proxy-macos-download.git "$DOWNLOADS_PATH"
    else
        echo "[INFO] 更新downloads仓库..."
        cd "$DOWNLOADS_PATH"
        git pull
        cd -
    fi
fi

# =========================================
# 运行bootstrap脚本
# =========================================
echo "[INFO] 运行bootstrap-vcpkg.sh..."
OLDPWD=$(pwd)
cd "$VCPKG_DIR"
./bootstrap-vcpkg.sh
cd "$OLDPWD"

# =========================================
# 构建 KBEngine-Nex
# =========================================
echo "[INFO] 进入 ../../kbe/src/"
cd "$(dirname "$0")/../../kbe/src/"

echo "[INFO] 配置 CMake"

cmake -G Ninja -B build -S . \
    -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_DIR/scripts/buildsystems/vcpkg.cmake" \
    -DKBE_CONFIG="$KBE_CONFIG"

echo "[INFO] 设置并行编译线程数"
export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)

echo "[INFO] 开始编译 KBEngine-Nex"
cmake --build build -j"$(nproc)"

echo "[INFO] 安装完成"

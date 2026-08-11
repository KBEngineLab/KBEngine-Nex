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

run_as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}

detect_package_manager() {
    for kbe_package_manager_candidate in apt-get dnf yum zypper pacman apk; do
        if command -v "$kbe_package_manager_candidate" >/dev/null 2>&1; then
            PACKAGE_MANAGER="$kbe_package_manager_candidate"
            return
        fi
    done

    error "不支持当前 Linux 包管理器，请手动安装构建依赖后设置 KBE_SKIP_SYSTEM_DEPS=1。"
}

update_package_index() {
    if [ "${PACKAGE_INDEX_UPDATED:-0}" -eq 1 ]; then
        return
    fi

    if [ "${KBE_SKIP_PACKAGE_UPDATE:-0}" = "1" ]; then
        echo "[INFO] 已跳过系统软件包索引更新。"
        PACKAGE_INDEX_UPDATED=1
        return
    fi

    echo "[INFO] 更新系统软件包索引: $PACKAGE_MANAGER"
    case "$PACKAGE_MANAGER" in
        apt-get) run_as_root apt-get update ;;
        dnf) run_as_root dnf -y makecache ;;
        yum) run_as_root yum -y makecache ;;
        zypper) run_as_root zypper --non-interactive refresh ;;
        # Arch 不允许只刷新数据库而不升级系统，否则可能产生部分升级环境。
        # Arch forbids partial upgrades, so refresh and upgrade are performed together.
        pacman) run_as_root pacman -Syu --noconfirm ;;
        apk) run_as_root apk update ;;
    esac

    PACKAGE_INDEX_UPDATED=1
}

package_is_installed() {
    kbe_package_to_check="$1"
    case "$PACKAGE_MANAGER" in
        apt-get) dpkg -s "$kbe_package_to_check" >/dev/null 2>&1 ;;
        dnf|yum|zypper) rpm -q "$kbe_package_to_check" >/dev/null 2>&1 ;;
        pacman) pacman -Qi "$kbe_package_to_check" >/dev/null 2>&1 ;;
        apk) apk info -e "$kbe_package_to_check" >/dev/null 2>&1 ;;
    esac
}

install_package_candidate() {
    kbe_candidate_to_install="$1"
    case "$PACKAGE_MANAGER" in
        apt-get) run_as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "$kbe_candidate_to_install" ;;
        dnf) run_as_root dnf install -y "$kbe_candidate_to_install" ;;
        yum) run_as_root yum install -y "$kbe_candidate_to_install" ;;
        zypper) run_as_root zypper --non-interactive install --no-recommends "$kbe_candidate_to_install" ;;
        pacman) run_as_root pacman -S --needed --noconfirm "$kbe_candidate_to_install" ;;
        apk) run_as_root apk add --no-cache "$kbe_candidate_to_install" ;;
    esac
}

install_dependency() {
    kbe_dependency_name="$1"
    kbe_dependency_requirement="$2"
    shift 2

    # 候选包顺序沿用 Nex：先检查是否已有任一实现，再逐个尝试发行版包名。
    # Candidate ordering follows Nex: accept any installed provider, then try distro package names in order.
    for kbe_dependency_candidate in "$@"; do
        if package_is_installed "$kbe_dependency_candidate"; then
            echo "[INFO] $kbe_dependency_name 已安装: $kbe_dependency_candidate"
            return
        fi
    done

    update_package_index
    for kbe_dependency_candidate in "$@"; do
        echo "[INFO] 尝试安装 $kbe_dependency_name: $kbe_dependency_candidate"
        if install_package_candidate "$kbe_dependency_candidate" >/dev/null 2>&1; then
            echo "[INFO] $kbe_dependency_name 安装成功: $kbe_dependency_candidate"
            return
        fi
    done

    if [ "$kbe_dependency_requirement" = "optional" ]; then
        echo "[WARN] 无法安装可选组件 $kbe_dependency_name，候选包: $*" >&2
        return
    fi

    error "无法安装 $kbe_dependency_name，候选包: $*"
}

is_rhel_family() {
    [ -f /etc/os-release ] && grep -qiE 'centos|rhel|fedora|rocky|almalinux' /etc/os-release
}

install_system_dependencies() {
    detect_package_manager
    PACKAGE_INDEX_UPDATED=0

    if [ "$(id -u)" -ne 0 ]; then
        require_command sudo
    fi

    echo "[INFO] 检查 KBEngine 和 vcpkg 所需的宿主工具。"
    install_dependency "CA certificates" required ca-certificates
    install_dependency "Git" required git
    install_dependency "GCC" required gcc
    install_dependency "G++" required g++ gcc-c++
    install_dependency "Make" required make
    install_dependency "Ninja" required ninja-build ninja
    install_dependency "Autoconf" required autoconf
    install_dependency "Autoconf Archive" required autoconf-archive
    install_dependency "Automake" required automake
    install_dependency "Libtool" required libtool-bin libtool
    install_dependency "CMake" required cmake
    install_dependency "pkg-config" required pkg-config pkgconf-pkg-config pkgconf
    install_dependency "curl" required curl
    install_dependency "zip" required zip
    install_dependency "unzip" required unzip
    install_dependency "tar" required tar
    install_dependency "patch" required patch
    install_dependency "file" required file
    install_dependency "M4" required m4

    if ! is_rhel_family; then
        install_dependency "Build Tools" required build-essential '@development-tools' base-devel build-base
    fi

    if is_rhel_family && command -v dnf >/dev/null 2>&1; then
        install_dependency "dnf-plugins-core" optional dnf-plugins-core
        run_as_root dnf config-manager --set-enabled crb >/dev/null 2>&1 || true
    fi

    install_dependency "Linux kernel headers" required linux-libc-dev kernel-headers linux-headers linux-api-headers kernel-devel
    install_dependency "TIRPC" required libtirpc-dev libtirpc-devel libtirpc
    install_dependency "libffi" required libffi-dev libffi-devel libffi
    install_dependency "UUID" required uuid-dev libuuid-devel util-linux-dev util-linux
    install_dependency "BZip2" required libbz2-dev bzip2-devel libbz2-devel bzip2
    install_dependency "Zlib" required zlib1g-dev zlib-devel zlib-dev zlib
    install_dependency "CURL development files" required libcurl4-openssl-dev libcurl-devel curl-dev curl
    install_dependency "Bison" required bison
    install_dependency "Flex" required flex

    if is_rhel_family; then
        install_dependency "Perl" required perl
        install_dependency "Perl core modules" required perl-core
        install_dependency "Perl IPC::Cmd" required perl-IPC-Cmd
        install_dependency "Perl FindBin" optional perl-FindBin
    fi
}

verify_system_dependencies() {
    for kbe_required_tool in git curl zip unzip tar patch file cmake ninja pkg-config make autoconf automake libtoolize bison flex m4 perl; do
        require_command "$kbe_required_tool"
    done

    require_command cc
    require_command c++

    # sigar 在 Unix 上直接通过 pkg-config 链接系统 libtirpc，不由主 vcpkg manifest 提供。
    # sigar links the host libtirpc through pkg-config on Unix; it is not supplied by the main manifest.
    pkg-config --exists libtirpc || error "pkg-config 无法找到 libtirpc，请安装对应的开发包。"
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

select_cmake() {
    CMAKE_COMMAND="$(command -v cmake)"
    if cmake_meets_minimum_version "$CMAKE_COMMAND"; then
        return
    fi

    # 较旧发行版的软件源可能只有 CMake 3.25 以下版本，使用 vcpkg 的受管工具兜底。
    # Older distributions may package CMake below 3.25; use vcpkg's managed tool as a fallback.
    echo "[WARN] 系统 CMake 版本低于 3.25，正在获取 vcpkg 管理的 CMake。"
    if ! kbe_vcpkg_fetch_output="$("$VCPKG_ROOT/vcpkg" fetch cmake --x-stderr-status)"; then
        error "vcpkg 获取 CMake 失败，请检查网络或 KBE_VCPKG_REPOSITORY 配置。"
    fi
    kbe_fetched_cmake="$(printf '%s\n' "$kbe_vcpkg_fetch_output" | tail -n 1)"
    [ -x "$kbe_fetched_cmake" ] || error "无法获取满足版本要求的 CMake。"
    cmake_meets_minimum_version "$kbe_fetched_cmake" || error "vcpkg 提供的 CMake 版本低于 3.25。"
    CMAKE_COMMAND="$kbe_fetched_cmake"
}

if [ "$(uname -s)" != "Linux" ]; then
    error "该脚本仅支持 Linux，请在 macOS 上运行 install_macos.sh。"
fi

if [ "$CONFIGURATION" != "Debug" ] && [ "$CONFIGURATION" != "Release" ]; then
    error "配置必须是 Debug 或 Release。"
fi

if [ "${KBE_SKIP_SYSTEM_DEPS:-0}" != "1" ]; then
    install_system_dependencies
else
    echo "[INFO] 已跳过系统依赖安装，将仅执行依赖验证。"
fi

verify_system_dependencies
ensure_vcpkg
select_cmake

cd "$SOURCE_DIR"
"$CMAKE_COMMAND" --fresh --preset linux-ninja
"$CMAKE_COMMAND" --build "$SOURCE_DIR/out/build/linux-ninja" --config "$CONFIGURATION" \
    --target kbe_runtime kbe_servers kbe_tests --parallel

echo "[SUCCESS] Task completed."
echo "[OUTPUT] CMake build directory: kbe/src/out/build/linux-ninja"
echo "[OUTPUT] Runtime directory: kbe/bin/server"
if [ -z "${CI:-}" ] && [ -t 0 ] && [ -t 1 ]; then
    printf "Press Enter to exit..."
    IFS= read -r kbe_unused_input || true
fi

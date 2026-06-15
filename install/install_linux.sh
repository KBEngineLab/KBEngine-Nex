#!/bin/sh
set -e


# =========================================
# Detect distribution and select package manager
# =========================================
UPDATED=0

if command -v apt-get >/dev/null 2>&1; then
    INSTALL_CMD="sudo apt-get install -y"
    PKG_CHECK="dpkg -s"
    PKG_UPDATE="sudo apt-get update -y"
elif command -v dnf >/dev/null 2>&1; then
    INSTALL_CMD="sudo dnf install -y"
    PKG_CHECK="rpm -q"
    PKG_UPDATE="sudo dnf makecache -y"
elif command -v yum >/dev/null 2>&1; then
    INSTALL_CMD="sudo yum install -y"
    PKG_CHECK="rpm -q"
    PKG_UPDATE="sudo yum makecache -y"
elif command -v zypper >/dev/null 2>&1; then
    INSTALL_CMD="sudo zypper install -y"
    PKG_CHECK="rpm -q"
    PKG_UPDATE="sudo zypper refresh"
elif command -v pacman >/dev/null 2>&1; then
    INSTALL_CMD="sudo pacman -S --noconfirm --needed"
    PKG_CHECK="pacman -Qi"
    PKG_UPDATE="sudo pacman -Sy"
elif command -v apk >/dev/null 2>&1; then
    INSTALL_CMD="sudo apk add --no-cache"
    PKG_CHECK="apk info"
    PKG_UPDATE=""
else
    echo "[ERROR] Unknown package manager, please install dependencies manually"
    exit 1
fi

update_pkg_index() {
    if [ "$UPDATED" -eq 0 ]; then
        if [ -n "$PKG_UPDATE" ]; then
            echo "[INFO] Updating package index..."
            $PKG_UPDATE || true
        fi
        UPDATED=1
    fi
}


update_pkg_index



install_dep() {
    NAME="$1"
    IGNORE_FAIL="$2"
    shift 2
    for PKG in "$@"; do
        # Check if installed
        if $PKG_CHECK "$PKG" >/dev/null 2>&1; then
            echo "[INFO] $NAME is already installed ($PKG)"
            return 0
        fi
    done
    # Try to install candidate packages
    for PKG in "$@"; do
        echo "[INFO] Trying to install $NAME ($PKG)"
        if $INSTALL_CMD "$PKG" >/dev/null 2>&1; then
            echo "[INFO] Installed successfully: $PKG"
            return 0
        fi
    done
    echo "[WARN] Failed to install $NAME, please install manually (candidates: $*)"
    if [ "$IGNORE_FAIL" = true ]; then
        return 0
    else
        return 1
    fi
}



# Check if system is CentOS / RHEL / Fedora
is_centos() {
    [ -f /etc/os-release ] && grep -qiE 'centos|rhel|fedora' /etc/os-release
}

# =========================================
# Build type
# =========================================
KBE_CONFIG=${1:-Release}
echo "[INFO] Using build type: $KBE_CONFIG"

# =========================================
# Basic tools
# =========================================
install_dep "Git" false git
install_dep "GCC" false gcc
install_dep "G++" false g++ gcc-c++
install_dep "Make" false make
install_dep "Ninja" false ninja-build ninja
install_dep "Autoconf" false autoconf
install_dep "autoconf-archive" false autoconf-archive
install_dep "automake" false automake
install_dep "Libtool" false libtool-bin libtool
install_dep "CMake" false cmake
install_dep "pkg-config" false pkg-config pkgconf-pkg-config pkgconf

install_dep "curl" false curl
install_dep "zip" false zip
install_dep "unzip" false unzip
install_dep "tar" false tar

# Install Build Tools only on non-CentOS systems
if ! is_centos; then
    install_dep "Build Tools"  false build-essential "@development-tools" base-devel
else
    echo "[INFO] Skipping 'Build Tools' installation on CentOS/RHEL/Fedora"
fi



# =========================================
# Extra dependencies
# =========================================

if is_centos; then
    # Install dnf-plugins-core
    install_dep "dnf-plugins-core" true dnf-plugins-core 
    sudo dnf config-manager --set-enabled crb  || true
else
    echo "[INFO] Non-CentOS/RHEL/Fedora system, skipping dnf-plugins-core installation"
fi


install_dep "TIRPC"  false libtirpc-dev libtirpc-devel
install_dep "libffi"  false libffi-dev libffi-devel libffi
install_dep "UUID"  false uuid-dev libuuid-devel util-linux-dev
install_dep "BZip2"  false libbz2-dev bzip2-devel libbz2-devel bzip2
install_dep "Zlib"  false zlib1g-dev zlib-devel zlib-dev
install_dep "CURL Dev"  false libcurl4-openssl-dev libcurl-devel curl-dev
install_dep "Bison"  false bison
install_dep "Flex"   false flex

if is_centos; then
    # Install Perl
    install_dep "perl" false perl
    install_dep "perl-core" false perl-core
    install_dep "perl-IPC-Cmd" false perl-IPC-Cmd
    install_dep "perl-FindBin" true perl-FindBin
else
    echo "[INFO] Non-CentOS/RHEL/Fedora system, skipping Perl installation"
fi

# =========================================
# Check GitHub accessibility
# =========================================
echo "[CHECK] Trying to access GitHub repository..."
if ! command -v git >/dev/null 2>&1; then
    echo "[WARN] Git is not installed, will install later"
fi

if ! git ls-remote https://github.com/microsoft/vcpkg.git >/dev/null 2>&1; then
    echo "[ERROR] Cannot access GitHub repository, please check network or proxy"
    echo "[INFO] You can also use the domestic Gitee mirror, try running install/gitee/install_linux.sh."
    exit 1
fi
echo "[SUCCESS] GitHub repository is accessible"



# =========================================
# Install vcpkg
# =========================================
VCPKG_DIR="$HOME/kbe-vcpkg"
if [ ! -d "$VCPKG_DIR" ] || [ ! -f "$VCPKG_DIR/bootstrap-vcpkg.sh" ]; then
    echo "[INFO] Cloning vcpkg"
    git clone https://github.com/microsoft/vcpkg.git "$VCPKG_DIR"
else
    echo "[INFO] vcpkg already exists: $VCPKG_DIR"
fi


git -C "$VCPKG_DIR" reset --hard HEAD
git -C "$VCPKG_DIR" pull


OLDPWD=$(pwd)
cd "$VCPKG_DIR"
./bootstrap-vcpkg.sh
cd "$OLDPWD"

# =========================================
# Build KBEngine-Nex
# =========================================
echo "[INFO] Entering ../kbe/src/"
cd "$(dirname "$0")/../kbe/src/"

echo "[INFO] Configuring CMake"
cmake -G Ninja -B build -S . \
    -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_DIR/scripts/buildsystems/vcpkg.cmake" \
    -DKBE_CONFIG="$KBE_CONFIG"

export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)

echo "[INFO] Building KBEngine-Nex"
cmake --build build -j"$(nproc)"

echo "[INFO] Installation complete"

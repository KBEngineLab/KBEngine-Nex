#!/bin/bash
set -e

echo "============================================"
echo "  KBEngine Vcpkg Uninstall (Linux)"
echo "============================================"
echo ""

VCPKG1="$HOME/.cache/vcpkg"
VCPKG2="$HOME/kbe-vcpkg-gitcode"
VCPKG3="$HOME/kbe-vcpkg-gitee"
VCPKG4="$HOME/kbe-vcpkg"

echo "The following directories will be removed:"
echo "  $VCPKG1"
echo "  $VCPKG2"
echo "  $VCPKG3"
echo "  $VCPKG4"
echo ""

read -p "Continue? [Y/N]: " confirm
if [[ ! "$confirm" =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi

echo ""

for dir in "$VCPKG1" "$VCPKG2" "$VCPKG3" "$VCPKG4"; do
    if [ -d "$dir" ]; then
        echo "Removing $dir ..."
        rm -rf "$dir"
        if [ -d "$dir" ]; then
            echo "  [FAILED] $dir could not be removed."
        else
            echo "  [OK] $dir removed."
        fi
    else
        echo "  [SKIP] $dir does not exist."
    fi
done

echo ""
echo "Uninstall completed."

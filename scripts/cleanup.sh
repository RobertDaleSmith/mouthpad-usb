#!/bin/bash

# Cleanup script for BLE-USB Bridge repository
# This script removes unnecessary files and directories to make the repo much smaller

set -e

echo "🧹 Cleaning up BLE-USB Bridge repository..."
echo "This will remove build artifacts and dependencies that can be regenerated."
echo ""

# Function to safely remove directory if it exists
remove_dir() {
    if [ -d "$1" ]; then
        echo "🗑️  Removing $1"
        rm -rf "$1"
    else
        echo "ℹ️  $1 not found (already removed or doesn't exist)"
    fi
}

# Function to safely remove file if it exists
remove_file() {
    if [ -f "$1" ]; then
        echo "🗑️  Removing $1"
        rm -f "$1"
    else
        echo "ℹ️  $1 not found (already removed or doesn't exist)"
    fi
}

echo "📦 Removing build artifacts and dependencies..."

# Remove build directories
remove_dir "build"
remove_dir ".west"

# Remove Zephyr and Nordic SDK directories (these are managed by west)
remove_dir "zephyr"
remove_dir "nrf"
remove_dir "nrfxlib"
remove_dir "modules"
remove_dir "bootloader"
remove_dir "tools"
remove_dir "test"

# Remove IDE and editor files
remove_dir ".vscode"
remove_file ".DS_Store"

# Remove backup files
remove_file "prj.conf.backup"

echo ""
echo "✅ Cleanup complete!"
echo ""
echo "📋 What was removed:"
echo "   • build/ - Build artifacts (regenerated on build)"
echo "   • .west/ - West workspace (regenerated on west init)"
echo "   • zephyr/ - Zephyr RTOS (managed by west)"
echo "   • nrf/ - Nordic Connect SDK (managed by west)"
echo "   • nrfxlib/ - Nordic libraries (managed by west)"
echo "   • modules/ - Zephyr modules (managed by west)"
echo "   • bootloader/ - MCUboot (managed by west)"
echo "   • tools/ - Build tools (managed by west)"
echo "   • test/ - Test files (not needed for main project)"
echo "   • .vscode/ - VS Code settings"
echo "   • .DS_Store - macOS system file"
echo "   • prj.conf.backup - Backup configuration file"
echo ""
echo "📋 What was kept:"
echo "   • src/ - Your source code"
echo "   • boards/ - Custom board definitions"
echo "   • sdk_config/ - SDK configuration"
echo "   • *.sh - Custom build and flash scripts"
echo "   • Makefile - Build system"
echo "   • prj.conf - Project configuration"
echo "   • CMakeLists.txt - CMake configuration"
echo "   • west.yml - West manifest"
echo "   • README.md - Documentation"
echo ""
echo "🔄 To restore dependencies, run:"
echo "   west init -l ."
echo "   west update"
echo ""
echo "🔨 To build the project:"
echo "   make build"
echo ""
echo "💾 Repository size should be much smaller now!" 
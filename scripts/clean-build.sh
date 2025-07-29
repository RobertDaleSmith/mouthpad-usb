#!/bin/bash

# Clean build script for MouthPad USB
# This script uses local west but runs in a completely clean environment

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if west is available
check_west() {
    if ! command -v west &> /dev/null; then
        print_error "west is not available. Please install the Nordic Connect SDK first."
        exit 1
    fi
    print_success "west is available: $(which west)"
}

# Build in clean environment
build_clean() {
    local board=${1:-xiao_ble}
    print_status "Building for board: $board in clean environment"
    
    # Create a clean environment by unsetting problematic variables
    export ZEPHYR_SDK_INSTALL_DIR=${ZEPHYR_SDK_INSTALL_DIR:-~/zephyr-sdk-0.16.4}
    export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
    
    # Clean environment variables that might cause issues
    unset PYTHONPATH
    unset CMAKE_PREFIX_PATH
    unset TERM_PROGRAM
    unset VSCODE_IPC_HOOK_CLI
    
    # Set a minimal PATH with only essential directories
    export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
    
    # Add west to PATH if it's not in the minimal PATH
    if ! command -v west &> /dev/null; then
        export PATH="$PATH:/opt/miniconda3/bin"
    fi
    
    print_status "Using PATH: $PATH"
    print_status "Using ZEPHYR_SDK_INSTALL_DIR: $ZEPHYR_SDK_INSTALL_DIR"
    
    # Run the build in a clean subshell
    (
        # Initialize workspace if needed
        if [ ! -d ".west" ]; then
            print_status "Initializing workspace..."
            west init -m https://github.com/nrfconnect/sdk-nrf.git --mr main
            west update
        fi
        
        # Build the project
        print_status "Building firmware..."
        west build -b $board app --pristine=always
        
        print_success "Build completed!"
        
        # Check for artifacts
        if [ -f "build/zephyr/app.hex" ]; then
            print_success "✓ app.hex created"
            ls -lh build/zephyr/app.hex
        fi
        
        if [ -f "build/zephyr/app.uf2" ]; then
            print_success "✓ app.uf2 created"
            ls -lh build/zephyr/app.uf2
        fi
        
        if [ -f "build/zephyr/app.bin" ]; then
            print_success "✓ app.bin created"
            ls -lh build/zephyr/app.bin
        fi
    )
}

# Main function
main() {
    local board=${1:-xiao_ble}
    
    print_status "Starting MouthPad USB clean build"
    print_status "Target board: $board"
    
    # Check prerequisites
    check_west
    
    # Build
    build_clean "$board"
    
    print_success "Build completed successfully!"
    print_status "Artifacts are available in the build/ directory"
}

# Show help
show_help() {
    echo "Usage: $0 [BOARD]"
    echo ""
    echo "Builds the MouthPad USB firmware using local west in a clean environment"
    echo ""
    echo "Arguments:"
    echo "  BOARD    Target board (default: xiao_ble)"
    echo ""
    echo "Examples:"
    echo "  $0                    # Build for xiao_ble"
    echo "  $0 nrf52840dongle    # Build for nrf52840dongle"
    echo ""
    echo "Available boards:"
    echo "  xiao_ble"
    echo "  nrf52840dongle"
    echo "  nrf52840dk_nrf52840"
}

# Parse command line arguments
case "${1:-}" in
    -h|--help)
        show_help
        exit 0
        ;;
    *)
        main "$@"
        ;;
esac 
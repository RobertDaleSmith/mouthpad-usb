#!/bin/bash

# Simple Docker build script for MouthPad USB
# This script uses the local west installation but runs in a clean environment

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

# Check if west is available locally
check_west() {
    if ! command -v west &> /dev/null; then
        print_error "west is not available. Please install the Nordic Connect SDK first."
        exit 1
    fi
    print_success "west is available: $(which west)"
}

# Build using local west but in a clean environment
build_local() {
    local board=${1:-xiao_ble}
    print_status "Building for board: $board using local west"
    
    # Set up a clean environment
    export ZEPHYR_SDK_INSTALL_DIR=${ZEPHYR_SDK_INSTALL_DIR:-~/zephyr-sdk-0.16.4}
    export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
    
    # Clean environment variables that might cause issues
    unset PYTHONPATH
    unset CMAKE_PREFIX_PATH
    
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
    
    print_status "Starting MouthPad USB build (local west)"
    print_status "Target board: $board"
    
    # Check prerequisites
    check_west
    
    # Build
    build_local "$board"
    
    print_success "Build completed successfully!"
    print_status "Artifacts are available in the build/ directory"
}

# Show help
show_help() {
    echo "Usage: $0 [BOARD]"
    echo ""
    echo "Builds the MouthPad USB firmware using local west installation"
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
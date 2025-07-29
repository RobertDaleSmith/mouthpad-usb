#!/bin/bash

# Test GitHub Actions build locally
# This script runs the exact same Docker build that GitHub Actions uses

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

# Check if Docker is installed
check_docker() {
    if ! command -v docker &> /dev/null; then
        print_error "Docker is not installed. Please install Docker first."
        exit 1
    fi
    
    if ! docker info &> /dev/null; then
        print_error "Docker is not running. Please start Docker first."
        exit 1
    fi
    
    print_success "Docker is available"
}

# Run the same build as GitHub Actions
run_github_build() {
    local board=${1:-xiao_ble}
    print_status "Running GitHub Actions build locally for board: $board"
    
    # Create a temporary directory for the build
    local temp_dir=$(mktemp -d)
    print_status "Using temporary directory: $temp_dir"
    
    # Copy the current directory to the temp location
    cp -r . "$temp_dir/mouthpad_usb"
    
    # Run the exact same Docker build as GitHub Actions
    docker run --rm \
        -v "$temp_dir:/workspace" \
        -w /workspace \
        -e CMAKE_PREFIX_PATH=/opt/toolchains \
        ghcr.io/zephyrproject-rtos/ci:v0.26.6 \
        bash -c "
            mkdir -p zephyr-workspace
        " && \
    docker run --rm \
        -v "$temp_dir:/workspace" \
        -w /workspace \
        -e CMAKE_PREFIX_PATH=/opt/toolchains \
        ghcr.io/zephyrproject-rtos/ci:v0.26.6 \
        bash -c "
            echo '♻️ Initializing Zephyr Workspace...'
            cd zephyr-workspace
            rm -rf .west
            west init -l mouthpad_usb
            west update
            
            echo '🔨 Building Project...'
            ccache -z
            west build \
                --board $board \
                --pristine=always mouthpad_usb/app
            ccache -sv
            
            echo '📦 Build completed!'
            echo 'Artifacts:'
            ls -la build/zephyr/zephyr.*
            
            # Copy artifacts back to the original directory
            cp build/zephyr/zephyr.* /workspace/mouthpad_usb/build/zephyr/ 2>/dev/null || true
        "
    
    # Copy artifacts back to the current directory
    if [ -d "$temp_dir/mouthpad_usb/build/zephyr" ]; then
        mkdir -p build/zephyr
        cp "$temp_dir/mouthpad_usb/build/zephyr/"* build/zephyr/ 2>/dev/null || true
        print_success "Artifacts copied to build/zephyr/"
    fi
    
    # Clean up
    rm -rf "$temp_dir"
    
    print_success "Local GitHub Actions build completed!"
}

# Main function
main() {
    local board=${1:-xiao_ble}
    
    print_status "Starting local GitHub Actions build test"
    print_status "Target board: $board"
    
    # Check prerequisites
    check_docker
    
    # Build
    run_github_build "$board"
    
    print_success "Test completed successfully!"
    print_status "Artifacts are available in the build/zephyr/ directory"
}

# Show help
show_help() {
    echo "Usage: $0 [BOARD]"
    echo ""
    echo "Runs the exact same Docker build locally that GitHub Actions uses"
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
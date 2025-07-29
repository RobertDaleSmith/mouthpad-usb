#!/bin/bash

# Simple Docker test - uses local west but runs in Docker environment
# This tests if the Docker environment can build our project

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

# Test build in Docker environment
test_docker_build() {
    local board=${1:-xiao_ble}
    print_status "Testing build in Docker environment for board: $board"
    
    # Run the build in Docker with local west
    docker run --rm \
        -v "$(pwd):/workspace" \
        -w /workspace \
        -e CMAKE_PREFIX_PATH=/opt/toolchains \
        -e ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-0.17.2 \
        -e ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
        ghcr.io/zephyrproject-rtos/ci:v0.26.6 \
        bash -c "
            echo '🔧 Testing build in Docker environment...'
            
            # Check if west is available
            if command -v west &> /dev/null; then
                echo '✓ west is available'
            else
                echo '✗ west not found, installing...'
                pip3 install west
            fi
            
            # Initialize workspace if needed
            if [ ! -d '.west' ]; then
                echo 'Initializing workspace...'
                west init -m https://github.com/nordicsemi/sdk-nrf.git --mr main
                west update
            fi
            
            # Build the project
            echo 'Building firmware...'
            west build -b $board app --pristine=always
            
            echo 'Build completed!'
            echo 'Artifacts:'
            ls -la build/zephyr/zephyr.*
        "
    
    print_success "Docker build test completed!"
}

# Main function
main() {
    local board=${1:-xiao_ble}
    
    print_status "Starting simple Docker build test"
    print_status "Target board: $board"
    
    # Check prerequisites
    check_docker
    
    # Test build
    test_docker_build "$board"
    
    print_success "Test completed successfully!"
}

# Show help
show_help() {
    echo "Usage: $0 [BOARD]"
    echo ""
    echo "Tests building in Docker environment"
    echo ""
    echo "Arguments:"
    echo "  BOARD    Target board (default: xiao_ble)"
    echo ""
    echo "Examples:"
    echo "  $0                    # Test for xiao_ble"
    echo "  $0 nrf52840dongle    # Test for nrf52840dongle"
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
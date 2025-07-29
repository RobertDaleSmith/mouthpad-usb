#!/bin/bash

# Docker-only build script for MouthPad USB
# This script builds everything in Docker without local dependencies

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

# Build using Docker
build_with_docker() {
    local board=${1:-xiao_ble}
    print_status "Building for board: $board using Docker-only approach"
    
    # Build the Docker image
    print_status "Building Docker image..."
    docker build -t mouthpad-usb-builder .
    
    # Run the build
    print_status "Building firmware in Docker..."
    docker run --rm \
        -v "$(pwd):/workspace" \
        -w /workspace \
        mouthpad-usb-builder \
        bash -c "
            if [ ! -d '.west' ]; then
                echo 'Initializing workspace...'
                west init -m https://github.com/nrfconnect/sdk-nrf.git --mr main
                west update
            fi
            echo 'Building firmware...'
            west build -b $board app --pristine=always
            echo 'Build completed!'
            echo 'Artifacts:'
            ls -la build/app/zephyr/zephyr.*
        "
}

# Main function
main() {
    local board=${1:-xiao_ble}
    
    print_status "Starting MouthPad USB Docker-only build"
    print_status "Target board: $board"
    
    # Check prerequisites
    check_docker
    
    # Build
    build_with_docker "$board"
    
    print_success "Build completed successfully!"
    print_status "Artifacts are available in the build/app/zephyr/ directory"
}

# Show help
show_help() {
    echo "Usage: $0 [BOARD]"
    echo ""
    echo "Builds the MouthPad USB firmware using Docker-only approach"
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
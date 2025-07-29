#!/bin/bash

# Test NCS build with persistent workspace
# This uses the Zephyr CI image with a persistent zephyr_workspace

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

# Test the NCS build
test_ncs_build() {
    local board=${1:-xiao_ble}
    print_status "Testing NCS build for board: $board"
    
    # Run the build using docker-compose
    print_status "Running build with persistent workspace..."
    docker-compose run --rm mouthpad-build
    
    print_success "NCS build test completed!"
}

# Main function
main() {
    local board=${1:-xiao_ble}
    
    print_status "Starting NCS build test"
    print_status "Target board: $board"
    
    # Check prerequisites
    check_docker
    
    # Test build
    test_ncs_build "$board"
    
    print_success "Test completed successfully!"
}

# Show help
show_help() {
    echo "Usage: $0 [BOARD]"
    echo ""
    echo "Tests NCS build with persistent workspace"
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
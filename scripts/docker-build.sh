#!/bin/bash

# Docker build script for MouthPad USB
# This script provides an easy way to build the project using Docker

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
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

# Check if docker-compose is available
check_docker_compose() {
    if command -v docker-compose &> /dev/null; then
        print_success "docker-compose is available"
        return 0
    else
        print_warning "docker-compose not found, using direct Docker commands"
        return 1
    fi
}

# Build using docker-compose
build_with_compose() {
    local board=${1:-xiao_ble}
    print_status "Building for board: $board"
    
    # Create a temporary docker-compose override
    cat > docker-compose.override.yml << EOF
version: '3.8'
services:
  mouthpad-build:
    command: >
      bash -c "
        west init -m https://github.com/nrfconnect/sdk-nrf.git --mr main
        west update
        west build -b $board app --pristine=always
        echo 'Build completed successfully!'
        ls -la build/zephyr/
      "
EOF
    
    docker-compose up mouthpad-build
    rm -f docker-compose.override.yml
}

# Build using direct Docker
build_with_docker() {
    local board=${1:-xiao_ble}
    print_status "Building for board: $board using direct Docker"
    
    docker run --rm \
        -v "$(pwd):/workspace" \
        -w /workspace \
        ryansummers/nordic:latest \
        bash -c "
            west init -m https://github.com/nrfconnect/sdk-nrf.git --mr main
            west update
            west build -b $board app --pristine=always
            echo 'Build completed successfully!'
            ls -la build/zephyr/
        "
}

# Main function
main() {
    local board=${1:-xiao_ble}
    
    print_status "Starting MouthPad USB Docker build"
    print_status "Target board: $board"
    
    # Check prerequisites
    check_docker
    
    # Try docker-compose first, fall back to direct Docker
    if check_docker_compose; then
        build_with_compose "$board"
    else
        build_with_docker "$board"
    fi
    
    print_success "Build completed!"
    print_status "Artifacts are available in the build/ directory"
}

# Show help
show_help() {
    echo "Usage: $0 [BOARD]"
    echo ""
    echo "Builds the MouthPad USB firmware using Docker"
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
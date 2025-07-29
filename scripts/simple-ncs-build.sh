#!/bin/bash

# Simple NCS build - exactly as specified
# 1. Boot image, 2. Create /zephyr_workspace, 3. west init, 4. west update, 5. Mount app, 6. Build

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

# Run the build exactly as specified
run_build() {
    local board=${1:-xiao_ble}
    print_status "Running NCS build for board: $board"
    
    # Create a temporary directory for the workspace
    local temp_dir=$(mktemp -d)
    print_status "Using temporary workspace: $temp_dir"
    
    # Step 1: Boot the image and set up workspace
    print_status "Step 1: Booting image and creating workspace..."
    docker run --rm \
        -v "$temp_dir:/zephyr_workspace" \
        -w /zephyr_workspace \
        -e CMAKE_PREFIX_PATH=/opt/toolchains \
        ghcr.io/zephyrproject-rtos/ci:v0.26.6 \
        bash -c "
            echo 'Step 2: Creating /zephyr_workspace directory...'
            mkdir -p /zephyr_workspace
            cd /zephyr_workspace
            
            echo 'Step 3: Running west init...'
            west init -m https://github.com/nordicsemi/sdk-nrf.git --mr main
            
            echo 'Step 4: Running west update...'
            west update
            
            echo 'Workspace setup completed!'
        "
    
    # Step 5: Mount app folder and build
    print_status "Step 5: Mounting app folder and building..."
    docker run --rm \
        -v "$temp_dir:/zephyr_workspace" \
        -v "$(pwd)/app:/zephyr_workspace/app" \
        -w /zephyr_workspace \
        -e CMAKE_PREFIX_PATH=/opt/toolchains \
        ghcr.io/zephyrproject-rtos/ci:v0.26.6 \
        bash -c "
            echo 'Step 6: Running west build...'
            west build -b $board app --pristine=always
            
            echo 'Build completed!'
            echo 'Artifacts:'
            ls -la build/zephyr/zephyr.*
            
            # Copy artifacts back to current directory
            cp build/zephyr/zephyr.* /workspace/build/zephyr/ 2>/dev/null || true
        "
    
    # Copy artifacts back to current directory
    if [ -d "$temp_dir/build/zephyr" ]; then
        mkdir -p build/zephyr
        cp "$temp_dir/build/zephyr/"* build/zephyr/ 2>/dev/null || true
        print_success "Artifacts copied to build/zephyr/"
    fi
    
    # Clean up
    rm -rf "$temp_dir"
    
    print_success "NCS build completed!"
}

# Main function
main() {
    local board=${1:-xiao_ble}
    
    print_status "Starting simple NCS build"
    print_status "Target board: $board"
    
    # Check prerequisites
    check_docker
    
    # Run build
    run_build "$board"
    
    print_success "Build completed successfully!"
    print_status "Artifacts are available in the build/zephyr/ directory"
}

# Show help
show_help() {
    echo "Usage: $0 [BOARD]"
    echo ""
    echo "Runs NCS build exactly as specified:"
    echo "1. Boot image"
    echo "2. Create /zephyr_workspace"
    echo "3. west init"
    echo "4. west update"
    echo "5. Mount app folder"
    echo "6. Build"
    echo ""
    echo "Arguments:"
    echo "  BOARD    Target board (default: xiao_ble)"
    echo ""
    echo "Examples:"
    echo "  $0                    # Build for xiao_ble"
    echo "  $0 nrf52840dongle    # Build for nrf52840dongle"
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
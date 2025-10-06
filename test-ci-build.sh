#!/bin/bash
set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

BOARD=${1:-xiao_ble}
REPO_ROOT=$(pwd)

echo -e "${BLUE}Testing CI build for board: ${BOARD}${NC}"

# Use the same Docker image as GitHub Actions
DOCKER_IMAGE="ghcr.io/zephyrproject-rtos/ci:v0.26.6"

docker run --rm \
  -v "${REPO_ROOT}:/workspace" \
  -w /zephyr_workspace \
  -e CMAKE_PREFIX_PATH=/opt/toolchains \
  ${DOCKER_IMAGE} \
  bash -c "
    set -e
    
    echo '=== Initializing Zephyr Workspace ==='
    mkdir -p /zephyr_workspace
    cd /zephyr_workspace
    west init -m https://github.com/nrfconnect/sdk-nrf.git --mr v3.1.0
    west update --narrow -o=--depth=1
    
    echo '=== Copying App to Workspace ==='
    cp -r /workspace/ncs/app /zephyr_workspace/app
    
    # Apply overlay for dongle variants
    if [[ '${BOARD}' == 'nrf52840dongle_nordic' ]]; then
      echo '=== Applying Nordic dongle overlay ==='
      cd /zephyr_workspace/app/boards
      cp nrf52840dongle_nrf52840.nordic.overlay nrf52840dongle_nrf52840.overlay
      BOARD_TARGET='nrf52840dongle'
    elif [[ '${BOARD}' == 'nrf52840dongle_april' ]]; then
      echo '=== Applying April Brothers dongle overlay ==='
      cd /zephyr_workspace/app/boards
      cp nrf52840dongle_nrf52840.april.overlay nrf52840dongle_nrf52840.overlay
      BOARD_TARGET='nrf52840dongle'
    else
      BOARD_TARGET='${BOARD}'
    fi
    
    cd /zephyr_workspace
    
    # Disable sysbuild for dongle boards
    SYSBUILD_FLAG=''
    if [[ '${BOARD}' == *'dongle'* ]]; then
      echo '=== Building with --no-sysbuild for dongle ==='
      SYSBUILD_FLAG='--no-sysbuild'
    fi
    
    echo '=== Building Project ==='
    west build \
        --board \${BOARD_TARGET} \
        --pristine=always \${SYSBUILD_FLAG} app
    
    echo '=== Verifying Build Artifacts ==='
    [ -f build/app/zephyr/zephyr.uf2 ] && echo '✓ zephyr.uf2 found' || echo '✗ zephyr.uf2 missing'
    ls -lh build/app/zephyr/zephyr.uf2 || ls -lh build/zephyr/zephyr.uf2
    
    echo '=== Copying artifacts back ==='
    mkdir -p /workspace/build-test
    if [ -f build/app/zephyr/zephyr.uf2 ]; then
      cp build/app/zephyr/zephyr.uf2 /workspace/build-test/${BOARD}.uf2
    elif [ -f build/zephyr/zephyr.uf2 ]; then
      cp build/zephyr/zephyr.uf2 /workspace/build-test/${BOARD}.uf2
    fi
  "

echo -e "${GREEN}Build complete! Artifacts in ./build-test/${BOARD}.uf2${NC}"

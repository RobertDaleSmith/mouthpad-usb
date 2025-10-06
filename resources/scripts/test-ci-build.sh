#!/bin/bash
set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
NC='\033[0m'

REPO_ROOT=$(pwd)
BUILD_DIR="${REPO_ROOT}/builds"
TIMESTAMP=$(date +%Y%m%d_%H%M)
VOLUME_NAME="mouthpad_zephyr_workspace"

# Parse arguments
FORCE_INIT=false
if [[ "$1" == "--force-init" ]]; then
  FORCE_INIT=true
  echo -e "${YELLOW}Force re-initialization requested${NC}"
fi

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  Testing CI builds for all boards                         ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Ensure build directory exists (keeps old builds)
mkdir -p "${BUILD_DIR}"

# Use the same Docker image as GitHub Actions
DOCKER_IMAGE="ghcr.io/zephyrproject-rtos/ci:v0.26.6"

# Check if volume exists
VOLUME_EXISTS=$(docker volume ls -q -f name=${VOLUME_NAME})

if [[ -z "$VOLUME_EXISTS" ]] || [[ "$FORCE_INIT" == true ]]; then
  if [[ "$FORCE_INIT" == true ]] && [[ -n "$VOLUME_EXISTS" ]]; then
    echo -e "${YELLOW}Removing existing volume...${NC}"
    docker volume rm ${VOLUME_NAME}
  fi

  echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
  echo -e "${YELLOW}  Creating and initializing persistent workspace          ${NC}"
  echo -e "${YELLOW}  (This only happens once, ~5-10 minutes)                 ${NC}"
  echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"

  docker volume create ${VOLUME_NAME}

  # Initialize workspace
  docker run --rm \
    -v ${VOLUME_NAME}:/zephyr_workspace \
    -e CMAKE_PREFIX_PATH=/opt/toolchains \
    ${DOCKER_IMAGE} \
    bash -c "
      set -e
      cd /zephyr_workspace
      west init -m https://github.com/nrfconnect/sdk-nrf.git --mr v3.1.0
      west update --narrow -o=--depth=1
    "

  echo ""
  echo -e "${GREEN}✓ Persistent workspace created and initialized${NC}"
else
  echo -e "${GREEN}✓ Using existing persistent workspace (volume: ${VOLUME_NAME})${NC}"
  echo -e "${BLUE}  Tip: Run with --force-init to recreate the workspace${NC}"
fi

echo ""

# Build all boards using the persistent workspace
docker run --rm \
  -v "${REPO_ROOT}:/workspace" \
  -v ${VOLUME_NAME}:/zephyr_workspace \
  -w /zephyr_workspace \
  -e CMAKE_PREFIX_PATH=/opt/toolchains \
  -e TIMESTAMP="${TIMESTAMP}" \
  ${DOCKER_IMAGE} \
  bash -c "
    set -e
    cd /zephyr_workspace

    echo '=== Copying App to Workspace ==='
    rm -rf app
    cp -r /workspace/ncs/app /zephyr_workspace/app

    # Build all boards using the same workspace
    for BOARD in xiao_ble adafruit_feather_nrf52840 nrf52840dongle_nordic nrf52840dongle_april; do
      echo ''
      echo -e '\033[0;33m═══════════════════════════════════════════════════════════\033[0m'
      echo -e '\033[0;33m  Building: '\${BOARD}'\033[0m'
      echo -e '\033[0;33m═══════════════════════════════════════════════════════════\033[0m'

      cd /zephyr_workspace

      # Apply overlay for dongle variants
      if [[ \"\${BOARD}\" == 'nrf52840dongle_nordic' ]]; then
        echo '=== Applying Nordic dongle overlay ==='
        cp app/boards/nrf52840dongle_nrf52840.nordic.overlay app/boards/nrf52840dongle_nrf52840.overlay
        BOARD_TARGET='nrf52840dongle'
      elif [[ \"\${BOARD}\" == 'nrf52840dongle_april' ]]; then
        echo '=== Applying April Brothers dongle overlay ==='
        cp app/boards/nrf52840dongle_nrf52840.april.overlay app/boards/nrf52840dongle_nrf52840.overlay
        BOARD_TARGET='nrf52840dongle'
      else
        BOARD_TARGET=\"\${BOARD}\"
      fi

      echo '=== Building Project ==='
      west build \
          --board \${BOARD_TARGET} \
          --pristine=always app

      echo '=== Verifying Build Artifacts ==='
      if [ -f build/app/zephyr/zephyr.uf2 ]; then
        echo '✓ zephyr.uf2 found'
        ls -lh build/app/zephyr/zephyr.uf2
        cp build/app/zephyr/zephyr.uf2 /workspace/builds/mp_usb_\${BOARD}_\${TIMESTAMP}.uf2
      elif [ -f build/zephyr/zephyr.uf2 ]; then
        echo '✓ zephyr.uf2 found'
        ls -lh build/zephyr/zephyr.uf2
        cp build/zephyr/zephyr.uf2 /workspace/builds/mp_usb_\${BOARD}_\${TIMESTAMP}.uf2
      else
        echo '✗ zephyr.uf2 missing'
        exit 1
      fi

      echo -e '\033[0;32m✓ '\${BOARD}' build complete!\033[0m'
    done

    echo ''
    echo -e '\033[0;32m╔════════════════════════════════════════════════════════════╗\033[0m'
    echo -e '\033[0;32m║  All builds complete!                                      ║\033[0m'
    echo -e '\033[0;32m╚════════════════════════════════════════════════════════════╝\033[0m'
  "

echo ""
echo "Build artifacts:"
ls -lh "${BUILD_DIR}"/mp_usb_*.uf2
echo ""
echo -e "${BLUE}Persistent workspace volume: ${VOLUME_NAME}${NC}"
echo -e "${BLUE}To remove: docker volume rm ${VOLUME_NAME}${NC}"

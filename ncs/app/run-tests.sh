#!/bin/bash
# Helper script to run Zephyr unit tests in Docker
# Uses the same Docker image as the CI build workflow

# Must run from ncs/app directory
if [ ! -f "run-tests.sh" ]; then
  echo "Error: Must run from ncs/app directory"
  exit 1
fi

# Mount the parent (ncs) directory as the west workspace
docker run --rm -v $(pwd)/..:/workdir -w /workdir/app \
  ghcr.io/zephyrproject-rtos/ci:v0.26.6 \
  west twister "$@"

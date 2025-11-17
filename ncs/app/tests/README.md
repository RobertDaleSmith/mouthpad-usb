# MouthPad USB Firmware Unit Tests

This directory contains unit tests for the MouthPad USB firmware built on Nordic nRF Connect SDK (NCS) v3.1.0.

## Overview

Testing framework: **Ztest** (Zephyr's official testing framework)
Test runner: **Twister** (Zephyr's test automation tool)
Mocking: **CMock** (for isolating hardware dependencies)
Platform: **native_sim** (Linux-only simulation board)

## Quick Start

### On macOS (Recommended)

Use the provided helper script that uses the same Docker image as your CI workflow:

```bash
cd ncs/app

# Run all unit tests
./run-tests.sh -T tests/unit/ -v

# Run specific module tests
./run-tests.sh -T tests/unit/ble_bas/ -v

# Run with coverage
./run-tests.sh -T tests/unit/ --coverage
```

### On Linux

```bash
cd ncs/app

# Run all unit tests
west twister -T tests/unit/

# Run specific test
west twister -T tests/unit/ble_bas/ -v
```

## Test Structure

```
tests/
├── README.md                    # This file
└── unit/                        # Unit tests
    ├── ble_bas/                 # Battery Service tests
    │   ├── CMakeLists.txt       # Build configuration
    │   ├── testcase.yaml        # Twister test definition
    │   ├── prj.conf             # Zephyr Kconfig
    │   ├── README.md            # Module-specific docs
    │   └── src/
    │       └── test_ble_bas_standalone.c
    └── [future tests...]
```

## Current Test Coverage

### BLE Battery Service (ble_bas)
- ✅ Battery color calculation - Discrete mode
- ✅ Battery color calculation - Gradient mode
- ✅ Edge cases (invalid values, boundaries)
- **Status**: 36 test assertions, all passing

### Planned Tests
- [ ] usb_cdc - CRC and framing logic
- [ ] button - Input event detection
- [ ] ble_transport - Connection management (with mocks)
- [ ] ble_central - State machine (with mocks)
- [ ] ble_dis - Device info caching

## Docker Image

Tests use the **same Docker image as your CI firmware builds**:
- Image: `ghcr.io/zephyrproject-rtos/ci:v0.26.6`
- Benefits:
  - Already cached from firmware builds (faster)
  - Consistent environment (same as production)
  - No additional downloads needed

## Running Tests Locally

### Helper Script (Easiest)

The `run-tests.sh` script handles Docker for you:

```bash
# From ncs/app directory
./run-tests.sh -T tests/unit/ -v
```

### Direct Docker Command

```bash
cd ncs/app

docker run --rm -v $(pwd):/workdir -w /workdir \
  ghcr.io/zephyrproject-rtos/ci:v0.26.6 \
  west twister -T tests/unit/ -v
```

### Common Options

```bash
# Verbose output
./run-tests.sh -T tests/unit/ble_bas/ -v

# Coverage report
./run-tests.sh -T tests/unit/ --coverage

# Run tests matching tag
./run-tests.sh -T tests/unit/ --tag ble

# Parallel execution (faster)
./run-tests.sh -T tests/unit/ -j 8
```

## CI/CD Integration

Tests can be added to `.github/workflows/ci.yml`:

```yaml
test-firmware:
  runs-on: ubuntu-latest
  container:
    image: ghcr.io/zephyrproject-rtos/ci:v0.26.6

  steps:
    - uses: actions/checkout@v4

    - name: Run Unit Tests
      run: |
        cd ncs/app
        west twister -T tests/unit/ --coverage
```

## Writing New Tests

### 1. Create Test Directory

```bash
mkdir -p tests/unit/my_module/src
```

### 2. Required Files

**testcase.yaml:**
```yaml
common:
  type: unit
  tags: my_module unit
  platform_allow: native_sim

tests:
  app.unit.my_module.test_name:
    tags: feature
```

**prj.conf:**
```conf
CONFIG_ZTEST=y
```

**CMakeLists.txt:**
```cmake
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_my_module)

target_sources(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/test_my_module.c
)
```

**src/test_my_module.c:**
```c
#include <zephyr/ztest.h>
#include "my_module.h"

ZTEST_SUITE(my_module_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(my_module_tests, test_feature)
{
    int result = my_function(42);
    zassert_equal(result, 42, "Feature test failed");
}
```

### 3. Run Your Test

```bash
./run-tests.sh -T tests/unit/my_module/ -v
```

## Testing Strategies

### Pure Logic Tests (Easiest)
Test functions with no hardware dependencies:
- Battery calculations
- CRC computations
- State machine logic
- Data parsing

### Tests with Mocks (Medium)
Test modules that use Zephyr APIs:
```cmake
# In CMakeLists.txt
cmock_handle(${ZEPHYR_BASE}/include/zephyr/bluetooth/conn.h bt_conn)
```

Then use mocks in tests:
```c
#include "mock_bt_conn.h"

ZTEST(ble_tests, test_connection)
{
    __cmock_bt_conn_ref_ExpectAndReturn(conn, conn);
    // Test code...
}
```

## Troubleshooting

### "Invalid BOARD: native_sim"
- You're on macOS/Windows - use Docker (./run-tests.sh)
- native_sim only works on Linux

### Test fails after modifying source
- Update standalone test if using that approach
- Or switch to linking against actual source with mocks

### Docker image pull is slow
- Good news: you already have this image from firmware builds!
- Docker will use the cached image automatically

## Resources

- **Main Strategy Doc**: `/Users/robert/git/augmental/TESTING_STRATEGY.md`
- **Ztest Documentation**: https://docs.zephyrproject.org/latest/develop/test/ztest.html
- **NCS Testing Guide**: https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/test_and_optimize.html
- **Example Test**: `tests/unit/ble_bas/` (good starting point)

## Questions?

See the comprehensive testing strategy document for detailed information:
```bash
cat /Users/robert/git/augmental/TESTING_STRATEGY.md
```

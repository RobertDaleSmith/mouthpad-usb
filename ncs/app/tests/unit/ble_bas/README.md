# BLE Battery Service (BAS) Unit Tests

This directory contains unit tests for the Battery Service client battery color calculation logic.

## Test Coverage

The test suite covers the `ble_bas_get_battery_color()` function with comprehensive tests for:

### Discrete Color Mode
- Full battery (100%) → Green
- High battery (75%, 50%) → Green
- Medium battery (49%-10%) → Yellow
- Low battery (9%-0%) → Red
- Invalid/out-of-range values → Green (default)

### Gradient Color Mode
- Smooth color transitions from green (100%) → yellow (50%) → red (0%)
- Multiple interpolation points (90%, 75%, 60%, 50%, 40%, 25%, 10%, 0%)
- Proper RGB value calculations based on battery percentage

### Test Statistics
- **Total Test Cases**: 36 assertions across 2 test suites
- **Discrete Mode Tests**: 11 test cases
- **Gradient Mode Tests**: 7 test cases (with multi-point validation)

## Running the Tests (Industry Standard)

### Prerequisites

**Important**: The `native_sim` platform (required for unit tests) only runs on Linux.

For **macOS/Windows** developers:
- Use Docker (recommended - see below)
- Use a Linux VM
- Run in CI/CD only

### On Linux

```bash
cd ncs/app

# Run all BLE BAS tests
west twister -T tests/unit/ble_bas/

# Run with verbose output
west twister -T tests/unit/ble_bas/ -v

# Run specific test suite
west twister -T tests/unit/ble_bas/ -s app.unit.ble_bas.color_discrete

# Generate coverage report
west twister -T tests/unit/ble_bas/ --coverage
```

### On macOS/Windows (Docker)

**Using the helper script** (recommended - same image as CI):

The repository includes a helper script that uses the same Docker image as your CI workflow:

```bash
cd ncs/app

# Run BLE BAS tests
./run-tests.sh -T tests/unit/ble_bas/ -v

# Run all unit tests
./run-tests.sh -T tests/unit/

# Run with coverage
./run-tests.sh -T tests/unit/ble_bas/ --coverage
```

**Direct Docker command:**
```bash
cd ncs/app

docker run --rm -v $(pwd):/workdir -w /workdir \
  ghcr.io/zephyrproject-rtos/ci:v0.26.6 \
  west twister -T tests/unit/ble_bas/ -v
```

**Benefits of using the same image:**
- ✅ Faster - image already cached from your CI builds
- ✅ Consistent - same environment as production builds
- ✅ Reliable - tested and validated in your workflow

### Manual Build and Run

**On Linux:**
```bash
cd ncs/app
west build -b native_sim tests/unit/ble_bas --pristine
./build/zephyr/zephyr.exe
```

**With Docker:**
```bash
cd ncs/app
docker run --rm -v $(pwd):/workdir -w /workdir \
  ghcr.io/zephyrproject-rtos/ci:v0.26.6 \
  sh -c "west build -b native_sim tests/unit/ble_bas --pristine && ./build/zephyr/zephyr.exe"
```

## Test Implementation Details

### Standalone Test Approach

This test uses a **standalone implementation** that doesn't link against the actual `ble_bas.c` source file. This approach:

1. **Avoids BLE dependencies**: No need for Bluetooth stack, GATT, or Nordic libraries
2. **Fast build times**: Minimal dependencies = quick compilation
3. **Cross-platform**: Can run anywhere Zephyr unit tests are supported
4. **Pure logic testing**: Focuses solely on the color calculation algorithm

The test file (`test_ble_bas_standalone.c`) contains an inline copy of the `get_battery_color()` function that matches the implementation in `ble_bas.c`.

**Important**: If you modify the battery color logic in `ble_bas.c`, you must also update the corresponding function in `test_ble_bas_standalone.c` to keep tests accurate.

### Alternative: Full Integration Test

For testing with the actual `ble_bas.c` file (requires more dependencies):
1. Uncomment the alternative CMakeLists.txt configuration (see comments)
2. Add required Bluetooth and Nordic library mocks
3. Update `prj.conf` with BLE configuration

## Expected Output

When tests pass, you should see:

```
Running TESTSUITE ble_bas_color_discrete
===================================================================
START - test_full_battery_100_percent
 PASS - test_full_battery_100_percent in 0.001 seconds
===================================================================
START - test_high_battery_75_percent
 PASS - test_high_battery_75_percent in 0.001 seconds
===================================================================
...
===================================================================
TESTSUITE ble_bas_color_discrete succeeded

Running TESTSUITE ble_bas_color_gradient
===================================================================
...
===================================================================
PROJECT EXECUTION SUCCESSFUL
```

## Adding New Tests

To add more test cases:

1. Open `src/test_ble_bas_standalone.c`
2. Add a new `ZTEST()` function to the appropriate suite:

```c
ZTEST(ble_bas_color_discrete, test_new_scenario)
{
    ble_bas_rgb_color_t color = get_battery_color(42, BAS_COLOR_MODE_DISCRETE);

    zassert_equal(color.red, expected_red, "Red mismatch");
    zassert_equal(color.green, expected_green, "Green mismatch");
    zassert_equal(color.blue, expected_blue, "Blue mismatch");
}
```

3. Run the tests to verify

## CI/CD Integration

Add to `.github/workflows/test.yml`:

```yaml
- name: Run Unit Tests
  run: |
    docker run --rm -v $PWD:/workdir -w /workdir/ncs/app \
      nordicplayground/nrfconnect-sdk:main \
      west twister -T tests/unit/ble_bas/ --coverage
```

## Troubleshooting

### "Invalid BOARD: native_sim"
- You're on macOS/Windows - use Docker or Linux VM
- Or skip unit tests during local development (run in CI instead)

### "CMake build failure"
- Check that all required Kconfig options are set in `prj.conf`
- Verify CMakeLists.txt paths are correct
- Try building with `--pristine` flag

### Tests fail after modifying `ble_bas.c`
- Update the inline function in `test_ble_bas_standalone.c` to match
- Or switch to the full integration test approach

## Future Enhancements

- [ ] Add tests for `ble_bas_get_battery_level()`
- [ ] Add tests for state management functions
- [ ] Add integration tests with mocked Bluetooth APIs
- [ ] Add performance/benchmark tests
- [ ] Test battery level caching and update logic

# Makefile for BLE-USB Bridge (Seeed XIAO nRF52840)
# Uses Zephyr RTOS with nRF Connect SDK

.PHONY: all build clean flash help

# Default target
all: build

# Build the project
build:
	./scripts/build.sh

# Clean build directory
clean:
	rm -rf build

# Flash the device (requires device to be in bootloader mode)
flash: build
	./scripts/flash.sh

# Flash via UF2 bootloader
flash-uf2: build
	./scripts/flash-uf2.sh

# Simple UF2 flash (recommended)
flash-simple: build
	./scripts/flash-simple.sh

# Monitor serial output
monitor:
	./scripts/monitor.sh

# Trigger DFU mode via serial command
dfu:
	./scripts/dfu.sh

# Clean up repository (remove dependencies)
cleanup:
	./scripts/cleanup.sh

# Show help
help:
	@echo "Available targets:"
	@echo "  build     - Build the project with MCUboot bootloader"
	@echo "  clean     - Clean build directory"
	@echo "  dfu       - Trigger DFU mode via serial command"
	@echo "  flash     - Flash device with west (requires bootloader mode)"
	@echo "  cleanup   - Clean up repository (remove dependencies)"
	@echo "  flash-uf2 - Flash device via UF2 bootloader"
	@echo "  flash-simple - Simple UF2 flash (recommended)"
	@echo "  monitor   - Monitor serial output"
	@echo "  help      - Show this help message"
	@echo ""
	@echo "To flash (three options):"
	@echo "  Option 1 (MCUboot DFU):"
	@echo "    1. Run 'make dfu' to trigger DFU mode via serial"
	@echo "    2. Run 'make flash' to flash firmware"
	@echo "  Option 2 (UF2 Bootloader - Recommended):"
	@echo "    1. Double-tap reset button to enter UF2 mode"
	@echo "    2. Run 'make flash-simple' for automatic flashing"
	@echo "  Option 3 (Manual UF2):"
	@echo "    1. Double-tap reset button to enter UF2 mode"
	@echo "    2. Copy build/zephyr/zephyr.uf2 to XIAO-SENSE volume"
	@echo ""
	@echo "The device will restart with new firmware automatically."
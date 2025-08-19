# Makefile for mouthpad_usb project
# Provides convenient targets for building, cleaning, and workspace management

.PHONY: init build build-xiao build-feather flash monitor clean fullclean help

# Default target
all: build

# Initialize the workspace
init:
	west init -m https://github.com/nrfconnect/sdk-nrf.git --mr main
	west update

# Build the project (default to xiao_ble, can override with BOARD=)
BOARD ?= xiao_ble
build:
	west build -b $(BOARD) app --pristine=always

# Board-specific build targets
build-xiao:
	west build -b xiao_ble app --pristine=always

build-feather:
	west build -b adafruit_feather_nrf52840 app --pristine=always

# Flash the built firmware
flash:
	west flash --runner jlink --build-dir build/app

# Open serial monitor
monitor:
	west monitor

# Clean build directory only
clean:
	rm -rf build

# Full workspace cleanup - removes all generated workspace files
# This will require re-initializing the workspace with 'west init' and 'west update'
fullclean:
	rm -rf .west
	rm -rf nrf
	rm -rf zephyr
	rm -rf nrfxlib
	rm -rf modules
	rm -rf bootloader
	rm -rf test
	rm -rf tools
	rm -rf build
	@echo "Full workspace cleanup complete."
	@echo "To reinitialize workspace, run:"
	@echo "  west init -m https://github.com/nrfconnect/sdk-nrf.git --mr main"
	@echo "  west update"

# Show help
help:
	@echo "Available targets:"
	@echo "  init         - Initialize workspace (west init + west update)"
	@echo "  build        - Build the project (default: xiao_ble, override with BOARD=)"
	@echo "  build-xiao   - Build specifically for Seeed XIAO nRF52840"
	@echo "  build-feather - Build specifically for Adafruit Feather nRF52840 Express"
	@echo "  flash        - Flash the built firmware to device"
	@echo "  monitor      - Open serial monitor"
	@echo "  clean        - Remove build directory only"
	@echo "  fullclean    - Remove all workspace files (requires re-initialization)"
	@echo "  help         - Show this help message" 
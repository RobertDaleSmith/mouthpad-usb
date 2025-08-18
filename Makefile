# Makefile for mouthpad_usb project
# Provides convenient targets for building, cleaning, and workspace management

.PHONY: init build flash monitor clean fullclean help

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

# Flash the built firmware
flash:
	west flash

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
	@echo "  init       - Initialize workspace (west init + west update)"
	@echo "  build      - Build the project (west build -b xiao_ble app --pristine=always)"
	@echo "  flash      - Flash the built firmware to device"
	@echo "  monitor    - Open serial monitor"
	@echo "  clean      - Remove build directory only"
	@echo "  fullclean  - Remove all workspace files (requires re-initialization)"
	@echo "  help       - Show this help message" 
# Makefile for mouthpad_usb project
# Provides convenient targets for building, cleaning, and workspace management

.PHONY: init build build-xiao build-feather build-nordic-dongle build-april-dongle build-raytac-dongle flash monitor monitor-rtt monitor-cdc clean fullclean help

# Default target
all: build

# Initialize the workspace
init:
	west init -m https://github.com/nrfconnect/sdk-nrf.git --mr v3.1.0
	cd nrf && git checkout 6c6e5b32496e
	west update
	west config build.sysbuild true
	@echo "Workspace initialized with sysbuild enabled (NCS commit 6c6e5b32496e)"

# Build the project (default to xiao_ble, can override with BOARD=)
BOARD ?= xiao_ble
build:
	west build -b $(BOARD) app --pristine=always

# Board-specific build targets
build-xiao:
	west build -b xiao_ble app --pristine=always -- \
		-DDTC_OVERLAY_FILE="$(shell pwd)/app/boards/seeed_xiao_nrf52840.overlay" \
		-DEXTRA_CONF_FILE="$(shell pwd)/app/boards/seeed_xiao_nrf52840.conf"

build-feather:
	west build -b adafruit_feather_nrf52840 app --pristine=always

build-nordic-dongle:
	@echo "Building for Nordic PCA10059 Dongle (stock pins)..."
	west build -b nrf52840dongle app --pristine=always -- \
		-DDTC_OVERLAY_FILE="$(shell pwd)/app/boards/nordic_nrf52840dongle.overlay" \
		-DEXTRA_CONF_FILE="$(shell pwd)/app/boards/nordic_nrf52840dongle.conf"

build-april-dongle:
	@echo "Building for April Brothers Dongle (non-standard LED wiring)..."
	west build -b nrf52840dongle app --pristine=always -- \
		-DDTC_OVERLAY_FILE="$(shell pwd)/app/boards/aprbrother_nrf52840.overlay" \
		-DEXTRA_CONF_FILE="$(shell pwd)/app/boards/aprbrother_nrf52840.conf"

build-raytac-dongle:
	@echo "Building for Raytac MDBT50Q-RX Dongle..."
	west build -b nrf52840dongle app --pristine=always -- \
		-DDTC_OVERLAY_FILE="$(shell pwd)/app/boards/raytac_mdbt50q_rx.overlay" \
		-DEXTRA_CONF_FILE="$(shell pwd)/app/boards/raytac_mdbt50q_rx.conf"

build-makerdiary-dongle:
	@echo "Building for MakerDiary nRF52840 MDK USB Dongle..."
	west build -b nrf52840dongle app --pristine=always -- \
		-DDTC_OVERLAY_FILE="$(shell pwd)/app/boards/makerdiary_nrf52840mdk.overlay" \
		-DEXTRA_CONF_FILE="$(shell pwd)/app/boards/makerdiary_nrf52840mdk.conf"

# Flash the built firmware
flash:
	west flash --runner jlink

# Flash UF2 firmware to mounted bootloader drive
# Usage: make flash-uf2 (will copy to /Volumes/NRF52BOOT or similar)
flash-uf2:
	@UF2_FILE=""; \
	if [ -f "build/app/zephyr/zephyr.uf2" ]; then \
		UF2_FILE="build/app/zephyr/zephyr.uf2"; \
	elif [ -f "build/zephyr/zephyr.uf2" ]; then \
		UF2_FILE="build/zephyr/zephyr.uf2"; \
	else \
		echo "Error: No UF2 file found. Please build first."; \
		exit 1; \
	fi; \
	if [ -d "/Volumes/NRF52BOOT" ]; then \
		echo "Copying UF2 to /Volumes/NRF52BOOT..."; \
		cp "$$UF2_FILE" /Volumes/NRF52BOOT/; \
		echo "Flash complete! Device will reset automatically."; \
	elif [ -d "/Volumes/XIAO-SENSE" ]; then \
		echo "Copying UF2 to /Volumes/XIAO-SENSE..."; \
		cp "$$UF2_FILE" /Volumes/XIAO-SENSE/; \
		echo "Flash complete! Device will reset automatically."; \
	elif [ -d "/Volumes/FTHR840BOOT" ]; then \
		echo "Copying UF2 to /Volumes/FTHR840BOOT..."; \
		cp "$$UF2_FILE" /Volumes/FTHR840BOOT/; \
		echo "Flash complete! Device will reset automatically."; \
	elif [ -d "/Volumes/MDBT50QBOOT" ]; then \
		echo "Copying UF2 to /Volumes/MDBT50QBOOT..."; \
		cp "$$UF2_FILE" /Volumes/MDBT50QBOOT/; \
		echo "Flash complete! Device will reset automatically."; \
	elif [ -d "/Volumes/UF2BOOT" ]; then \
		echo "Copying UF2 to /Volumes/UF2BOOT..."; \
		cp "$$UF2_FILE" /Volumes/UF2BOOT/; \
		echo "Flash complete! Device will reset automatically."; \
	else \
		echo "Error: No UF2 bootloader drive found."; \
		echo "Please enter bootloader mode and try again."; \
		echo "Expected drives: /Volumes/NRF52BOOT, /Volumes/XIAO-SENSE, /Volumes/FTHR840BOOT, /Volumes/MDBT50QBOOT, or /Volumes/UF2BOOT"; \
		exit 1; \
	fi

# Monitor console output via USB CDC (default for UF2-based boards)
monitor:
	@$(MAKE) monitor-cdc

# Monitor via USB CDC (for XIAO, Feather, dongles with UF2 bootloader)
monitor-cdc:
	@echo "Opening serial monitor on maintenance console (CDC port 1)"
	@echo "NOTE: Use Ctrl+C to exit"
	@echo ""
	@if [ -z "$(PORT)" ]; then \
		PORT=$$(ls /dev/cu.usbmodem* 2>/dev/null | grep '3$$' | head -1); \
		if [ -n "$$PORT" ]; then \
			echo "Auto-detected port: $$PORT"; \
			echo "Connecting at 115200 baud..."; \
			python3 -m serial.tools.miniterm $$PORT 115200 --eol LF --raw; \
		else \
			echo "No port auto-detected. Specify with: make monitor-cdc PORT=/dev/cu.usbmodem..."; \
			echo "Available ports:"; \
			ls -1 /dev/cu.usbmodem* 2>/dev/null || echo "  (none found)"; \
		fi \
	else \
		echo "Connecting to $(PORT) at 115200 baud..."; \
		python3 -m serial.tools.miniterm $(PORT) 115200 --eol LF --raw; \
	fi

# Monitor via J-Link RTT (requires J-Link debugger hardware)
monitor-rtt:
	@echo "Starting J-Link RTT monitor (press Ctrl+C to exit)..."
	@echo "Note: Segger RTT Viewer app provides better experience with GUI"
	@/Applications/SEGGER/JLink_V794e/JLinkGDBServerCLExe -device nRF52840_xxAA -if SWD -speed 4000 -rtttelnetport 19021 -silent & \
	sleep 2; \
	/Applications/SEGGER/JLink_V794e/JLinkRTTClientExe

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
	@echo "  build-nordic-dongle - Build for Nordic PCA10059 nRF52840 Dongle (stock pins)"
	@echo "  build-april-dongle - Build for April Brothers nRF52840 Dongle (non-standard LED wiring)"
	@echo "  build-raytac-dongle - Build for Raytac MDBT50Q-RX Dongle"
	@echo "  build-makerdiary-dongle - Build for MakerDiary nRF52840 MDK USB Dongle"
	@echo "  flash        - Flash the built firmware to device using J-Link"
	@echo "  flash-uf2    - Flash the built firmware to device using UF2 bootloader"
	@echo "  monitor      - Monitor serial output via USB CDC (auto-detect port)"
	@echo "  monitor-cdc  - Monitor via USB CDC console (for UF2 boards)"
	@echo "  monitor-rtt  - Monitor via J-Link RTT (requires J-Link hardware)"
	@echo "  clean        - Remove build directory only"
	@echo "  fullclean    - Remove all workspace files (requires re-initialization)"
	@echo "  help         - Show this help message" 
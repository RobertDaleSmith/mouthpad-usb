# MouthPad ESP32 Build System
# Easy board selection without messy config file copying

.PHONY: build flash monitor clean xiao lilygo help

# Default board (can be overridden)
BOARD ?= XIAO

# Board configurations
COMMON_SDKCONFIG := sdkconfig.defaults

ifeq ($(BOARD),XIAO)
    BOARD_CONFIG = -D CONFIG_MOUTHPAD_BOARD_XIAO_ESP32S3=y
    SDKCONFIG_BOARD := sdkconfig.board.xiao
    BOARD_NAME = XIAO ESP32-S3
endif

ifeq ($(BOARD),LILYGO)
    BOARD_CONFIG = -D CONFIG_MOUTHPAD_BOARD_LILYGO_T_DISPLAY_S3=y
    SDKCONFIG_BOARD := sdkconfig.board.lilygo
    BOARD_NAME = LilyGo T-Display-S3
endif

SDKCONFIG_FILES := $(COMMON_SDKCONFIG)
ifdef SDKCONFIG_BOARD
    SDKCONFIG_FILES := $(COMMON_SDKCONFIG);$(SDKCONFIG_BOARD)
endif

init:
	@echo "Initializing ESP-IDF"
	@source ~/esp-idf/export.sh

# Default target
build:
	@echo "Building for $(BOARD_NAME)"
	SDKCONFIG_DEFAULTS="$(SDKCONFIG_FILES)" idf.py build $(BOARD_CONFIG)

# Board-specific targets
xiao:
	@$(MAKE) BOARD=XIAO build

lilygo:
	@$(MAKE) BOARD=LILYGO build

# Flash and monitor
flash:
	@echo "Flashing $(BOARD_NAME)"
	SDKCONFIG_DEFAULTS="$(SDKCONFIG_FILES)" idf.py flash $(BOARD_CONFIG)

monitor:
	SDKCONFIG_DEFAULTS="$(SDKCONFIG_FILES)" idf.py monitor

# Combined targets
flash-xiao:
	@$(MAKE) BOARD=XIAO flash

flash-lilygo:
	@$(MAKE) BOARD=LILYGO flash

# Clean
clean:
	SDKCONFIG_DEFAULTS="$(SDKCONFIG_FILES)" idf.py fullclean

# Help
help:
	@echo "MouthPad ESP32 Build Commands:"
	@echo ""
	@echo "  make [BOARD=XIAO|LILYGO]  - Build for specified board (default: XIAO)"
	@echo "  make xiao                 - Build for XIAO ESP32-S3"
	@echo "  make lilygo               - Build for LilyGo T-Display-S3"
	@echo ""
	@echo "  make flash [BOARD=...]    - Flash firmware"
	@echo "  make flash-xiao           - Flash XIAO ESP32-S3"
	@echo "  make flash-lilygo         - Flash LilyGo T-Display-S3"
	@echo ""
	@echo "  make monitor              - Monitor serial output"
	@echo "  make clean                - Clean build files"
	@echo ""
	@echo "Examples:"
	@echo "  make BOARD=LILYGO         # Build for T-Display-S3"
	@echo "  make xiao && make flash   # Build and flash XIAO"

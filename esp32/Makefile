# ESP32 BLE HID central prototype Makefile

IDF_PATH ?= $(HOME)/esp-idf
IDF_PY ?= idf.py
PYTHON ?= python3
ESP32_TARGET ?= esp32s3
ESP32_PORT ?=
ESP32_BAUD ?= 921600

# Prefer the ROM USB-Serial/JTAG interface (VID:PID 0x303a:0x1001) when a
# device path is not provided. When pyserial is unavailable or no matching port
# is detected we simply fall back to idf.py's own auto-detection.
ifeq ($(strip $(ESP32_PORT)),)
AUTO_SERIAL_JTAG_PORT := $(shell $(PYTHON) -c "import sys\ntry:\n    from serial.tools import list_ports\nexcept Exception:\n    sys.exit(0)\nports = list_ports.comports()\nfor p in ports:\n    if getattr(p, 'vid', None) == 0x303a and getattr(p, 'pid', None) == 0x1001:\n        sys.stdout.write(p.device)\n        break\nelse:\n    for p in ports:\n        desc = ((p.description or p.device) or '').lower()\n        if 'usb-serial/jtag' in desc or 'usb_serial_jtag' in desc:\n            sys.stdout.write(p.device)\n            break\n" 2>/dev/null)
endif

ifeq ($(strip $(ESP32_PORT)),)
ESP32_PORT := $(AUTO_SERIAL_JTAG_PORT)
ifneq ($(strip $(ESP32_PORT)),)
$(info Auto-detected ESP32 USB-Serial/JTAG port: $(ESP32_PORT))
endif
endif

IDF_PORT_ARG := $(if $(strip $(ESP32_PORT)),-p $(ESP32_PORT),)

.PHONY: init set-target build flash monitor flash-monitor clean fullclean

init:
	. $(IDF_PATH)/export.sh && $(IDF_PY) --version

set-target:
	$(IDF_PY) set-target $(ESP32_TARGET)

build: set-target
	$(IDF_PY) build

flash: build
	$(IDF_PY) $(IDF_PORT_ARG) -b $(ESP32_BAUD) flash

monitor:
	$(IDF_PY) $(IDF_PORT_ARG) monitor

flash-monitor: build
	$(IDF_PY) $(IDF_PORT_ARG) -b $(ESP32_BAUD) flash monitor

clean:
	$(IDF_PY) clean

fullclean:
	$(IDF_PY) fullclean

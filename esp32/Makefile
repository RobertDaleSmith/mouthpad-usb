# ESP32 BLE HID central prototype Makefile

IDF_PY ?= idf.py
ESP32_TARGET ?= esp32s3
ESP32_PORT ?= /dev/cu.usbmodem*
ESP32_BAUD ?= 921600

.PHONY: set-target build flash monitor flash-monitor clean fullclean

set-target:
	$(IDF_PY) set-target $(ESP32_TARGET)

build: set-target
	$(IDF_PY) build

flash: build
	$(IDF_PY) -p $(ESP32_PORT) -b $(ESP32_BAUD) flash

monitor:
	$(IDF_PY) -p $(ESP32_PORT) monitor

flash-monitor: build
	$(IDF_PY) -p $(ESP32_PORT) -b $(ESP32_BAUD) flash monitor

clean:
	$(IDF_PY) clean

fullclean:
	$(IDF_PY) fullclean

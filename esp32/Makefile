# Makefile for ESP32-S3 BLE-to-USB HID bridge
# Provides convenient targets for building, flashing, and monitoring via ESP-IDF

# Default serial port can be overridden: make flash PORT=/dev/ttyACM0
PORT ?= /dev/ttyACM0
# Default baud rate for flashing and monitoring
BAUD ?= 115200
# Target chip for ESP-IDF
IDF_TARGET ?= esp32s3

.PHONY: build flash monitor flash-monitor clean

build:
	idf.py -DIDF_TARGET=$(IDF_TARGET) build

flash:
	idf.py -p $(PORT) -b $(BAUD) flash

monitor:
	idf.py -p $(PORT) -b $(BAUD) monitor

flash-monitor: flash
	idf.py -p $(PORT) -b $(BAUD) monitor

clean:
	idf.py fullclean

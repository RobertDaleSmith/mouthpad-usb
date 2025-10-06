# Dual CDC + HID USB Sample

This sample demonstrates dual CDC ACM serial ports alongside USB HID using Zephyr's new USB device stack (`CONFIG_USB_DEVICE_STACK_NEXT`).

## Features

- **Dual CDC Serial Ports**
  - CDC0: Application data port
  - CDC1: Console and logging output
- **USB HID Mouse** with MouthPad descriptor
- **Custom Device Identity**
  - VID: 0x1915 (Augmental Tech)
  - PID: 0xEEEE
  - Product: "MouthPad^USB"

## Supported Boards

- **April Brothers nRF52840 Dongle** (`nrf52840dongle`)
  - UF2 bootloader without SoftDevice
  - Start address: 0x1000

- **Seeed XIAO nRF52840** (`xiao_ble`)
  - MCUboot bootloader
  - Start address: 0x27000

- **Adafruit Feather nRF52840** (`adafruit_feather_nrf52840`)
  - UF2 bootloader with SoftDevice
  - Start address: 0x26000

## Building

```bash
# April Brothers Dongle
west build -b nrf52840dongle samples/dual_cdc_hid --pristine=always

# XIAO nRF52840
west build -b xiao_ble samples/dual_cdc_hid --pristine=always

# Adafruit Feather nRF52840
west build -b adafruit_feather_nrf52840 samples/dual_cdc_hid --pristine=always
```

## Flashing

### April Brothers (nRF52840 Dongle)
```bash
nrfutil dfu usb-serial -pkg build/zephyr/app.uf2 -p /dev/tty.usbmodem*
```

### XIAO / Feather (UF2 Bootloader)
1. Double-tap reset button to enter bootloader mode
2. Copy `build/zephyr/zephyr.uf2` to the mounted drive:
   - XIAO: `/Volumes/XIAO-SENSE`
   - Feather: `/Volumes/FTHR840BOOT`

## Testing

After flashing, the device will enumerate as:
- 1x HID Mouse device
- 2x CDC ACM serial ports

Connect to CDC1 (second serial port) to view console logs showing device initialization.

Press the button to send mouse click reports via HID.

## Architecture Notes

This sample uses Zephyr's **new USB device stack** (`CONFIG_USB_DEVICE_STACK_NEXT`) which supports multiple CDC instances, unlike the legacy stack.

Each board has:
- Board-specific `.conf` file for partition configuration
- Board-specific `.overlay` file for hardware and partition layout
- Correct partition addresses for its bootloader type

## Key Configuration

- `CONFIG_USB_DEVICE_STACK_NEXT=y` - New USB stack
- `CONFIG_USBD_CDC_ACM_CLASS=y` - CDC ACM support
- `CONFIG_USBD_HID_SUPPORT=y` - HID device support
- `CONFIG_USE_DT_CODE_PARTITION=y` - Use device tree partitions (board-specific)
- `CONFIG_LOG_BACKEND_UART=n` - Prevent console recursion warnings

## Migration Reference

This sample serves as a reference for migrating the main MouthPad^USB application to support dual CDC ports alongside HID.

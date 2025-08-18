# Adafruit Feather nRF52840 Bootloader Compatibility

This document covers the bootloader compatibility challenges and solutions for using the **Adafruit Feather nRF52840 Express** with Zephyr/nRF Connect SDK applications, specifically for the MouthPad^USB project.

## Background

The Adafruit Feather nRF52840 Express ships with a custom UF2 bootloader that differs from the standard Nordic MCUboot bootloader used in most nRF Connect SDK applications. This creates compatibility challenges when deploying Zephyr applications to the board.

## Key Issues Identified

### 1. **Bootloader Incompatibility**
- **Stock Adafruit Bootloader**: Custom UF2 bootloader optimized for Arduino/CircuitPython
- **nRF Connect SDK Default**: MCUboot with DFU support
- **Problem**: Applications built for MCUboot won't boot with Adafruit's bootloader

### 2. **Memory Layout Conflicts**
- **Adafruit Layout**: Different partition scheme optimized for their ecosystem
- **Nordic Layout**: Standard MCUboot + application + settings partitions
- **Result**: Memory conflicts and boot failures

## Solutions Implemented

### Nordic DevZone References
Key insights from Nordic Developer Zone discussions:
- [Adafruit bootloader nRF Connect Zephyr application](https://devzone.nordicsemi.com/f/nordic-q-a/89216/adafruit-bootloader-nrf-connect-zephyr-application)
- [UF2 image and bootloader compatibility](https://devzone.nordicsemi.com/f/nordic-q-a/97314/uf2-image-and-bootloader/413630)

### Solution 1: Custom Memory Layout

Created board-specific partition configuration:

**File**: `app/pm_static_adafruit_feather_nrf52840.yml`
```yaml
# Custom partition layout for Adafruit Feather bootloader compatibility
app:
  address: 0x26000  # Start after Adafruit bootloader space
  size: 0xc6000     # Adjusted for available space
  
settings_storage:
  address: 0xec000
  size: 0x8000
  
external_flash:
  address: 0xf4000
  size: 0x32000
```

### Solution 2: Board Configuration Override

**File**: `app/boards/adafruit_feather_nrf52840.conf`
```kconfig
# Disable MCUboot (incompatible with Adafruit bootloader)
CONFIG_BOOTLOADER_MCUBOOT=n

# Use single application slot (no MCUboot DFU)
CONFIG_SINGLE_APPLICATION_SLOT=y

# UF2 support for Adafruit bootloader
CONFIG_BUILD_OUTPUT_UF2=y
```

### Solution 3: Device Tree Overlay

**File**: `app/boards/adafruit_feather_nrf52840.overlay`
```dts
// Disable features not available on Feather board
&pinctrl {
    // Custom pin configurations for Feather layout
};

// Remove OLED display (not available on Feather)
/ {
    aliases {
        /delete-property/ oled-display;
    };
};
```

## Deployment Process

### For Development
1. **Build**: `west build -b adafruit_feather_nrf52840 app`
2. **Enter Bootloader**: Double-tap reset button on Feather
3. **Flash**: Copy `zephyr.uf2` to `FEATHERBOOT` drive
4. **Verify**: Board resets and runs application

### For Production
- Use GitHub Actions CI artifacts: `mouthpad^usb_adafruit_feather_nrf52840_[commit].uf2`
- Same deployment process as development

## Key Differences from XIAO Board

| Feature | XIAO nRF52840 | Adafruit Feather nRF52840 |
|---------|---------------|---------------------------|
| **Bootloader** | MCUboot | Custom UF2 |
| **DFU Support** | Yes (MCUboot) | No |
| **Memory Layout** | Standard Nordic | Custom Adafruit |
| **Flash Method** | UF2 + DFU | UF2 only |
| **OLED Display** | Supported | Not available |
| **Pin Layout** | XIAO format | Feather format |

## Limitations

### No DFU Support
- **Impact**: Cannot use over-the-air updates
- **Workaround**: Physical access required for firmware updates
- **Alternative**: Consider MCUboot replacement (advanced)

### Single Application Slot
- **Impact**: No failsafe firmware rollback
- **Workaround**: Careful testing before deployment
- **Mitigation**: Always keep working firmware backup

### Hardware Differences
- **No OLED**: Display features disabled gracefully
- **Different Pins**: Some GPIO mappings differ from XIAO

## Troubleshooting

### Build Issues
```bash
# Clean build if switching between boards
west build -b adafruit_feather_nrf52840 app --pristine=always
```

### Boot Issues
1. **Check bootloader mode**: Double-tap reset should show `FEATHERBOOT` drive
2. **Verify UF2 file**: Ensure using Feather-specific build
3. **Memory conflicts**: Check partition manager logs

### Runtime Issues
- **I2C Problems**: Feather uses `i2c0` instead of `i2c1`
- **Display Errors**: Normal - Feather doesn't have OLED
- **Pin Mapping**: Verify GPIO assignments for Feather layout

## Future Improvements

### Potential MCUboot Migration
- Replace Adafruit bootloader with MCUboot
- Enables DFU support and dual-slot updates  
- Requires bootloader reflashing (more complex)

### Enhanced Pin Mapping
- Full GPIO compatibility layer
- Support for Feather-specific peripherals
- FeatherWing expansion compatibility

## References

1. [Nordic DevZone: Adafruit bootloader compatibility](https://devzone.nordicsemi.com/f/nordic-q-a/89216/adafruit-bootloader-nrf-connect-zephyr-application)
2. [Nordic DevZone: UF2 image and bootloader](https://devzone.nordicsemi.com/f/nordic-q-a/97314/uf2-image-and-bootloader/413630)
3. [Adafruit nRF52 Bootloader Documentation](https://github.com/adafruit/Adafruit_nRF52_Bootloader)
4. [nRF Connect SDK Partition Manager](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/scripts/partition_manager/partition_manager.html)

## Conclusion

The Adafruit Feather nRF52840 integration required significant bootloader compatibility work but now functions reliably with the stock Adafruit bootloader. While some advanced features like DFU are not available, the board provides an excellent alternative hardware platform for MouthPad^USB with its robust Feather ecosystem and form factor.
# USB HID Dual Mode Implementation Plan

## Overview

This document outlines the implementation plan for adding **dual mode USB HID support** to the MouthPad^USB firmware, enabling both **boot protocol** (legacy compatibility) and **report protocol** (modern features) support.

## Background

### Current State
- **Report Protocol Only**: Custom mouse descriptor with enhanced features
- **Modern OS Support**: Full compatibility with Windows, macOS, Linux
- **Limited Legacy Support**: No BIOS/UEFI compatibility

### Target State
- **Dual Protocol Support**: Boot protocol + Report protocol
- **Legacy Compatibility**: BIOS/UEFI system support
- **Automatic Selection**: OS chooses optimal protocol
- **Enhanced Features**: Maintain all current functionality

## Implementation Plan

### Phase 1: Core Infrastructure

#### 1.1 Add Boot Protocol Descriptor
```c
// Standard boot mouse descriptor for legacy compatibility
static const uint8_t hid_boot_mouse_desc[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x01,        // Usage (Pointer)
    0xA1, 0x00,        // Collection (Physical)
    0x05, 0x09,        // Usage Page (Button)
    0x19, 0x01,        // Usage Minimum (Button 1)
    0x29, 0x03,        // Usage Maximum (Button 3)
    0x15, 0x00,        // Logical Minimum (0)
    0x25, 0x01,        // Logical Maximum (1)
    0x95, 0x03,        // Report Count (3)
    0x75, 0x01,        // Report Size (1)
    0x81, 0x02,        // Input (Data, Variable, Absolute)
    0x95, 0x01,        // Report Count (1)
    0x75, 0x05,        // Report Size (5)
    0x81, 0x01,        // Input (Constant)
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x30,        // Usage (X)
    0x09, 0x31,        // Usage (Y)
    0x09, 0x38,        // Usage (Wheel)
    0x15, 0x81,        // Logical Minimum (-127)
    0x25, 0x7F,        // Logical Maximum (127)
    0x75, 0x08,        // Report Size (8)
    0x95, 0x03,        // Report Count (3)
    0x81, 0x06,        // Input (Data, Variable, Relative)
    0xC0,              // End Collection
    0xC0               // End Collection
};
```

#### 1.2 Add Protocol State Management
```c
// Protocol state tracking
static enum {
    USB_HID_PROTOCOL_BOOT,
    USB_HID_PROTOCOL_REPORT
} current_protocol = USB_HID_PROTOCOL_REPORT;

// Configuration option
CONFIG_USB_HID_DUAL_MODE=y
```

#### 1.3 Dynamic Descriptor Selection
```c
// Function to get appropriate descriptor based on protocol
static const uint8_t *get_hid_descriptor(void)
{
    if (current_protocol == USB_HID_PROTOCOL_BOOT) {
        return hid_boot_mouse_desc;
    } else {
        return hid_report_desc;  // Current custom descriptor
    }
}
```

### Phase 2: Protocol Switching Support

#### 2.1 USB HID Protocol Callbacks
```c
// Protocol switching callback
static int usb_hid_set_protocol_cb(const struct device *dev, uint8_t protocol)
{
    if (protocol == 0) {  // Boot protocol
        current_protocol = USB_HID_PROTOCOL_BOOT;
        LOG_INF("Switched to BOOT protocol");
    } else if (protocol == 1) {  // Report protocol
        current_protocol = USB_HID_PROTOCOL_REPORT;
        LOG_INF("Switched to REPORT protocol");
    }
    return 0;
}

// Protocol query callback
static int usb_hid_get_protocol_cb(const struct device *dev, uint8_t *protocol)
{
    *protocol = (current_protocol == USB_HID_PROTOCOL_BOOT) ? 0 : 1;
    return 0;
}
```

#### 2.2 Enhanced HID Operations Structure
```c
static const struct hid_ops ops = {
    .int_in_ready = int_in_ready_cb,
    .set_protocol = usb_hid_set_protocol_cb,
    .get_protocol = usb_hid_get_protocol_cb,
};
```

### Phase 3: Report Format Conversion

#### 3.1 Boot Protocol Report Format
```c
// Standard boot mouse report format
struct boot_mouse_report {
    uint8_t buttons;    // 3 bits for buttons, 5 bits reserved
    int8_t x;          // X movement (-127 to +127)
    int8_t y;          // Y movement (-127 to +127)
    int8_t wheel;      // Wheel movement (-127 to +127)
};
```

#### 3.2 Report Conversion Function
```c
// Convert custom reports to boot format
static void convert_to_boot_format(const uint8_t *custom_report, uint8_t *boot_report)
{
    if (current_protocol == USB_HID_PROTOCOL_BOOT) {
        // Convert Report ID 1 (buttons + wheel) to boot format
        boot_report[0] = custom_report[1] & 0x07;  // Buttons (3 bits)
        boot_report[1] = custom_report[2];          // X movement
        boot_report[2] = custom_report[3];          // Y movement  
        boot_report[3] = custom_report[1] >> 3;    // Wheel
    } else {
        // Use custom format directly
        memcpy(boot_report, custom_report, sizeof(custom_report));
    }
}
```

#### 3.3 Enhanced Report Sending
```c
// Modified report sending function
static int send_hid_report(const uint8_t *data, uint16_t len)
{
    uint8_t converted_report[4];
    
    if (current_protocol == USB_HID_PROTOCOL_BOOT) {
        convert_to_boot_format(data, converted_report);
        return hid_int_ep_write(hid_dev, converted_report, 4, NULL);
    } else {
        return hid_int_ep_write(hid_dev, data, len, NULL);
    }
}
```

### Phase 4: Configuration and Testing

#### 4.1 Configuration Options
```c
# Add to prj.conf
CONFIG_USB_HID_DUAL_MODE=y
CONFIG_USB_HID_BOOT_PROTOCOL=y
CONFIG_USB_HID_REPORT_PROTOCOL=y
CONFIG_USB_HID_AUTO_PROTOCOL_SELECTION=y
```

#### 4.2 Runtime Protocol Selection
```c
// API for runtime protocol switching
int usb_hid_set_protocol(enum usb_hid_protocol protocol);
int usb_hid_get_protocol(enum usb_hid_protocol *protocol);
```

#### 4.3 Testing Strategy
1. **Legacy System Testing**: BIOS/UEFI compatibility
2. **Modern OS Testing**: Windows, macOS, Linux
3. **Protocol Switching**: Automatic and manual switching
4. **Feature Testing**: Enhanced features in report mode

## Benefits

### Legacy Compatibility
- ✅ **BIOS/UEFI Support**: Boot protocol for legacy systems
- ✅ **Older OS Support**: Windows 98/XP compatibility
- ✅ **Embedded Systems**: Industrial/medical device compatibility

### Modern Features
- ✅ **Enhanced Functionality**: Media keys, extra buttons
- ✅ **Custom Reports**: Flexible data formats
- ✅ **Advanced Features**: High-resolution movement, pressure sensitivity

### Automatic Selection
- ✅ **OS Detection**: Automatic protocol selection
- ✅ **Fallback Support**: Boot protocol if report fails
- ✅ **Optimal Performance**: Best protocol for each system

## Implementation Timeline

### Week 1: Core Infrastructure
- [ ] Add boot protocol descriptor
- [ ] Implement protocol state management
- [ ] Add dynamic descriptor selection

### Week 2: Protocol Switching
- [ ] Implement protocol switching callbacks
- [ ] Add enhanced HID operations
- [ ] Test protocol switching

### Week 3: Report Conversion
- [ ] Implement report format conversion
- [ ] Add enhanced report sending
- [ ] Test report conversion

### Week 4: Testing and Polish
- [ ] Comprehensive testing
- [ ] Documentation updates
- [ ] Performance optimization

## Technical Considerations

### Memory Usage
- **Boot Descriptor**: ~50 bytes additional
- **Protocol State**: 1 byte additional
- **Conversion Buffer**: 4 bytes additional
- **Total Overhead**: ~55 bytes

### Performance Impact
- **Protocol Switching**: Minimal overhead
- **Report Conversion**: <1μs per report
- **Memory Usage**: Negligible increase

### Compatibility Matrix

| System Type | Boot Protocol | Report Protocol | Auto Selection |
|-------------|---------------|-----------------|----------------|
| **BIOS/UEFI** | ✅ Yes | ❌ No | ✅ Boot |
| **Windows 98/XP** | ✅ Yes | ❌ No | ✅ Boot |
| **Windows 7+** | ✅ Yes | ✅ Yes | ✅ Report |
| **macOS** | ✅ Yes | ✅ Yes | ✅ Report |
| **Linux** | ✅ Yes | ✅ Yes | ✅ Report |
| **Embedded** | ✅ Yes | ❌ No | ✅ Boot |

## Future Enhancements

### Keyboard Support
- Add boot keyboard descriptor
- Implement keyboard report conversion
- Support for legacy keyboard compatibility

### Enhanced Features
- Pressure sensitivity in report mode
- High-resolution movement
- Custom button mappings
- Macro support

### Advanced Protocol Selection
- Machine learning for protocol optimization
- User preference storage
- Performance monitoring and adaptation

## Conclusion

This dual mode implementation will provide **maximum compatibility** while maintaining **enhanced functionality**, making the MouthPad^USB device suitable for both **legacy systems** and **modern applications**.

The implementation is **modular** and **backward compatible**, ensuring that existing functionality is preserved while adding new capabilities for broader system support. 
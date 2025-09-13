# Even G1 Bitmap Transmission Protocol - Official Reference

This document describes the exact bitmap transmission protocol used by the official EvenDemoApp, based on analysis of the Flutter/Dart source code.

## Overview

The Even G1 AR glasses receive bitmap images through a specific BLE protocol that transmits complete BMP files with headers, not raw pixel data.

## Image Requirements

### Format Specification
- **File Format**: Windows Bitmap (.bmp)
- **Dimensions**: Exactly 576 × 136 pixels
- **Bit Depth**: 1-bit monochrome (black and white only)
- **Color Interpretation**: 
  - Bit 0 = WHITE pixel (foreground)
  - Bit 1 = BLACK pixel (background)
- **File Size**: Approximately 9,784 bytes (including all headers)

### BMP File Structure
The complete BMP file contains:

1. **BMP File Header** (14 bytes)
   ```
   Offset  Size  Description
   0-1     2     "BM" signature
   2-5     4     File size (little-endian)
   6-9     4     Reserved (zeros)
   10-13   4     Offset to pixel data (62 for 1-bit BMP)
   ```

2. **DIB Header** (40 bytes - BITMAPINFOHEADER)
   ```
   Offset  Size  Description
   14-17   4     Header size (40)
   18-21   4     Width (576, little-endian)
   22-25   4     Height (136, little-endian, positive = bottom-up)
   26-27   2     Planes (1)
   28-29   2     Bits per pixel (1)
   30-33   4     Compression (0 = none)
   34-37   4     Image size (can be 0 for uncompressed)
   38-41   4     X pixels per meter (0)
   42-45   4     Y pixels per meter (0)
   46-49   4     Colors used (2 for 1-bit)
   50-53   4     Important colors (0 = all)
   ```

3. **Color Palette** (8 bytes for 1-bit)
   ```
   Offset  Size  Description
   54-57   4     Color 0: RGB(255,255,255) + padding (WHITE)
   58-61   4     Color 1: RGB(0,0,0) + padding (BLACK)
   ```

4. **Pixel Data** (~9,720 bytes)
   - 1-bit packed pixels
   - Rows padded to 4-byte boundaries
   - Bottom-up row order (last row first in file)
   - Each byte represents 8 pixels (MSB = leftmost pixel)

## Transmission Protocol

### Packet Structure

**First Packet Only:**
```
[0x15, 0x00, 0x00, 0x1C, 0x00, 0x00, <194 bytes of BMP data>]
 ^      ^                ^
 |      |                └─ Storage address (0x001C0000)
 |      └─ Sequence number (always 0x00 for first packet)
 └─ Bitmap data opcode
```

**Subsequent Packets:**
```
[0x15, sequence, <194 bytes of BMP data>]
 ^      ^
 |      └─ Sequence number (0x01, 0x02, 0x03, ...)
 └─ Bitmap data opcode
```

### Transmission Sequence

1. **Data Transmission Phase**
   - Send complete BMP file in 194-byte chunks
   - First packet includes storage address `{0x00, 0x1C, 0x00, 0x00}`
   - Sequence numbers increment: 0x00, 0x01, 0x02, ...
   - Typically requires ~51 packets for full BMP file

2. **End Marker Phase**
   ```
   [0x20, 0x0D, 0x0E]
   ```
   - Opcode 0x20 = End of bitmap data
   - Fixed payload: 0x0D, 0x0E

3. **CRC Verification Phase**
   ```
   [0x16, <4 bytes CRC32>]
   ```
   - Opcode 0x16 = CRC verification
   - CRC32 is calculated on: Storage Address + Complete BMP File

### CRC Calculation Details - OFFICIAL IMPLEMENTATION

**VERIFIED FROM EvenDemoApp SOURCE CODE:**

**Input Data Composition:**
```dart
// From lib/controllers/bmp_update_manager.dart
Uint8List prependAddress(Uint8List image) {
  List<int> addressBytes = [0x00, 0x1c, 0x00, 0x00];
  Uint8List newImage = Uint8List(addressBytes.length + image.length);
  newImage.setRange(0, addressBytes.length, addressBytes);
  newImage.setRange(addressBytes.length, newImage.length, image);
  return newImage;
}
```

**CRC Algorithm - CRC-32/XZ (NOT CRC32-IEEE):**
```dart
// Exact implementation from EvenDemoApp
var crc32 = Crc32Xz().convert(result); 
var val = crc32.toBigInt().toInt();
```

**CRC-32/XZ Specification:**
- **Width:** 32 bits
- **Polynomial:** 0x04c11db7
- **Initial Value:** 0xffffffff  
- **Reflect Input:** true
- **Reflect Output:** true
- **XOR Output:** 0xffffffff
- **Check Value:** 0xcbf43926

**Transmission Format - Big-Endian:**
```dart
// Network byte order (big-endian)
var crc = Uint8List.fromList([
  val >> 8 * 3 & 0xff,  // MSB first
  val >> 8 * 2 & 0xff,
  val >> 8 & 0xff,
  val & 0xff,           // LSB last
]);
```

**Critical Differences from CRC32-IEEE:**
- Uses polynomial 0x04c11db7 (not 0xedb88320)
- Proper initialization with 0xffffffff
- Final XOR with 0xffffffff
- Reflected input/output processing

## Expected Responses

### End Command Response
Both arms respond with:
```
[0x20, 0xC9, 0x00, 0x00, ...]
```
- 0x20 = End command acknowledgment
- 0xC9 = Success status

### CRC Validation Response
Both arms respond with:
```
[0x16, <4 bytes echoed CRC>, 0xC9, 0x00, ...]
```
- 0x16 = CRC validation response
- Echoed CRC matches transmitted CRC
- 0xC9 = Success status

## Implementation Notes

### Critical Requirements
1. **Must send complete BMP file** - not raw pixel data
2. **Must include storage address** in first packet only
3. **Must use exact packet sizes** (194 bytes data per packet)
4. **Must calculate CRC on address + complete BMP file**
5. **Must wait for both arms** to acknowledge end and CRC

### Common Mistakes
❌ Sending raw pixel data instead of complete BMP file
❌ Calculating CRC on pixel data only (missing headers)
❌ Using wrong storage address or including it in every packet  
❌ Wrong CRC algorithm or byte ordering
❌ Not waiting for dual-arm acknowledgments

### Success Criteria
✅ Both arms respond with 0xC9 status to end command
✅ Both arms echo correct CRC and respond with 0xC9 status
✅ Bitmap displays on glasses after successful transmission

## Reference Implementation

The MouthPad^USB implementation correctly follows this protocol:

```c
// Create proper BMP file from pixel data
create_bmp_file_from_pixels(&bmp_data, &bmp_size, pixels, width, height);

// Send complete BMP file (not raw pixels)
even_g1_send_bmp_file(bmp_data, bmp_size);

// CRC calculation includes storage address + complete BMP
uint8_t crc_input[4 + bmp_size];
memcpy(crc_input, address, 4);
memcpy(crc_input + 4, bmp_data, bmp_size);
uint32_t crc = calculate_crc32(crc_input, 4 + bmp_size);
```

## Debugging Tips

1. **Verify BMP file structure** with hex dump of first 64 bytes
2. **Check packet count** should be ~51 for typical BMP file
3. **Verify CRC calculation** by comparing with known working implementation
4. **Monitor both arms** - both must acknowledge successfully
5. **Check timing** - allow adequate delays between phases

## Official Sample Files

The EvenDemoApp repository contains working samples:
- `assets/images/image_1.bmp` (576×136, 1-bit, ~9,784 bytes)
- `assets/images/image_2.bmp` (576×136, 1-bit, ~9,784 bytes)

These files can be used as reference for correct BMP structure and as test cases for protocol implementation.
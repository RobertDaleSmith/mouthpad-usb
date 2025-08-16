#!/usr/bin/env python3
"""
Convert PNG logo to C bitmap array for SSD1306 OLED display
"""

from PIL import Image
import sys

def png_to_c_bitmap(input_file, output_name="augmental_logo"):
    """Convert PNG to C bitmap array suitable for SSD1306"""
    
    # Open and process image
    img = Image.open(input_file)
    print(f"Original image: {img.size[0]}x{img.size[1]} pixels, mode: {img.mode}")
    
    # Convert to grayscale first, then to 1-bit (black/white)
    if img.mode == 'RGBA':
        # Handle transparency by converting to white background
        background = Image.new('RGB', img.size, (255, 255, 255))
        background.paste(img, mask=img.split()[-1])  # Use alpha channel as mask
        img = background
    
    # Convert to grayscale then to 1-bit
    img = img.convert('L')  # Grayscale
    img = img.convert('1')  # 1-bit black/white
    
    width, height = img.size
    print(f"Converted to: {width}x{height} pixels, 1-bit monochrome")
    
    # Get pixel data
    pixels = list(img.getdata())
    
    # Convert to bytes (8 pixels per byte, MSB first)
    bitmap_data = []
    for y in range(0, height, 8):  # Process 8 rows at a time
        for x in range(width):
            byte_val = 0
            for bit in range(8):
                if y + bit < height:
                    pixel_idx = (y + bit) * width + x
                    if pixel_idx < len(pixels) and pixels[pixel_idx] == 0:  # Black pixel
                        byte_val |= (1 << bit)
            bitmap_data.append(byte_val)
    
    # Generate C code
    c_code = f"""/* Augmental logo bitmap - {width}x{height} pixels */
#include <stdint.h>

#define AUGMENTAL_LOGO_WIDTH  {width}
#define AUGMENTAL_LOGO_HEIGHT {height}

static const uint8_t {output_name}_bitmap[] = {{
"""
    
    # Add bitmap data in rows of 16 bytes for readability
    for i in range(0, len(bitmap_data), 16):
        row = bitmap_data[i:i+16]
        hex_values = [f"0x{b:02X}" for b in row]
        c_code += "    " + ", ".join(hex_values)
        if i + 16 < len(bitmap_data):
            c_code += ","
        c_code += "\n"
    
    c_code += "};\n"
    
    print(f"Generated {len(bitmap_data)} bytes of bitmap data")
    return c_code

if __name__ == "__main__":
    input_file = "3dprint/augmental_logo.png"
    
    try:
        c_bitmap = png_to_c_bitmap(input_file)
        
        # Write to header file
        output_file = "app/src/augmental_logo.h"
        with open(output_file, 'w') as f:
            f.write("/*\n")
            f.write(" * Augmental Logo Bitmap\n")
            f.write(" * Generated from augmental_logo.png\n")
            f.write(" * For SSD1306 OLED Display (128x64)\n")
            f.write(" */\n\n")
            f.write("#ifndef AUGMENTAL_LOGO_H_\n")
            f.write("#define AUGMENTAL_LOGO_H_\n\n")
            f.write(c_bitmap)
            f.write("\n#endif /* AUGMENTAL_LOGO_H_ */\n")
        
        print(f"✅ C bitmap header saved to: {output_file}")
        
        # Also print preview for verification
        print("\n📄 Preview of generated C code:")
        print(c_bitmap[:300] + "..." if len(c_bitmap) > 300 else c_bitmap)
        
    except Exception as e:
        print(f"❌ Error: {e}")
        sys.exit(1)
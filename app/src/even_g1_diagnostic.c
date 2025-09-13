/* Even G1 Bitmap Diagnostic Tool
 * 
 * This tool helps compare our bitmap transmission with the official EvenDemoApp
 * by logging exact byte sequences for comparison.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "even_g1.h"

LOG_MODULE_REGISTER(even_g1_diag, LOG_LEVEL_INF);

/* Hexdump a buffer for easy comparison */
static void hexdump_line(const char *label, const uint8_t *data, size_t len, size_t offset)
{
    char hex_str[49] = {0};  /* 16 * 3 + 1 */
    char ascii_str[17] = {0};
    size_t i;
    
    size_t bytes_to_print = (len - offset > 16) ? 16 : (len - offset);
    
    for (i = 0; i < bytes_to_print; i++) {
        snprintf(&hex_str[i * 3], 4, "%02X ", data[offset + i]);
        ascii_str[i] = (data[offset + i] >= 32 && data[offset + i] <= 126) ? 
                       data[offset + i] : '.';
    }
    
    LOG_INF("%s[%04X]: %-48s |%s|", label, offset, hex_str, ascii_str);
}

void even_g1_diagnostic_dump_packet(const char *label, const uint8_t *data, size_t len)
{
    LOG_INF("=== %s: %zu bytes ===", label, len);
    
    for (size_t offset = 0; offset < len; offset += 16) {
        hexdump_line(label, data, len, offset);
    }
}

/* Analyze BMP file structure */
void even_g1_diagnostic_analyze_bmp(const uint8_t *bmp_data, size_t bmp_size)
{
    if (bmp_size < 62) {
        LOG_ERR("BMP too small: %zu bytes", bmp_size);
        return;
    }
    
    LOG_INF("=== BMP FILE ANALYSIS ===");
    LOG_INF("Total size: %zu bytes", bmp_size);
    
    /* File header */
    LOG_INF("File Header:");
    LOG_INF("  Magic: %c%c (should be 'BM')", bmp_data[0], bmp_data[1]);
    uint32_t file_size = *(uint32_t*)&bmp_data[2];
    uint32_t data_offset = *(uint32_t*)&bmp_data[10];
    LOG_INF("  File size: %u bytes", file_size);
    LOG_INF("  Data offset: %u (0x%02X)", data_offset, data_offset);
    
    /* DIB header */
    LOG_INF("DIB Header:");
    uint32_t header_size = *(uint32_t*)&bmp_data[14];
    int32_t width = *(int32_t*)&bmp_data[18];
    int32_t height = *(int32_t*)&bmp_data[22];
    uint16_t planes = *(uint16_t*)&bmp_data[26];
    uint16_t bits_per_pixel = *(uint16_t*)&bmp_data[28];
    uint32_t compression = *(uint32_t*)&bmp_data[30];
    
    LOG_INF("  Header size: %u", header_size);
    LOG_INF("  Width: %d", width);
    LOG_INF("  Height: %d", height);
    LOG_INF("  Planes: %u", planes);
    LOG_INF("  Bits/pixel: %u", bits_per_pixel);
    LOG_INF("  Compression: %u", compression);
    
    /* Color palette */
    if (data_offset >= 62) {
        LOG_INF("Color Palette:");
        LOG_INF("  Color 0: %02X %02X %02X %02X", 
                bmp_data[54], bmp_data[55], bmp_data[56], bmp_data[57]);
        LOG_INF("  Color 1: %02X %02X %02X %02X",
                bmp_data[58], bmp_data[59], bmp_data[60], bmp_data[61]);
    }
    
    /* First few bytes of pixel data */
    LOG_INF("First 32 bytes of pixel data:");
    hexdump_line("PIXELS", bmp_data, bmp_size, data_offset);
    if (bmp_size > data_offset + 16) {
        hexdump_line("PIXELS", bmp_data, bmp_size, data_offset + 16);
    }
}

/* Log exact packet sequence for comparison */
void even_g1_diagnostic_log_protocol_sequence(void)
{
    LOG_INF("=== EXPECTED PROTOCOL SEQUENCE ===");
    LOG_INF("1. First packet: [0x15, 0x00, 0x00, 0x1C, 0x00, 0x00] + 194 bytes of BMP");
    LOG_INF("2. Data packets: [0x15, seq_num] + 194 bytes of BMP");
    LOG_INF("3. End marker: [0x20, 0x0D, 0x0E]");
    LOG_INF("4. CRC packet: [0x16, CRC_MSB, CRC_2, CRC_3, CRC_LSB]");
    LOG_INF("");
    LOG_INF("Expected responses:");
    LOG_INF("- End ACK: [0x20, 0xC9, ...]");
    LOG_INF("- CRC success: [0x16, CRC_ECHO, 0xC9, ...]");
    LOG_INF("- CRC failure: [0x16, CRC_ECHO, 0xCA, ...]");
}

/* Compare two CRC calculations */
void even_g1_diagnostic_compare_crc(uint32_t our_crc, uint32_t expected_crc)
{
    LOG_INF("=== CRC COMPARISON ===");
    LOG_INF("Our CRC:      0x%08X", our_crc);
    LOG_INF("Expected CRC: 0x%08X", expected_crc);
    
    if (our_crc == expected_crc) {
        LOG_INF("✓ CRC values match!");
    } else {
        LOG_ERR("✗ CRC mismatch!");
        LOG_INF("Difference: 0x%08X", our_crc ^ expected_crc);
    }
    
    /* Show byte order variations */
    LOG_INF("Byte orders:");
    LOG_INF("  Big-endian:    %02X %02X %02X %02X",
            (our_crc >> 24) & 0xFF, (our_crc >> 16) & 0xFF,
            (our_crc >> 8) & 0xFF, our_crc & 0xFF);
    LOG_INF("  Little-endian: %02X %02X %02X %02X",
            our_crc & 0xFF, (our_crc >> 8) & 0xFF,
            (our_crc >> 16) & 0xFF, (our_crc >> 24) & 0xFF);
}

/* Test with known working BMP from official app */
void even_g1_diagnostic_test_official_bmp(void)
{
    LOG_INF("=== TESTING WITH OFFICIAL BMP FORMAT ===");
    LOG_INF("The official app sends these exact bytes for a simple test image:");
    LOG_INF("We need to capture actual BMP data from the official app");
    LOG_INF("Use BLE sniffer or add logging to the official app");
    LOG_INF("Then we can test with the exact same data");
}
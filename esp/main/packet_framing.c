/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "packet_framing.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "packet_framing";

/* Calculate CRC-16 CCITT */
uint16_t packet_framing_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF; // Initial value

    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021; // CCITT polynomial
            } else {
                crc = crc << 1;
            }
        }
    }

    return crc & 0xFFFF;
}

/* Frame a message for transmission */
int packet_framing_frame(const uint8_t *payload, uint16_t payload_len,
                         uint8_t *output, uint16_t output_size)
{
    if (!payload || !output) {
        return -1; // Invalid parameters
    }

    if (payload_len > FRAME_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "Payload too large: %d bytes (max %d)", payload_len, FRAME_MAX_PAYLOAD);
        return -2; // Payload too large
    }

    // Calculate total frame size
    uint16_t frame_size = FRAME_HEADER_SIZE + payload_len + FRAME_CRC_SIZE;
    if (frame_size > output_size) {
        ESP_LOGE(TAG, "Output buffer too small: %d bytes needed, %d available",
                 frame_size, output_size);
        return -3; // Output buffer too small
    }

    uint16_t idx = 0;

    // Add header magic bytes
    output[idx++] = FRAME_MAGIC1;
    output[idx++] = FRAME_MAGIC2;

    // Add length (big-endian, 2 bytes)
    output[idx++] = (payload_len >> 8) & 0xFF;  // High byte
    output[idx++] = payload_len & 0xFF;         // Low byte

    // Add payload
    memcpy(&output[idx], payload, payload_len);
    idx += payload_len;

    // Calculate and add CRC-16 CCITT
    uint16_t crc = packet_framing_crc16(payload, payload_len);
    output[idx++] = (crc >> 8) & 0xFF;  // High byte
    output[idx++] = crc & 0xFF;         // Low byte

    return frame_size;
}

/* Initialize packet reconstructor */
int packet_reconstructor_init(packet_reconstructor_t *reconstructor,
                              uint8_t *buffer, uint16_t buffer_size)
{
    if (!reconstructor || !buffer || buffer_size < 256) {
        return -1; // Invalid parameters
    }

    reconstructor->buffer = buffer;
    reconstructor->buffer_size = buffer_size;
    reconstructor->data_len = 0;
    reconstructor->offset = 0;
    reconstructor->packets_received = 0;
    reconstructor->crc_errors = 0;
    reconstructor->frame_errors = 0;

    return 0;
}

/* Add received data to reconstructor */
int packet_reconstructor_add_data(packet_reconstructor_t *reconstructor,
                                  const uint8_t *data, uint16_t len)
{
    if (!reconstructor || !data || len == 0) {
        return -1; // Invalid parameters
    }

    // Check if buffer has space
    if (reconstructor->data_len + len > reconstructor->buffer_size) {
        // Compact buffer if possible
        if (reconstructor->offset > 0 && reconstructor->data_len > reconstructor->offset) {
            uint16_t remaining = reconstructor->data_len - reconstructor->offset;
            memmove(reconstructor->buffer,
                   reconstructor->buffer + reconstructor->offset,
                   remaining);
            reconstructor->data_len = remaining;
            reconstructor->offset = 0;
        }

        // Check again after compaction
        if (reconstructor->data_len + len > reconstructor->buffer_size) {
            ESP_LOGW(TAG, "Buffer full, dropping %d bytes", len);
            return -2; // Buffer full
        }
    }

    // Add new data to buffer
    memcpy(reconstructor->buffer + reconstructor->data_len, data, len);
    reconstructor->data_len += len;

    return 0;
}

/* Try to extract a complete message from the reconstructor */
int packet_reconstructor_extract_message(packet_reconstructor_t *reconstructor,
                                         uint8_t *output, uint16_t output_size)
{
    if (!reconstructor || !output) {
        return -1; // Invalid parameters
    }

    // Compact buffer periodically
    if (reconstructor->offset >= 1024 &&
        reconstructor->offset < reconstructor->data_len) {
        uint16_t remaining = reconstructor->data_len - reconstructor->offset;
        memmove(reconstructor->buffer,
               reconstructor->buffer + reconstructor->offset,
               remaining);
        reconstructor->data_len = remaining;
        reconstructor->offset = 0;
    }

    // Search for frame header
    while (reconstructor->offset < reconstructor->data_len) {
        // Check if we have enough data for a header
        if (reconstructor->offset + FRAME_HEADER_SIZE > reconstructor->data_len) {
            // Compact buffer if we've consumed most of it
            if (reconstructor->offset > 0) {
                uint16_t remaining = reconstructor->data_len - reconstructor->offset;
                memmove(reconstructor->buffer,
                       reconstructor->buffer + reconstructor->offset,
                       remaining);
                reconstructor->data_len = remaining;
                reconstructor->offset = 0;
            }
            return 0; // Need more data
        }

        uint8_t magic1 = reconstructor->buffer[reconstructor->offset];
        uint8_t magic2 = reconstructor->buffer[reconstructor->offset + 1];

        // Check for valid header
        if (magic1 != FRAME_MAGIC1 || magic2 != FRAME_MAGIC2) {
            // Invalid header, skip one byte and try again (frame resync)
            reconstructor->offset++;
            reconstructor->frame_errors++;
            continue;
        }

        // Valid header found - extract length
        uint16_t length_high = reconstructor->buffer[reconstructor->offset + 2];
        uint16_t length_low = reconstructor->buffer[reconstructor->offset + 3];
        uint16_t payload_len = (length_high << 8) | length_low;

        // Validate payload length
        if (payload_len > FRAME_MAX_PAYLOAD) {
            ESP_LOGE(TAG, "Invalid payload length: %d", payload_len);
            reconstructor->offset++;
            reconstructor->frame_errors++;
            continue;
        }

        // Calculate total packet size
        uint16_t total_size = FRAME_HEADER_SIZE + payload_len + FRAME_CRC_SIZE;

        // Check if we have the complete packet
        if (reconstructor->offset + total_size > reconstructor->data_len) {
            // Incomplete packet, need more data
            if (reconstructor->offset > 0) {
                uint16_t remaining = reconstructor->data_len - reconstructor->offset;
                memmove(reconstructor->buffer,
                       reconstructor->buffer + reconstructor->offset,
                       remaining);
                reconstructor->data_len = remaining;
                reconstructor->offset = 0;
            }
            return 0; // Need more data
        }

        // Check if output buffer is large enough
        if (payload_len > output_size) {
            ESP_LOGE(TAG, "Output buffer too small: %d bytes needed, %d available",
                     payload_len, output_size);
            reconstructor->offset++;
            reconstructor->frame_errors++;
            continue;
        }

        // Extract payload
        uint8_t *payload = reconstructor->buffer + reconstructor->offset + FRAME_HEADER_SIZE;

        // Extract CRC bytes
        uint16_t crc_offset = reconstructor->offset + FRAME_HEADER_SIZE + payload_len;
        uint16_t crc_high = reconstructor->buffer[crc_offset];
        uint16_t crc_low = reconstructor->buffer[crc_offset + 1];
        uint16_t received_crc = (crc_high << 8) | crc_low;

        // Validate CRC
        uint16_t calculated_crc = packet_framing_crc16(payload, payload_len);
        if (calculated_crc != received_crc) {
            ESP_LOGE(TAG, "CRC mismatch: calculated=0x%04X, received=0x%04X",
                     calculated_crc, received_crc);
            reconstructor->crc_errors++;
            // Skip this packet and try to resync
            reconstructor->offset++;
            continue; // Don't return error - just continue searching
        }

        // Valid packet found!
        reconstructor->packets_received++;

        // Copy payload to output
        memcpy(output, payload, payload_len);

        // Advance offset past this packet
        reconstructor->offset += total_size;

        return payload_len;
    }

    // Reached end of buffer without finding complete packet
    if (reconstructor->offset > 0) {
        uint16_t remaining = reconstructor->data_len - reconstructor->offset;
        memmove(reconstructor->buffer,
               reconstructor->buffer + reconstructor->offset,
               remaining);
        reconstructor->data_len = remaining;
        reconstructor->offset = 0;
    }

    return 0; // Need more data
}

/* Reset the packet reconstructor state */
void packet_reconstructor_reset(packet_reconstructor_t *reconstructor)
{
    if (!reconstructor) {
        return;
    }

    reconstructor->data_len = 0;
    reconstructor->offset = 0;
    ESP_LOGI(TAG, "Packet reconstructor reset");
}

/* Get current buffer usage */
uint16_t packet_reconstructor_buffer_usage(const packet_reconstructor_t *reconstructor)
{
    if (!reconstructor) {
        return 0;
    }

    return reconstructor->data_len - reconstructor->offset;
}

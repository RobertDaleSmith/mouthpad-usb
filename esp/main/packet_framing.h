/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PACKET_FRAMING_H
#define PACKET_FRAMING_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Packet framing constants */
#define FRAME_MAGIC1          0xAA
#define FRAME_MAGIC2          0x55
#define FRAME_HEADER_SIZE     4    // 2 magic bytes + 2 length bytes
#define FRAME_CRC_SIZE        2    // 2 bytes for CRC-16
#define FRAME_MAX_PAYLOAD     65535 // Maximum payload size

/* Packet reconstructor state */
typedef struct {
    uint8_t *buffer;        // Buffer for accumulating received data
    uint16_t buffer_size;   // Total buffer size
    uint16_t data_len;      // Current amount of data in buffer
    uint16_t offset;        // Current parsing offset

    // Statistics
    uint32_t packets_received;
    uint32_t crc_errors;
    uint32_t frame_errors;
} packet_reconstructor_t;

/**
 * @brief Calculate CRC-16 CCITT for data
 *
 * Polynomial: 0x1021 (x^16 + x^12 + x^5 + 1)
 * Initial value: 0xFFFF
 *
 * @param data Pointer to data
 * @param len Length of data
 * @return CRC-16 value
 */
uint16_t packet_framing_crc16(const uint8_t *data, uint16_t len);

/**
 * @brief Frame a message for transmission
 *
 * Creates framed packet: [0xAA 0x55] [len_h len_l] [payload...] [crc_h crc_l]
 *
 * @param payload Pointer to payload data
 * @param payload_len Length of payload (max 65535 bytes)
 * @param output Buffer to store framed packet
 * @param output_size Size of output buffer
 * @return Number of bytes written to output, or negative error code
 */
int packet_framing_frame(const uint8_t *payload, uint16_t payload_len,
                         uint8_t *output, uint16_t output_size);

/**
 * @brief Initialize packet reconstructor
 *
 * @param reconstructor Pointer to reconstructor structure
 * @param buffer Buffer for accumulating received data
 * @param buffer_size Size of buffer (recommended: 2048+ bytes)
 * @return 0 on success, negative error code on failure
 */
int packet_reconstructor_init(packet_reconstructor_t *reconstructor,
                              uint8_t *buffer, uint16_t buffer_size);

/**
 * @brief Add received data to reconstructor
 *
 * @param reconstructor Pointer to reconstructor structure
 * @param data Pointer to received data
 * @param len Length of received data
 * @return 0 on success, negative error code on failure
 */
int packet_reconstructor_add_data(packet_reconstructor_t *reconstructor,
                                  const uint8_t *data, uint16_t len);

/**
 * @brief Try to extract a complete message from the reconstructor
 *
 * This function searches for a valid frame header, validates the CRC,
 * and extracts the payload if a complete valid packet is found.
 *
 * @param reconstructor Pointer to reconstructor structure
 * @param output Buffer to store extracted payload
 * @param output_size Size of output buffer
 * @return Number of payload bytes extracted (> 0), 0 if more data needed,
 *         or negative error code on CRC/frame error
 */
int packet_reconstructor_extract_message(packet_reconstructor_t *reconstructor,
                                         uint8_t *output, uint16_t output_size);

/**
 * @brief Reset the packet reconstructor state
 *
 * @param reconstructor Pointer to reconstructor structure
 */
void packet_reconstructor_reset(packet_reconstructor_t *reconstructor);

/**
 * @brief Get current buffer usage
 *
 * @param reconstructor Pointer to reconstructor structure
 * @return Number of bytes currently in buffer
 */
uint16_t packet_reconstructor_buffer_usage(const packet_reconstructor_t *reconstructor);

#ifdef __cplusplus
}
#endif

#endif /* PACKET_FRAMING_H */

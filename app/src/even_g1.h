/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef EVEN_G1_H
#define EVEN_G1_H

#include <zephyr/bluetooth/conn.h>
#include <stdint.h>
#include <stdbool.h>

/* Even G1 Opcodes */
#define EVEN_G1_OPCODE_TEXT     0x4E
#define EVEN_G1_OPCODE_BITMAP   0x15
#define EVEN_G1_OPCODE_BMP_END  0x20
#define EVEN_G1_OPCODE_BMP_CRC  0x16
#define EVEN_G1_OPCODE_MIC      0x0E
#define EVEN_G1_OPCODE_MIC_DATA 0xF1
#define EVEN_G1_OPCODE_EVENT    0xF5

/* Even G1 Events (0xF5 followed by event code) */
#define EVEN_G1_EVENT_DOUBLE_TAP_CLOSE 0x00  /* Close features/turn off display */
#define EVEN_G1_EVENT_SINGLE_TAP       0x01
#define EVEN_G1_EVENT_DASHBOARD_CLOSE  0x02  /* Dashboard closes (head tilt down) */
#define EVEN_G1_EVENT_DASHBOARD_OPEN   0x03  /* Dashboard opens (head tilt up) */
#define EVEN_G1_EVENT_TRIPLE_TAP_ON    0x04  /* Toggle silent mode on */
#define EVEN_G1_EVENT_TRIPLE_TAP_OFF   0x05  /* Toggle silent mode off */
#define EVEN_G1_EVENT_STATUS_09         0x09  /* Unknown status */
#define EVEN_G1_EVENT_STATUS_0A         0x0A  /* Unknown status with value 0x64 */
#define EVEN_G1_EVENT_STATUS_11         0x11  /* Unknown status - appears after text send */
#define EVEN_G1_EVENT_LONG_PRESS       0x17
#define EVEN_G1_EVENT_STATUS_1E         0x1E  /* Unknown status */

/* Even G1 Command Queue */
#define EVEN_G1_MAX_QUEUE_SIZE 8

typedef enum {
    EVEN_G1_CMD_IDLE,
    EVEN_G1_CMD_PENDING,      /* Queued, not yet sent */
    EVEN_G1_CMD_LEFT_SENT,    /* Sent to left arm, waiting for ACK */
    EVEN_G1_CMD_LEFT_ACKED,   /* Left arm ACKed, sending to right arm */
    EVEN_G1_CMD_RIGHT_SENT,   /* Sent to right arm, waiting for ACK */
    EVEN_G1_CMD_COMPLETED,    /* Both arms ACKed, command complete */
    EVEN_G1_CMD_FAILED        /* Command failed */
} even_g1_cmd_state_t;

typedef enum {
    EVEN_G1_CMD_TYPE_TEXT,    /* Text display command */
    EVEN_G1_CMD_TYPE_BITMAP,  /* Bitmap display command */
    EVEN_G1_CMD_TYPE_CLEAR,   /* Clear display command */
    EVEN_G1_CMD_TYPE_MIC      /* Microphone command (right arm only) */
} even_g1_cmd_type_t;

typedef struct {
    even_g1_cmd_type_t type;
    even_g1_cmd_state_t state;
    uint8_t packet[256];
    uint16_t packet_len;
    uint32_t timestamp;       /* For timeout handling */
    bool dual_arm;            /* true = send to both arms, false = right arm only */
} even_g1_command_t;

/* Even G1 State */
typedef struct {
    struct bt_conn *left_conn;
    struct bt_conn *right_conn;
    bool left_ready;
    bool right_ready;
    bool left_nus_ready;
    bool right_nus_ready;
    bool left_security_ready;
    bool right_security_ready;
    uint16_t left_mtu;
    uint16_t right_mtu;
    uint8_t sequence;
    bool waiting_for_left_ack;
    bool waiting_for_right_ack;
    bool left_ack_received;
    bool right_ack_received;
    uint8_t last_sent_packet[256];
    uint16_t last_sent_packet_len;
    /* Command queue */
    even_g1_command_t cmd_queue[EVEN_G1_MAX_QUEUE_SIZE];
    uint8_t queue_head;           /* Next command to process */
    uint8_t queue_tail;           /* Next slot to add command */
    uint8_t queue_count;          /* Number of commands in queue */
    even_g1_command_t *current_cmd; /* Currently processing command */
    
    /* Dashboard state */
    bool dashboard_open;          /* true = dashboard open (0x03), false = dashboard closed (0x02) */
} even_g1_state_t;

/* Initialization and connection management */
int even_g1_init(void);
int even_g1_connect_left(struct bt_conn *conn);
int even_g1_connect_right(struct bt_conn *conn);
int even_g1_disconnect_left(void);
int even_g1_disconnect_right(void);
bool even_g1_is_connected(void);
bool even_g1_is_ready(void);

/* Dashboard state management */
bool even_g1_is_dashboard_open(void);

/* NUS service callbacks */
void even_g1_nus_discovered(struct bt_conn *conn);
void even_g1_nus_data_received(struct bt_conn *conn, const uint8_t *data, uint16_t len);
void even_g1_mtu_exchanged(struct bt_conn *conn, uint16_t mtu);
void even_g1_security_changed(struct bt_conn *conn, bt_security_t level);

/* Protocol functions */
int even_g1_send_text_dual_arm(const char *text);
int even_g1_send_text_formatted_dual_arm(const char *line1, const char *line2, 
                                          const char *line3, const char *line4, 
                                          const char *line5);
int even_g1_send_text(const char *text);
int even_g1_send_text_formatted(const char *line1, const char *line2, 
                                const char *line3, const char *line4, 
                                const char *line5);
int even_g1_send_bitmap(const uint8_t *bitmap_data, size_t width, size_t height);
int even_g1_clear_display(void);

/* MouthPad status display */
int even_g1_show_mouthpad_status(bool connected, int8_t rssi, uint8_t battery);
int even_g1_show_mouse_event(int16_t x, int16_t y, uint8_t buttons);

/* Queue-based command system */
int even_g1_queue_command(even_g1_cmd_type_t type, const uint8_t *packet, uint16_t packet_len, bool dual_arm);
int even_g1_send_data(const uint8_t *data, uint16_t len, even_g1_cmd_type_t type, bool dual_arm);
void even_g1_process_queue(void);

/* Bitmap/image functions */
int even_g1_send_bitmap(const uint8_t *bitmap_data, size_t width, size_t height);
int even_g1_clear_display_dual_arm(void);

/* Debug/status */
void even_g1_print_status(void);
void even_g1_show_current_status(void);

#endif /* EVEN_G1_H */
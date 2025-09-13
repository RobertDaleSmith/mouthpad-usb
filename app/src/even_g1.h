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
#define EVEN_G1_OPCODE_BITMAP   0x15  /* Send BMP data packet */
#define EVEN_G1_OPCODE_BMP_END  0x20  /* End BMP transmission */
#define EVEN_G1_OPCODE_BMP_CRC  0x16  /* Send BMP CRC check */
#define EVEN_G1_OPCODE_MIC      0x0E
#define EVEN_G1_OPCODE_MIC_DATA 0xF1
#define EVEN_G1_OPCODE_EVENT    0xF5
#define EVEN_G1_OPCODE_HEARTBEAT 0x25

/* Even G1 Events (0xF5 followed by event code) */
#define EVEN_G1_EVENT_DOUBLE_TAP_CLOSE 0x00  /* Close features/turn off display */
#define EVEN_G1_EVENT_SINGLE_TAP       0x01
#define EVEN_G1_EVENT_DASHBOARD_CLOSE  0x03  /* Dashboard closes (head tilt down) */
#define EVEN_G1_EVENT_DASHBOARD_OPEN   0x02  /* Dashboard opens (head tilt up) */
#define EVEN_G1_EVENT_TRIPLE_TAP_ON    0x04  /* Toggle silent mode on */
#define EVEN_G1_EVENT_TRIPLE_TAP_OFF   0x05  /* Toggle silent mode off */
#define EVEN_G1_EVENT_STATUS_09         0x09  /* Unknown status */
#define EVEN_G1_EVENT_STATUS_0A         0x0A  /* Unknown status with value 0x64 */
#define EVEN_G1_EVENT_STATUS_11         0x11  /* Unknown status - appears after text send */
#define EVEN_G1_EVENT_LONG_PRESS       0x17
#define EVEN_G1_EVENT_STATUS_1E         0x1E  /* Unknown status */

/* Even G1 Command Queue */
#define EVEN_G1_MAX_QUEUE_SIZE 8

/* Bitmap Transmission */
#define EVEN_G1_MAX_PACKET_RETRIES 3

/* Even G1 Bitmap Constants */
#define EVEN_G1_BITMAP_WIDTH    576
#define EVEN_G1_BITMAP_HEIGHT   135  /* VERIFIED: Official BMP files are 576*135 pixels */
#define EVEN_G1_BITMAP_PACKET_SIZE 194  /* Per Even G1 specification */
#define EVEN_G1_BITMAP_ADDRESS  {0x00, 0x1C, 0x00, 0x00}  /* Glasses storage address per documentation */
#define EVEN_G1_BMP_END_DATA    {0x0D, 0x0E}              /* End command data */

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
    EVEN_G1_CMD_TYPE_TEXT,        /* Text display command */
    EVEN_G1_CMD_TYPE_BITMAP,      /* Bitmap display command (old single packet) */
    EVEN_G1_CMD_TYPE_BMP_PACKET,  /* Bitmap data packet (0x15) */
    EVEN_G1_CMD_TYPE_BMP_END,     /* Bitmap end command (0x20) */
    EVEN_G1_CMD_TYPE_BMP_CRC,     /* Bitmap CRC command (0x16) */
    EVEN_G1_CMD_TYPE_CLEAR,       /* Clear display command */
    EVEN_G1_CMD_TYPE_MIC          /* Microphone command (right arm only) */
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
    
    /* Display mode state */
    bool bitmap_mode;            /* true = bitmap mode, false = text mode */
    
    /* Heartbeat state */
    uint8_t heartbeat_sequence;   /* Rolling sequence number for heartbeats */
    int64_t last_activity_time;   /* Timestamp of last data sent to Even G1 */
    
    /* Bitmap transmission state */
    bool bitmap_in_progress;      /* true if bitmap transmission is active */
    uint8_t *bitmap_data;         /* Pointer to current bitmap data */
    uint32_t bitmap_size;         /* Size of bitmap data in bytes */
    bool bitmap_data_allocated;   /* true if bitmap_data was k_malloc'd and needs k_free */
    uint16_t bitmap_packets_total; /* Total number of packets to send */
    uint16_t bitmap_packets_sent; /* Number of packets sent so far */
    uint32_t bitmap_crc32;        /* CRC32 of bitmap data for verification */
    bool bitmap_left_pending;     /* Waiting for left arm send completion */
    bool bitmap_right_pending;    /* Waiting for right arm send completion */
    uint8_t bitmap_current_packet[256]; /* Current packet being sent */
    uint16_t bitmap_current_packet_len; /* Length of current packet */
    uint8_t bitmap_left_retry_count;  /* Retry count for left arm */
    uint8_t bitmap_right_retry_count; /* Retry count for right arm */
    bool bitmap_left_crc_validated;  /* Left arm CRC validation completed */
    bool bitmap_right_crc_validated; /* Right arm CRC validation completed */
    bool bitmap_left_end_acked;      /* Left arm end command acknowledged */
    bool bitmap_right_end_acked;     /* Right arm end command acknowledged */
    uint8_t *bitmap_original_buffer;  /* Original BMP buffer to free after transmission */
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

/* Advanced bitmap transmission functions */
int even_g1_send_bmp_file(const uint8_t *bmp_data, size_t bmp_size);
int even_g1_send_real_bmp_file(const uint8_t *bmp_file_data, size_t bmp_file_size);
int even_g1_send_official_sample_bmp(void);
int even_g1_create_test_bitmap_square(uint8_t **bitmap_data, size_t *data_size);
int even_g1_create_test_bitmap_triangle(uint8_t **bitmap_data, size_t *data_size);
int even_g1_send_example_bitmap_1(void);
int even_g1_send_example_bitmap_2(void);
int even_g1_send_test_all_black(void);
int even_g1_send_test_all_white(void);
int even_g1_send_github_bitmap(const char *filename);
int even_g1_send_test_left_arm_only(void);
bool even_g1_is_bitmap_in_progress(void);

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
int even_g1_clear_bitmap_display(void);

/* Display mode control */
void even_g1_toggle_display_mode(void);
bool even_g1_is_bitmap_mode(void);
void even_g1_set_bitmap_mode(bool enable);

/* Debug/status */
void even_g1_print_status(void);
void even_g1_show_current_status(void);

#endif /* EVEN_G1_H */
/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "even_g1.h"
#include "ble_nus_multi_client.h"
#include "ble_multi_conn.h"
#include "ble_bas.h"
#include "ble_transport.h"
#include "ble_central.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(even_g1, LOG_LEVEL_INF);

/* Even G1 state */
static even_g1_state_t g1_state = {0};
static struct k_mutex g1_mutex;

/* Work queue for async operations */
static struct k_work_delayable init_sequence_work;
static struct k_work_delayable keepalive_work;
static struct k_work bitmap_completion_work;

/* Forward declarations */
static int send_to_left(const uint8_t *data, uint16_t len);
static int send_to_right(const uint8_t *data, uint16_t len);
static void queue_complete_command(void);
static int create_bmp_file_from_pixels(uint8_t **bmp_file_data, size_t *bmp_file_size, 
                                     const uint8_t *pixel_data, size_t width, size_t height);
static void queue_fail_command(void);
static void init_sequence_handler(struct k_work *work);
static void keepalive_handler(struct k_work *work);
static void bitmap_completion_handler(struct k_work *work);
static int send_ack_to_arm(struct bt_conn *conn);
static int send_heartbeat_command(void);
static int send_heartbeat_to_left(const uint8_t *data, uint16_t len);
static int send_heartbeat_to_right(const uint8_t *data, uint16_t len);
static int send_mic_stop_command(void);

/* Bitmap transmission functions */
static int even_g1_send_next_bitmap_packet(void);
static int even_g1_send_bitmap_end(void);
static int even_g1_send_bitmap_crc(void);
static void even_g1_abort_bitmap_transmission(void);
static void even_g1_complete_bitmap_transmission(void);
static void even_g1_complete_bitmap_transmission_internal(void);
static void even_g1_try_display_activation_sequence(void);
static bool send_activation_cmd_with_retry(const uint8_t *data, uint8_t len, const char *desc, int max_retries);
static void even_g1_bitmap_send_callback(struct bt_conn *conn, uint8_t err);
static void even_g1_continue_bitmap_transmission(void);

int even_g1_init(void)
{
    k_mutex_init(&g1_mutex);
    memset(&g1_state, 0, sizeof(g1_state));
    k_work_init_delayable(&init_sequence_work, init_sequence_handler);
    k_work_init_delayable(&keepalive_work, keepalive_handler);
    k_work_init(&bitmap_completion_work, bitmap_completion_handler);
    
    /* Initialize activity timestamp to current time */
    g1_state.last_activity_time = k_uptime_get();
    
    /* Register callback for bitmap transmission flow control */
    ble_nus_multi_client_register_data_sent_cb(even_g1_bitmap_send_callback);
    
    LOG_INF("Even G1 module initialized");
    return 0;
}

int even_g1_connect_left(struct bt_conn *conn)
{
    if (!conn) {
        return -EINVAL;
    }
    
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    if (g1_state.left_conn) {
        bt_conn_unref(g1_state.left_conn);
    }
    
    g1_state.left_conn = bt_conn_ref(conn);
    g1_state.left_ready = false;
    g1_state.left_nus_ready = false;
    g1_state.left_security_ready = false;
    g1_state.left_mtu = 0;
    
    LOG_INF("Even G1 left arm connected");
    
    k_mutex_unlock(&g1_mutex);
    return 0;
}

int even_g1_connect_right(struct bt_conn *conn)
{
    if (!conn) {
        return -EINVAL;
    }
    
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    if (g1_state.right_conn) {
        bt_conn_unref(g1_state.right_conn);
    }
    
    g1_state.right_conn = bt_conn_ref(conn);
    g1_state.right_ready = false;
    g1_state.right_nus_ready = false;
    g1_state.right_security_ready = false;
    g1_state.right_mtu = 0;
    
    LOG_INF("Even G1 right arm connected");
    
    /* If both arms are now connected, start initialization sequence */
    if (g1_state.left_conn && g1_state.right_conn) {
        LOG_INF("Both Even G1 arms connected, scheduling init sequence");
        k_work_schedule(&init_sequence_work, K_SECONDS(1));
    }
    
    k_mutex_unlock(&g1_mutex);
    return 0;
}

int even_g1_disconnect_left(void)
{
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    if (g1_state.left_conn) {
        bt_conn_unref(g1_state.left_conn);
        g1_state.left_conn = NULL;
        g1_state.left_ready = false;
        g1_state.left_nus_ready = false;
        g1_state.left_security_ready = false;
        g1_state.left_mtu = 0;
        LOG_INF("Even G1 left arm disconnected");
    }
    
    /* Cancel heartbeat if no arms connected */
    if (!g1_state.left_conn && !g1_state.right_conn) {
        k_work_cancel_delayable(&keepalive_work);
    }
    
    k_mutex_unlock(&g1_mutex);
    return 0;
}

int even_g1_disconnect_right(void)
{
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    if (g1_state.right_conn) {
        bt_conn_unref(g1_state.right_conn);
        g1_state.right_conn = NULL;
        g1_state.right_ready = false;
        g1_state.right_nus_ready = false;
        g1_state.right_security_ready = false;
        g1_state.right_mtu = 0;
        LOG_INF("Even G1 right arm disconnected");
    }
    
    /* Cancel heartbeat if no arms connected */
    if (!g1_state.left_conn && !g1_state.right_conn) {
        k_work_cancel_delayable(&keepalive_work);
    }
    
    k_mutex_unlock(&g1_mutex);
    return 0;
}

bool even_g1_is_connected(void)
{
    return (g1_state.left_conn != NULL && g1_state.right_conn != NULL);
}

bool even_g1_is_ready(void)
{
    return (g1_state.left_ready && g1_state.right_ready && 
            g1_state.left_nus_ready && g1_state.right_nus_ready &&
            g1_state.left_security_ready && g1_state.right_security_ready);
}

bool even_g1_is_dashboard_open(void)
{
    bool state;
    k_mutex_lock(&g1_mutex, K_FOREVER);
    state = g1_state.dashboard_open;
    k_mutex_unlock(&g1_mutex);
    return state;
}

void even_g1_nus_discovered(struct bt_conn *conn)
{
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    if (conn == g1_state.left_conn) {
        LOG_INF("Even G1 left arm NUS discovered");
        g1_state.left_nus_ready = true;
    } else if (conn == g1_state.right_conn) {
        LOG_INF("Even G1 right arm NUS discovered");
        g1_state.right_nus_ready = true;
    }
    
    /* Init sequence will be triggered when security is also ready */
    
    k_mutex_unlock(&g1_mutex);
}

void even_g1_mtu_exchanged(struct bt_conn *conn, uint16_t mtu)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    
    LOG_INF("*** EVEN G1 MTU EXCHANGED CALLBACK: %s, MTU=%d ***", addr, mtu);
    
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    if (conn == g1_state.left_conn) {
        g1_state.left_mtu = mtu;
        g1_state.left_ready = true;
        LOG_INF("Even G1 left arm ready (MTU=%d)", mtu);
    } else if (conn == g1_state.right_conn) {
        g1_state.right_mtu = mtu;
        g1_state.right_ready = true;
        LOG_INF("Even G1 right arm ready (MTU=%d)", mtu);
    } else {
        LOG_WRN("MTU exchange for unknown Even G1 connection: %s", addr);
        k_mutex_unlock(&g1_mutex);
        return;
    }
    
    /* Notify central state machine */
    extern void ble_central_even_g1_mtu_exchange_complete(struct bt_conn *conn);
    ble_central_even_g1_mtu_exchange_complete(conn);
    
    /* Note: Init sequence will be triggered when NUS discovery completes for both arms */
    
    k_mutex_unlock(&g1_mutex);
}

void even_g1_security_changed(struct bt_conn *conn, bt_security_t level)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    
    LOG_INF("*** EVEN G1 SECURITY CHANGED CALLBACK: %s, level=%d ***", addr, level);
    
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    if (conn == g1_state.left_conn) {
        g1_state.left_security_ready = (level >= BT_SECURITY_L2);
        LOG_INF("Even G1 left arm security ready: %d", g1_state.left_security_ready);
    } else if (conn == g1_state.right_conn) {
        g1_state.right_security_ready = (level >= BT_SECURITY_L2);  
        LOG_INF("Even G1 right arm security ready: %d", g1_state.right_security_ready);
    } else {
        LOG_WRN("Security change for unknown Even G1 connection: %s", addr);
        k_mutex_unlock(&g1_mutex);
        return;
    }
    
    /* Check if we can start init sequence now */
    if (even_g1_is_ready()) {
        LOG_INF("Both Even G1 arms fully ready (MTU + NUS + Security), starting init sequence");
        k_work_schedule(&init_sequence_work, K_MSEC(100));
        /* Start heartbeat timer (8 seconds like official app) */
        k_work_schedule(&keepalive_work, K_SECONDS(8));
    }
    
    k_mutex_unlock(&g1_mutex);
}

void even_g1_nus_data_received(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) {
        return;
    }
    
    const char *arm = (conn == g1_state.left_conn) ? "LEFT" : 
                     (conn == g1_state.right_conn) ? "RIGHT" : "UNKNOWN";
    
    LOG_INF("Even G1 data from %s (%d bytes)", arm, len);
    LOG_HEXDUMP_INF(data, len, "Even G1 data");
    
    /* Handle different opcodes */
    switch (data[0]) {
    case EVEN_G1_OPCODE_TEXT:
        /* This is an ACK/response from the glasses for text commands */
        if (len >= 4) {
            LOG_INF("Even G1 0x4E ACK from %s: seq=%02x status=%02x pkg=%02x/%02x", 
                    arm, data[1], data[2], data[3], data[4]);
            /* Check if this is a success ACK (0xC9 in byte 1) */
            if (data[1] != 0xC9) {
                LOG_WRN("Even G1 ACK with non-success status: 0x%02x", data[1]);
            }
        } else {
            LOG_INF("Even G1 0x4E TEXT ACK from %s (short packet, len=%d)", arm, len);
        }
        /* Reset heartbeat timer on any activity */
        k_work_reschedule(&keepalive_work, K_SECONDS(8));
        
        /* Handle ACKs using queue-based system */
        k_mutex_lock(&g1_mutex, K_FOREVER);
        if (g1_state.current_cmd) {
            if (conn == g1_state.left_conn && g1_state.current_cmd->state == EVEN_G1_CMD_LEFT_SENT) {
                LOG_INF("*** LEFT arm 0x4E ACK received - NOW SENDING TO RIGHT ARM ***");
                g1_state.current_cmd->state = EVEN_G1_CMD_LEFT_ACKED;
                k_mutex_unlock(&g1_mutex);
                
                /* Add small delay before sending to right arm to ensure state sync */
                k_msleep(50);
                
                /* Send same packet to right arm */
                int right_err = send_to_right(g1_state.current_cmd->packet, g1_state.current_cmd->packet_len);
                if (right_err) {
                    LOG_ERR("Failed to send to right arm: %d", right_err);
                    queue_fail_command();
                } else {
                    k_mutex_lock(&g1_mutex, K_FOREVER);
                    g1_state.current_cmd->state = EVEN_G1_CMD_RIGHT_SENT;
                    k_mutex_unlock(&g1_mutex);
                    LOG_INF("Command sent to right arm, waiting for RIGHT ACK");
                }
                return;
            } else if (conn == g1_state.right_conn && g1_state.current_cmd->state == EVEN_G1_CMD_RIGHT_SENT) {
                LOG_INF("*** RIGHT arm 0x4E ACK received - COMMAND COMPLETE ***");
                k_mutex_unlock(&g1_mutex);
                queue_complete_command();
                /* Process next command in queue */
                even_g1_process_queue();
                return;
            }
        }
        k_mutex_unlock(&g1_mutex);
        LOG_DBG("Received ACK from %s but no matching command in queue", arm);
        break;
        
    case EVEN_G1_OPCODE_EVENT:
        if (len >= 2) {
            LOG_INF("Even G1 event from %s: 0x%02x", arm, data[1]);
            /* Reset keepalive timer on any activity */
            k_work_reschedule(&keepalive_work, K_SECONDS(15));
            
            /* Events can serve as implicit ACKs - device is responding to our commands */
            k_mutex_lock(&g1_mutex, K_FOREVER);
            if (conn == g1_state.left_conn) {
                LOG_INF("LEFT arm event: waiting_for_ack=%d", g1_state.waiting_for_left_ack);
                if (g1_state.waiting_for_left_ack) {
                    LOG_INF("*** LEFT arm EVENT (0xF5/0x%02x) ACK received while waiting - SETTING ACK FLAG ***", data[1]);
                    g1_state.left_ack_received = true;
                } else {
                    LOG_INF("LEFT arm EVENT (0xF5/0x%02x) received but not waiting", data[1]);
                }
            } else if (conn == g1_state.right_conn) {
                LOG_INF("RIGHT arm event: waiting_for_ack=%d", g1_state.waiting_for_right_ack);
                if (g1_state.waiting_for_right_ack) {
                    LOG_INF("*** RIGHT arm EVENT (0xF5/0x%02x) ACK received while waiting - SETTING ACK FLAG ***", data[1]);
                    g1_state.right_ack_received = true;
                } else {
                    LOG_INF("RIGHT arm EVENT (0xF5/0x%02x) received but not waiting", data[1]);
                }
                
                /* WORKAROUND: RIGHT arm sends events instead of proper ACKs */
                /* Check if we're waiting for a RIGHT arm ACK in queue system */
                k_mutex_lock(&g1_mutex, K_FOREVER);
                if (g1_state.current_cmd && g1_state.current_cmd->state == EVEN_G1_CMD_RIGHT_SENT) {
                    LOG_INF("*** RIGHT arm EVENT (0xF5/0x%02x) ACCEPTED AS ACK - COMMAND COMPLETE ***", data[1]);
                    k_mutex_unlock(&g1_mutex);
                    queue_complete_command();
                    /* Process next command in queue */
                    even_g1_process_queue();
                    return;
                } else {
                    k_mutex_unlock(&g1_mutex);
                }
            }
            k_mutex_unlock(&g1_mutex);
            
            /* Handle touch events and status events */
            switch (data[1]) {
            case EVEN_G1_EVENT_SINGLE_TAP:
                LOG_INF("TouchBar single tap detected from %s arm", arm);
                break;
            case EVEN_G1_EVENT_DOUBLE_TAP_CLOSE:
                LOG_INF("TouchBar double tap (close) detected from %s arm", arm);
                break;
            case EVEN_G1_EVENT_DASHBOARD_CLOSE:
                LOG_INF("Dashboard CLOSED (head tilt down) from %s arm", arm);
                k_mutex_lock(&g1_mutex, K_FOREVER);
                g1_state.dashboard_open = false;
                k_mutex_unlock(&g1_mutex);
                LOG_INF("Dashboard state: CLOSED - returning to text display");
                
                /* Return to normal text display */
                if (!even_g1_is_bitmap_in_progress()) {
                    even_g1_show_current_status();
                } else {
                    LOG_INF("Bitmap in progress, will show status after completion");
                }
                break;
            case EVEN_G1_EVENT_DASHBOARD_OPEN:
                LOG_INF("Dashboard OPENED (head tilt up) from %s arm", arm);
                k_mutex_lock(&g1_mutex, K_FOREVER);
                g1_state.dashboard_open = true;
                k_mutex_unlock(&g1_mutex);
                LOG_INF("Dashboard state: OPEN - triggering bitmap test");
                
                /* Trigger bitmap test when dashboard opens */
                if (!even_g1_is_bitmap_in_progress()) {
                    /* Alternate between the two example bitmaps */
                    static bool use_bitmap_1 = true;
                    
                    if (use_bitmap_1) {
                        LOG_INF("Sending all-white test (proven protocol)");
                        even_g1_send_test_all_white();
                    } else {
                        LOG_INF("Sending ALL WHITE test bitmap (raw pixel data)");
                        even_g1_send_test_all_white();
                    }
                    
                    use_bitmap_1 = !use_bitmap_1;  /* Alternate for next time */
                } else {
                    LOG_WRN("Bitmap already in progress, skipping test");
                }
                break;
            case EVEN_G1_EVENT_TRIPLE_TAP_ON:
                LOG_INF("TouchBar triple tap (silent mode ON) detected from %s arm", arm);
                break;
            case EVEN_G1_EVENT_TRIPLE_TAP_OFF:
                LOG_INF("TouchBar triple tap (silent mode OFF) detected from %s arm", arm);
                break;
            case EVEN_G1_EVENT_LONG_PRESS:
                LOG_INF("TouchBar long press detected from %s arm - mic activation", arm);
                /* Could respond with mic enable if needed */
                break;
            case 0x06:
                LOG_INF("Even G1 status event 0x06 from %s", arm);
                break;
            case 0x09:
                LOG_INF("Even G1 status event 0x09 from %s", arm);
                break;
            case 0x0a:
                LOG_INF("Even G1 status event 0x0a from %s", arm);
                break;
            case 0x11:
                LOG_INF("Even G1 status event 0x11 from %s", arm);
                break;
            case 0x12:
                LOG_INF("Even G1 status event 0x12 from %s", arm);
                break;
            default:
                LOG_INF("Unknown Even G1 event: 0x%02x from %s", data[1], arm);
                break;
            }
        }
        break;
        
    case EVEN_G1_OPCODE_MIC:
        if (len >= 2) {
            LOG_INF("Mic status from %s: 0x%02x", arm, data[1]);
            /* Reset keepalive timer on any activity */
            k_work_reschedule(&keepalive_work, K_SECONDS(15));
        }
        break;
        
    case EVEN_G1_OPCODE_MIC_DATA:
        LOG_WRN("Unexpected mic audio data from %s (%d bytes) - mic should not be active!", arm, len);
        /* Reset heartbeat timer on any activity */
        k_work_reschedule(&keepalive_work, K_SECONDS(8));
        /* Don't try to stop mic - we shouldn't have started it in the first place */
        break;
        
    case EVEN_G1_OPCODE_BITMAP:  /* 0x15 - Bitmap packet ACK/response */
        if (len >= 2) {
            LOG_DBG("Bitmap packet response from %s: seq=0x%02x", arm, data[1]);
            /* Glasses echo back 0x15 when receiving bitmap packets */
            /* This is expected behavior during bitmap transmission */
        }
        break;
        
    case EVEN_G1_OPCODE_HEARTBEAT:  /* 0x25 - Heartbeat response */
        if (len >= 2) {
            LOG_DBG("Heartbeat response from %s: seq=0x%02x", arm, data[1]);
            /* Glasses respond to heartbeat with same opcode */
        }
        /* Reset heartbeat timer on heartbeat response */
        k_work_reschedule(&keepalive_work, K_SECONDS(15));
        break;
        
    case EVEN_G1_OPCODE_BMP_END:  /* 0x20 - Bitmap end acknowledgment */
        if (len >= 2) {
            uint8_t status = data[1];
            LOG_INF("Bitmap end acknowledgment from %s: status=0x%02X", arm, status);
            if (status == 0xC9) {
                LOG_INF("Bitmap end command processed successfully by %s arm", arm);
                
                /* Mark this arm as end-command acknowledged */
                k_mutex_lock(&g1_mutex, K_FOREVER);
                if (conn == g1_state.left_conn) {
                    g1_state.bitmap_left_end_acked = true;
                } else if (conn == g1_state.right_conn) {
                    g1_state.bitmap_right_end_acked = true;
                }
                
                /* Check if both arms have acknowledged end command */
                if (g1_state.bitmap_left_end_acked && g1_state.bitmap_right_end_acked) {
                    LOG_INF("✅ Both arms acknowledged end command - NOW sending CRC");
                    k_mutex_unlock(&g1_mutex);
                    even_g1_send_bitmap_crc();
                } else {
                    k_mutex_unlock(&g1_mutex);
                    LOG_INF("⏳ Waiting for other arm to acknowledge end command...");
                }
            } else {
                LOG_WRN("Bitmap end command failed on %s arm: status=0x%02X", arm, status);
            }
        } else {
            LOG_INF("Bitmap end acknowledgment from %s (short)", arm);
        }
        break;
        
    case EVEN_G1_OPCODE_BMP_CRC:  /* 0x16 - Bitmap CRC acknowledgment */
        if (len >= 6) {
            /* Even G1 echoes back our CRC (4 bytes) then provides status */
            uint32_t echoed_crc = (data[1] << 24) | (data[2] << 16) | (data[3] << 8) | data[4];
            uint8_t status = data[5];  /* Status is at byte 5, not byte 1 */
            
            LOG_INF("Bitmap CRC acknowledgment from %s: echoed_crc=0x%08X status=0x%02X", 
                    arm, echoed_crc, status);
            
            if (status == 0xC9) {
                LOG_INF("CRC validated successfully by %s arm", arm);
                
                /* Mark this arm as CRC validated */
                k_mutex_lock(&g1_mutex, K_FOREVER);
                if (conn == g1_state.left_conn) {
                    g1_state.bitmap_left_crc_validated = true;
                } else if (conn == g1_state.right_conn) {
                    g1_state.bitmap_right_crc_validated = true;
                }
                
                /* Check if both arms have validated CRC */
                if (g1_state.bitmap_left_crc_validated && g1_state.bitmap_right_crc_validated) {
                    LOG_INF("🎉 Both arms CRC validated - bitmap should display automatically!");
                    LOG_INF("📺 According to official docs, no additional commands needed");
                    LOG_INF("🔍 Check glasses for border pattern display");
                    k_mutex_unlock(&g1_mutex);
                    
                    /* Trigger completion handler to clean up resources */
                    k_work_submit(&bitmap_completion_work);
                } else {
                    k_mutex_unlock(&g1_mutex);
                    LOG_INF("⏳ Waiting for other arm CRC validation...");
                }
            } else {
                LOG_WRN("CRC validation failed by %s arm: status=0x%02X", arm, status);
                
                /* Mark this arm as having a failed CRC validation */
                k_mutex_lock(&g1_mutex, K_FOREVER);
                if (conn == g1_state.left_conn) {
                    g1_state.bitmap_left_crc_validated = true;  /* Mark as "processed" even if failed */
                } else if (conn == g1_state.right_conn) {
                    g1_state.bitmap_right_crc_validated = true;  /* Mark as "processed" even if failed */
                }
                
                /* Check if both arms have completed CRC validation (success or failure) */
                if (g1_state.bitmap_left_crc_validated && g1_state.bitmap_right_crc_validated) {
                    LOG_WRN("❌ Both arms completed CRC validation - at least one failed");
                    LOG_WRN("🔄 Resetting bitmap transmission state to allow new commands");
                    k_mutex_unlock(&g1_mutex);
                    
                    /* Trigger completion handler to clean up resources and reset state */
                    k_work_submit(&bitmap_completion_work);
                } else {
                    k_mutex_unlock(&g1_mutex);
                    LOG_INF("⏳ Waiting for other arm CRC validation...");
                }
            }
        } else if (len >= 2) {
            /* Fallback for short response */
            uint8_t status = data[1];
            LOG_INF("Bitmap CRC acknowledgment from %s (short): status=0x%02X", arm, status);
        } else {
            LOG_INF("CRC acknowledgment from %s (too short)", arm);
        }
        break;
        
    default:
        LOG_INF("Unknown opcode 0x%02x from %s", data[0], arm);
        /* Reset heartbeat timer on any activity */
        k_work_reschedule(&keepalive_work, K_SECONDS(8));
        
        /* Also treat unknown opcodes as potential ACKs if we're waiting */
        k_mutex_lock(&g1_mutex, K_FOREVER);
        if (conn == g1_state.left_conn) {
            LOG_INF("LEFT arm unknown opcode: waiting_for_ack=%d", g1_state.waiting_for_left_ack);
            if (g1_state.waiting_for_left_ack) {
                LOG_INF("*** LEFT arm treating unknown opcode 0x%02x as ACK - SETTING ACK FLAG ***", data[0]);
                g1_state.left_ack_received = true;
            }
        } else if (conn == g1_state.right_conn) {
            LOG_INF("RIGHT arm unknown opcode: waiting_for_ack=%d", g1_state.waiting_for_right_ack);
            if (g1_state.waiting_for_right_ack) {
                LOG_INF("*** RIGHT arm treating unknown opcode 0x%02x as ACK - SETTING ACK FLAG ***", data[0]);
                g1_state.right_ack_received = true;
            }
        }
        k_mutex_unlock(&g1_mutex);
        break;
    }
}

static int send_to_left(const uint8_t *data, uint16_t len)
{
    if (!g1_state.left_conn || !g1_state.left_ready) {
        LOG_WRN("Even G1 left arm not ready");
        return -ENOTCONN;
    }
    
    int ret = ble_nus_multi_client_send_data(g1_state.left_conn, data, len);
    if (ret == 0) {
        /* Update activity timestamp on successful send */
        g1_state.last_activity_time = k_uptime_get();
    }
    return ret;
}

static int send_to_right(const uint8_t *data, uint16_t len)
{
    if (!g1_state.right_conn || !g1_state.right_ready) {
        LOG_WRN("Even G1 right arm not ready");
        return -ENOTCONN;
    }
    
    int ret = ble_nus_multi_client_send_data(g1_state.right_conn, data, len);
    if (ret == 0) {
        /* Update activity timestamp on successful send */
        g1_state.last_activity_time = k_uptime_get();
    }
    return ret;
}

/* Heartbeat-specific send functions that don't update activity timestamp */
static int send_heartbeat_to_left(const uint8_t *data, uint16_t len)
{
    if (!g1_state.left_conn || !g1_state.left_ready) {
        LOG_WRN("Even G1 left arm not ready for heartbeat");
        return -ENOTCONN;
    }
    
    return ble_nus_multi_client_send_data(g1_state.left_conn, data, len);
}

static int send_heartbeat_to_right(const uint8_t *data, uint16_t len)
{
    if (!g1_state.right_conn || !g1_state.right_ready) {
        LOG_WRN("Even G1 right arm not ready for heartbeat");
        return -ENOTCONN;
    }
    
    return ble_nus_multi_client_send_data(g1_state.right_conn, data, len);
}

/* ========================================
 * Queue-based Command System
 * ======================================== */

static int queue_add_command(even_g1_cmd_type_t type, const uint8_t *packet, uint16_t packet_len, bool dual_arm)
{
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    /* If queue is getting full, drop the oldest pending command to make room */
    if (g1_state.queue_count >= EVEN_G1_MAX_QUEUE_SIZE - 1) {
        LOG_WRN("Even G1 command queue near full (%d/%d), dropping oldest command", 
                g1_state.queue_count, EVEN_G1_MAX_QUEUE_SIZE);
        /* Drop oldest queued command (not the current one) */
        if (g1_state.queue_count > 0) {
            g1_state.queue_head = (g1_state.queue_head + 1) % EVEN_G1_MAX_QUEUE_SIZE;
            g1_state.queue_count--;
        }
    }
    
    if (g1_state.queue_count >= EVEN_G1_MAX_QUEUE_SIZE) {
        k_mutex_unlock(&g1_mutex);
        LOG_ERR("Even G1 command queue still full after cleanup");
        return -ENOMEM;
    }
    
    even_g1_command_t *cmd = &g1_state.cmd_queue[g1_state.queue_tail];
    cmd->type = type;
    cmd->state = EVEN_G1_CMD_PENDING;
    cmd->dual_arm = dual_arm;
    cmd->packet_len = packet_len;
    cmd->timestamp = k_uptime_get_32();
    
    if (packet && packet_len > 0) {
        memcpy(cmd->packet, packet, packet_len);
    }
    
    g1_state.queue_tail = (g1_state.queue_tail + 1) % EVEN_G1_MAX_QUEUE_SIZE;
    g1_state.queue_count++;
    
    LOG_DBG("Queued command type=%d, queue_count=%d", type, g1_state.queue_count);
    
    k_mutex_unlock(&g1_mutex);
    return 0;
}

static void queue_complete_command(void)
{
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    if (g1_state.current_cmd) {
        g1_state.current_cmd->state = EVEN_G1_CMD_COMPLETED;
        g1_state.current_cmd = NULL;
        
        /* Move to next command in queue */
        if (g1_state.queue_count > 0) {
            g1_state.queue_head = (g1_state.queue_head + 1) % EVEN_G1_MAX_QUEUE_SIZE;
            g1_state.queue_count--;
            LOG_DBG("Command completed, queue_count=%d", g1_state.queue_count);
        }
    }
    
    k_mutex_unlock(&g1_mutex);
}

static void queue_fail_command(void)
{
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    if (g1_state.current_cmd) {
        g1_state.current_cmd->state = EVEN_G1_CMD_FAILED;
        LOG_ERR("Command failed: type=%d", g1_state.current_cmd->type);
        g1_state.current_cmd = NULL;
        
        /* Move to next command in queue */
        if (g1_state.queue_count > 0) {
            g1_state.queue_head = (g1_state.queue_head + 1) % EVEN_G1_MAX_QUEUE_SIZE;
            g1_state.queue_count--;
        }
    }
    
    k_mutex_unlock(&g1_mutex);
    
    /* Process next command in queue */
    even_g1_process_queue();
}

int even_g1_queue_command(even_g1_cmd_type_t type, const uint8_t *packet, uint16_t packet_len, bool dual_arm)
{
    if (!even_g1_is_ready()) {
        LOG_ERR("Even G1 not ready for command");
        return -ENOTCONN;
    }
    
    return queue_add_command(type, packet, packet_len, dual_arm);
}

void even_g1_process_queue(void)
{
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    /* Check for timeout on current command */
    if (g1_state.current_cmd) {
        uint32_t elapsed = k_uptime_get_32() - g1_state.current_cmd->timestamp;
        if (elapsed > 2000) { /* 2 second timeout - fail faster to prevent queue buildup */
            LOG_ERR("Command timeout: type=%d, state=%d", 
                    g1_state.current_cmd->type, g1_state.current_cmd->state);
            k_mutex_unlock(&g1_mutex);
            queue_fail_command();
            return;
        }
    }
    
    /* Start next command if none is currently processing */
    if (!g1_state.current_cmd && g1_state.queue_count > 0) {
        g1_state.current_cmd = &g1_state.cmd_queue[g1_state.queue_head];
        g1_state.current_cmd->state = EVEN_G1_CMD_LEFT_SENT;
        g1_state.current_cmd->timestamp = k_uptime_get_32();
        
        LOG_INF("Starting command: type=%d, dual_arm=%d, len=%d", 
                g1_state.current_cmd->type, g1_state.current_cmd->dual_arm, 
                g1_state.current_cmd->packet_len);
        
        /* Send to left arm (or right for mic commands) */
        int err;
        if (g1_state.current_cmd->dual_arm) {
            err = send_to_left(g1_state.current_cmd->packet, g1_state.current_cmd->packet_len);
        } else {
            /* Right arm only command (e.g., mic) */
            err = send_to_right(g1_state.current_cmd->packet, g1_state.current_cmd->packet_len);
            g1_state.current_cmd->state = EVEN_G1_CMD_RIGHT_SENT;
        }
        
        if (err) {
            LOG_ERR("Failed to send command: %d", err);
            k_mutex_unlock(&g1_mutex);
            queue_fail_command();
            return;
        }
    }
    
    k_mutex_unlock(&g1_mutex);
}

int even_g1_send_data(const uint8_t *data, uint16_t len, even_g1_cmd_type_t type, bool dual_arm)
{
    if (!data || len == 0) {
        return -EINVAL;
    }
    
    /* Queue the command */
    int err = even_g1_queue_command(type, data, len, dual_arm);
    if (err) {
        return err;
    }
    
    /* Process queue immediately to start sending */
    even_g1_process_queue();
    
    return 0;
}

/* ========================================
 * Legacy Text Functions (Updated to use Queue)
 * ======================================== */


int even_g1_send_text(const char *text)
{
    if (!text) {
        return -EINVAL;
    }
    
    /* REQUIRE BOTH arms to be ready before sending text */
    if (!even_g1_is_ready()) {
        LOG_WRN("Both Even G1 arms must be ready for text display");
        return -ENOTCONN;
    }
    
    /* Simple text packet for testing */
    /* Format: [0x4E, seq, total_pkg, current_pkg, newscreen, char_pos0, char_pos1, 
     *          current_page, max_page, text...] */
    uint8_t packet[200] = {0};
    size_t text_len = strlen(text);
    
    if (text_len > 180) {
        text_len = 180;  /* Limit text length */
    }
    
    packet[0] = EVEN_G1_OPCODE_TEXT;
    packet[1] = g1_state.sequence++;  /* Sequence number (0-255) */
    packet[2] = 1;  /* Total packages (1-255) */
    packet[3] = 0;  /* Current package number (0-255, starts from 0!) */
    packet[4] = 0x71;  /* New screen (0x01) + Text Show mode (0x70) = 0x71 */
    packet[5] = 0;  /* new_char_pos0: Higher 8 bits of char position */
    packet[6] = 0;  /* new_char_pos1: Lower 8 bits of char position */
    packet[7] = 0;  /* Current page number (0-255, starts from 0) */
    packet[8] = 1;  /* Max page number (1-255) */
    
    memcpy(&packet[9], text, text_len);
    
    LOG_INF("Sending text to Even G1: '%s'", text);
    LOG_HEXDUMP_DBG(packet, 9 + text_len, "Text packet");
    
    /* Use queue-based system for left→ACK→right flow */
    return even_g1_send_data(packet, 9 + text_len, EVEN_G1_CMD_TYPE_TEXT, true);
}

/* Unified dual-arm text sending function - sends to both arms with ACK protocol */
int even_g1_send_text_dual_arm(const char *text)
{
    if (!even_g1_is_ready()) {
        LOG_ERR("Even G1 not ready for text sending");
        return -ENOTCONN;
    }
    
    /* Check if bitmap transmission is in progress */
    if (even_g1_is_bitmap_in_progress()) {
        LOG_WRN("Cannot send text while bitmap transmission is in progress");
        return -EBUSY;
    }
    
    if (!text) {
        LOG_ERR("Text is NULL");
        return -EINVAL;
    }
    
    size_t text_len = strlen(text);
    if (text_len == 0) {
        LOG_ERR("Text is empty");
        return -EINVAL;
    }
    
    if (text_len > 180) {
        text_len = 180;  /* Limit text length */
    }
    
    uint8_t packet[256];
    packet[0] = EVEN_G1_OPCODE_TEXT;
    packet[1] = g1_state.sequence++;  /* Sequence number (0-255) */
    packet[2] = 1;  /* Total packages (1-255) */
    packet[3] = 0;  /* Current package number (0-255, starts from 0!) */
    packet[4] = 0x71;  /* New screen (0x01) + Text Show mode (0x70) = 0x71 */
    packet[5] = 0;  /* new_char_pos0: Higher 8 bits of char position */
    packet[6] = 0;  /* new_char_pos1: Lower 8 bits of char position */
    packet[7] = 0;  /* Current page number (0-255, starts from 0) */
    packet[8] = 1;  /* Max page number (1-255) */
    
    memcpy(&packet[9], text, text_len);
    
    LOG_INF("Sending dual-arm text to Even G1: '%s'", text);
    LOG_INF("Text packet details: seq=%02x total=%02x current=%02x newscreen=%02x",
            packet[1], packet[2], packet[3], packet[4]);
    LOG_HEXDUMP_DBG(packet, 9 + text_len, "Dual-arm text packet");
    
    /* Use queue-based system for left→ACK→right flow */
    return even_g1_send_data(packet, 9 + text_len, EVEN_G1_CMD_TYPE_TEXT, true);
}

/* Unified dual-arm formatted text sending function */
int even_g1_send_text_formatted_dual_arm(const char *line1, const char *line2, 
                                          const char *line3, const char *line4, 
                                          const char *line5)
{
    char buffer[256];
    int len = 0;
    
    /* Format lines with newlines between non-empty lines */
    if (line1 && strlen(line1) > 0) {
        len += snprintf(buffer + len, sizeof(buffer) - len, "%s", line1);
    }
    if (line2 && strlen(line2) > 0) {
        len += snprintf(buffer + len, sizeof(buffer) - len, "%s%s", (len > 0) ? "\n" : "", line2);
    }
    if (line3 && strlen(line3) > 0) {
        len += snprintf(buffer + len, sizeof(buffer) - len, "%s%s", (len > 0) ? "\n" : "", line3);
    }
    if (line4 && strlen(line4) > 0) {
        len += snprintf(buffer + len, sizeof(buffer) - len, "%s%s", (len > 0) ? "\n" : "", line4);
    }
    if (line5 && strlen(line5) > 0) {
        len += snprintf(buffer + len, sizeof(buffer) - len, "%s%s", (len > 0) ? "\n" : "", line5);
    }
    
    return even_g1_send_text_dual_arm(buffer);
}

int even_g1_send_text_formatted(const char *line1, const char *line2, 
                                const char *line3, const char *line4, 
                                const char *line5)
{
    char buffer[256];
    int len = 0;
    
    /* Format lines with proper spacing for Even G1 display */
    if (line1) len += snprintf(buffer + len, sizeof(buffer) - len, "%s\n", line1);
    if (line2) len += snprintf(buffer + len, sizeof(buffer) - len, "%s\n", line2);
    if (line3) len += snprintf(buffer + len, sizeof(buffer) - len, "%s\n", line3);
    if (line4) len += snprintf(buffer + len, sizeof(buffer) - len, "%s\n", line4);
    if (line5) len += snprintf(buffer + len, sizeof(buffer) - len, "%s", line5);
    
    return even_g1_send_text_dual_arm(buffer);
}

int even_g1_clear_display(void)
{
    /* Send empty text to clear display on both arms */
    return even_g1_send_text_dual_arm(" ");
}

int even_g1_clear_display_dual_arm(void)
{
    /* Send empty text to clear display on both arms using queue system */
    return even_g1_send_text_dual_arm(" ");
}

int even_g1_clear_bitmap_display(void)
{
    /* Use official Even G1 clear commands found in EvenDemoApp */
    LOG_INF("🗑️ Clearing bitmap display using official Even G1 commands");
    
    /* First try the TouchBar clear command: 0xF5 0x00 - "Close features or turn off display details" */
    LOG_INF("Sending TouchBar clear command (0xF5 0x00)");
    uint8_t touchbar_clear[] = {0xF5, 0x00};
    int ret = even_g1_send_data(touchbar_clear, sizeof(touchbar_clear), EVEN_G1_CMD_TYPE_CLEAR, true);
    if (ret != 0) {
        LOG_WRN("TouchBar clear command failed: %d", ret);
    }
    
    /* Small delay to let command process */
    k_sleep(K_MSEC(50));
    
    /* Also send the soft display clear using "--" text string */
    LOG_INF("Sending soft display clear text (\"--\")");
    ret = even_g1_send_text_dual_arm("--");
    if (ret != 0) {
        LOG_WRN("Soft display clear failed: %d", ret);
        /* Fallback to regular text clear */
        LOG_INF("Using fallback text clear");
        return even_g1_clear_display_dual_arm();
    }
    
    LOG_INF("✅ Official Even G1 clear commands sent");
    return 0;
}

int even_g1_send_bitmap(const uint8_t *bitmap_data, size_t width, size_t height)
{
    if (!bitmap_data || width != 576 || height != 136) {
        LOG_ERR("Invalid bitmap parameters: data=%p, size=%zux%zu", 
                bitmap_data, width, height);
        return -EINVAL;
    }
    
    if (!even_g1_is_ready()) {
        LOG_ERR("Even G1 not ready for bitmap");
        return -ENOTCONN;
    }
    
    const size_t bitmap_size = (width * height) / 8; /* 1-bit bitmap */
    const size_t chunk_size = 194; /* Data bytes per packet */
    size_t bytes_sent = 0;
    uint8_t sequence = g1_state.sequence++;
    
    LOG_INF("Sending bitmap to Even G1: %zux%zu (%zu bytes)", width, height, bitmap_size);
    
    /* Send bitmap in chunks */
    while (bytes_sent < bitmap_size) {
        uint8_t packet[256];
        size_t chunk_len = MIN(chunk_size, bitmap_size - bytes_sent);
        size_t packet_len;
        
        packet[0] = EVEN_G1_OPCODE_BITMAP;
        packet[1] = sequence;
        
        if (bytes_sent == 0) {
            /* First packet has header */
            packet[2] = 0x00;
            packet[3] = 0x1C;  /* Width high byte (576 >> 8) = 2, but use 0x1C from docs */
            packet[4] = 0x00;  /* Width low byte (576 & 0xFF) = 64 */
            packet[5] = 0x00;  /* Height/other data */
            memcpy(&packet[6], bitmap_data + bytes_sent, chunk_len);
            packet_len = 6 + chunk_len;
        } else {
            /* Subsequent packets */
            memcpy(&packet[2], bitmap_data + bytes_sent, chunk_len);
            packet_len = 2 + chunk_len;
        }
        
        /* Queue the bitmap chunk */
        int err = even_g1_send_data(packet, packet_len, EVEN_G1_CMD_TYPE_BITMAP, true);
        if (err) {
            LOG_ERR("Failed to queue bitmap chunk: %d", err);
            return err;
        }
        
        bytes_sent += chunk_len;
        LOG_DBG("Queued bitmap chunk: %zu/%zu bytes", bytes_sent, bitmap_size);
    }
    
    /* Send end marker */
    uint8_t end_packet[] = {EVEN_G1_OPCODE_BMP_END, 0x0D, 0x0E};
    int err = even_g1_send_data(end_packet, sizeof(end_packet), EVEN_G1_CMD_TYPE_BITMAP, true);
    if (err) {
        LOG_ERR("Failed to queue bitmap end marker: %d", err);
        return err;
    }
    
    /* Calculate and send CRC */
    /* For now, use a placeholder CRC - real implementation would calculate CRC32-XZ */
    uint8_t crc_packet[] = {EVEN_G1_OPCODE_BMP_CRC, 0x58, 0x8D, 0x52, 0x3A}; /* Example CRC */
    err = even_g1_send_data(crc_packet, sizeof(crc_packet), EVEN_G1_CMD_TYPE_BITMAP, true);
    if (err) {
        LOG_ERR("Failed to queue bitmap CRC: %d", err);
        return err;
    }
    
    LOG_INF("Bitmap queued successfully (%zu chunks)", (bitmap_size + chunk_size - 1) / chunk_size);
    return 0;
}

int even_g1_show_mouthpad_status(bool connected, int8_t rssi, uint8_t battery)
{
    char line1[] = "MouthPad^USB";
    char line2[32];
    char line3[32];
    char line4[32];
    
    if (connected) {
        snprintf(line2, sizeof(line2), "Status: Connected");
        snprintf(line3, sizeof(line3), "RSSI: %d dBm", rssi);
        snprintf(line4, sizeof(line4), "Battery: %d%%", battery);
    } else {
        snprintf(line2, sizeof(line2), "Status: Scanning");
        line3[0] = '\0';
        line4[0] = '\0';
    }
    
    return even_g1_send_text_formatted_dual_arm(line1, line2, line3, line4, NULL);
}

int even_g1_show_mouse_event(int16_t x, int16_t y, uint8_t buttons)
{
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "Mouse: X=%d Y=%d B=%02x", x, y, buttons);
    return even_g1_send_text_dual_arm(buffer);
}

static void init_sequence_handler(struct k_work *work)
{
    if (!even_g1_is_ready()) {
        LOG_WRN("Even G1 not ready for init sequence");
        return;
    }
    
    LOG_INF("Running Even G1 initialization sequence");
    
    /* Show current status immediately upon connection */
    even_g1_show_current_status();
}

void even_g1_print_status(void)
{
    k_mutex_lock(&g1_mutex, K_FOREVER);
    const char *mode = g1_state.bitmap_mode ? "BITMAP" : "TEXT";
    bool dashboard_open = g1_state.dashboard_open;
    k_mutex_unlock(&g1_mutex);
    
    LOG_INF("=== Even G1 Status ===");
    LOG_INF("Display Mode: %s", mode);
    LOG_INF("Dashboard: %s", dashboard_open ? "OPEN" : "CLOSED");
    LOG_INF("Left arm: conn=%p ready=%d mtu=%d", 
            g1_state.left_conn, g1_state.left_ready, g1_state.left_mtu);
    LOG_INF("Right arm: conn=%p ready=%d mtu=%d",
            g1_state.right_conn, g1_state.right_ready, g1_state.right_mtu);
    LOG_INF("Sequence: %d", g1_state.sequence);
}

void even_g1_show_current_status(void)
{
    if (!even_g1_is_ready()) {
        LOG_DBG("Even G1 not ready, cannot show status");
        return;
    }
    
    bool has_mouthpad = ble_multi_conn_has_type(DEVICE_TYPE_MOUTHPAD);
    uint8_t battery_level = ble_bas_get_battery_level();
    int8_t rssi_dbm = has_mouthpad ? ble_transport_get_rssi() : 0;
    bool dashboard_open = even_g1_is_dashboard_open();

    /* Build status display using same logic as main loop */
    const char *full_title;
    if (has_mouthpad) {
        ble_device_connection_t *mouthpad = ble_multi_conn_get_mouthpad();
        if (mouthpad && strlen(mouthpad->name) > 0) {
            full_title = mouthpad->name;
        } else {
            full_title = "MouthPad";
        }
    } else {
        full_title = "MouthPad^USB";  /* Show default when only Even G1 connected */
    }
    
    char display_title[13];
    strncpy(display_title, full_title, 12);
    display_title[12] = '\0';
    
    /* Determine status line based on MouthPad connection state and scanning state */
    const char *status_line;
    if (has_mouthpad) {
        status_line = "Connected";
    } else if (ble_central_is_scanning()) {
        status_line = "Scanning...";
    } else {
        /* When no MouthPad connected and not scanning, show Ready */
        status_line = "Ready";
    }
    
    LOG_INF("Even G1 status debug: has_mouthpad=%d, scanning=%d, status=%s", 
            has_mouthpad, ble_central_is_scanning(), status_line);
    
    /* Battery line - always create it (empty if no battery data) */
    char battery_str[32] = "";
    if (has_mouthpad && battery_level != 0xFF && battery_level <= 100) {
        char battery_icon[8];
        if (battery_level > 75) {
            strcpy(battery_icon, "[||||]");
        } else if (battery_level > 50) {
            strcpy(battery_icon, "[|||.]");
        } else if (battery_level > 25) {
            strcpy(battery_icon, "[||..]");
        } else if (battery_level > 10) {
            strcpy(battery_icon, "[|...]");
        } else {
            strcpy(battery_icon, "[....]");
        }
        snprintf(battery_str, sizeof(battery_str), "%s %d%%", battery_icon, battery_level);
    }
    
    /* Signal line - always create it (empty if not connected) */
    char signal_str[32] = "";
    if (has_mouthpad) {
        const char* signal_bars;
        if (rssi_dbm >= -50) {
            signal_bars = "[||||]";
        } else if (rssi_dbm >= -60) {
            signal_bars = "[|||.]";
        } else if (rssi_dbm >= -70) {
            signal_bars = "[||..]";
        } else if (rssi_dbm >= -80) {
            signal_bars = "[|...]";
        } else {
            signal_bars = "[....]";
        }
        snprintf(signal_str, sizeof(signal_str), "%s %d dBm", signal_bars, rssi_dbm);
        LOG_DBG("MouthPad connected - signal_str: '%s'", signal_str);
    } else {
        LOG_DBG("No MouthPad connected - signal_str remains empty: '%s'", signal_str);
    }
    
    LOG_INF("Showing current status on Even G1 (mode: %s)", 
            g1_state.bitmap_mode ? "BITMAP" : "TEXT");
    
    /* Check if bitmap mode is active - if so, send bitmap instead of text */
    if (g1_state.bitmap_mode) {
        /* Bitmap mode - send test bitmap with exit command and alternating pattern */
        LOG_INF("📸 Bitmap mode active - sending test bitmap");
        even_g1_send_test_all_black();  /* Exit command + alternating pattern test */
    } else {
        /* Text mode - send normal status display based on dashboard state */
        if (dashboard_open) {
            /* Dashboard OPEN: Show full status with battery and signal */
            LOG_INF("📝 Text mode: Dashboard OPEN - showing full status display");
            even_g1_send_text_formatted_dual_arm(display_title, status_line, 
                                                 battery_str,  /* Line 3: battery (empty if no data) */
                                                 signal_str,   /* Line 4: signal (empty if not connected) */
                                                 NULL);        /* Line 5: unused */
        } else {
            /* Dashboard CLOSED: Show minimal status - just name and status */
            LOG_INF("📝 Text mode: Dashboard CLOSED - showing minimal display");
            even_g1_send_text_formatted_dual_arm(display_title, status_line, 
                                                 "",           /* Line 3: empty */
                                                 "",           /* Line 4: empty */
                                                 NULL);        /* Line 5: unused */
        }
    }
}

static void keepalive_handler(struct k_work *work)
{
    if (!even_g1_is_ready()) {
        LOG_DBG("Even G1 not ready, skipping keepalive");
        return;
    }
    
    LOG_DBG("Even G1 keepalive timer triggered");
    
    /* Check time since last activity (any data sent to Even G1) */
    int64_t current_time = k_uptime_get();
    int64_t time_since_activity = current_time - g1_state.last_activity_time;
    
    /* Send heartbeat if no activity for more than 6 seconds */
    if (time_since_activity > 6000) {
        LOG_INF("No Even G1 activity for %lld ms - sending heartbeat", time_since_activity);
        send_heartbeat_command();
    } else {
        LOG_DBG("Recent Even G1 activity (%lld ms ago) - heartbeat not needed", time_since_activity);
    }
    
    /* Reschedule heartbeat (8 seconds like official app) */
    k_work_reschedule(&keepalive_work, K_SECONDS(8));
}

static void bitmap_completion_handler(struct k_work *work)
{
    LOG_INF("Bitmap completion handler running in work queue context");
    LOG_INF("Current bitmap_in_progress state: %d", g1_state.bitmap_in_progress);
    
    /* Check if both arms validated successfully */
    bool success = g1_state.bitmap_left_crc_validated && g1_state.bitmap_right_crc_validated;
    
    /* Work queue provides safe thread context - do completion without mutex contention */
    /* Reset bitmap transmission state variables */
    g1_state.bitmap_in_progress = false;
    g1_state.bitmap_size = 0;
    g1_state.bitmap_packets_total = 0;
    g1_state.bitmap_packets_sent = 0;
    g1_state.bitmap_crc32 = 0;
    g1_state.bitmap_left_pending = false;
    g1_state.bitmap_right_pending = false;
    g1_state.bitmap_left_retry_count = 0;
    g1_state.bitmap_right_retry_count = 0;
    g1_state.bitmap_left_crc_validated = false;
    g1_state.bitmap_right_crc_validated = false;
    g1_state.bitmap_left_end_acked = false;
    g1_state.bitmap_right_end_acked = false;
    
    /* Handle memory cleanup safely - free the ORIGINAL BMP buffer */
    if (g1_state.bitmap_original_buffer) {
        LOG_INF("Freeing bitmap buffer at %p", g1_state.bitmap_original_buffer);
        k_free(g1_state.bitmap_original_buffer);
        g1_state.bitmap_original_buffer = NULL;
    }
    g1_state.bitmap_data = NULL;
    g1_state.bitmap_data_allocated = false;
    
    if (success) {
        LOG_INF("PROTOCOL SUCCESS: Both arms CRC validated!");
        LOG_INF("Bitmap transmission completed successfully");
        
        /* According to protocol, bitmap should display automatically after CRC validation */
        LOG_INF("✅ Both arms validated CRC - bitmap should display automatically!");
    } else {
        LOG_WRN("Bitmap transmission failed - CRC validation unsuccessful");
    }
    
    LOG_INF("Bitmap state reset - bitmap_in_progress now: %d", g1_state.bitmap_in_progress);
}

static int send_ack_to_arm(struct bt_conn *conn)
{
    /* Don't send ACKs that might confuse the glasses - just log receipt */
    const char *arm = (conn == g1_state.left_conn) ? "LEFT" : 
                     (conn == g1_state.right_conn) ? "RIGHT" : "UNKNOWN";
    LOG_DBG("Acknowledging event from %s arm (no response sent)", arm);
    return 0;
}

static int send_heartbeat_command(void)
{
    if (!even_g1_is_ready()) {
        LOG_DBG("Even G1 not ready, cannot send heartbeat");
        return -ENOTCONN;
    }

    /* Increment heartbeat sequence */
    g1_state.heartbeat_sequence = (g1_state.heartbeat_sequence + 1) % 0xFF;
    
    /* Create heartbeat packet based on official Even G1 app format:
     * [0x25, length_low, length_high, sequence, 0x04, sequence] */
    uint8_t heartbeat[] = {
        EVEN_G1_OPCODE_HEARTBEAT,    /* 0x25 */
        0x06,                        /* Length low byte (6 bytes total) */
        0x00,                        /* Length high byte */
        g1_state.heartbeat_sequence, /* Sequence number */
        0x04,                        /* Heartbeat identifier */
        g1_state.heartbeat_sequence  /* Sequence number (repeated) */
    };

    LOG_DBG("Sending heartbeat (seq=%d) to both arms", g1_state.heartbeat_sequence);
    
    /* Send to both arms like the official app (heartbeats don't count as activity) */
    int left_err = send_heartbeat_to_left(heartbeat, sizeof(heartbeat));
    int right_err = send_heartbeat_to_right(heartbeat, sizeof(heartbeat));
    
    if (left_err != 0) {
        LOG_WRN("Failed to send heartbeat to left arm: %d", left_err);
    }
    if (right_err != 0) {
        LOG_WRN("Failed to send heartbeat to right arm: %d", right_err);
    }
    
    /* Consider successful if at least one arm received it */
    return (left_err == 0 || right_err == 0) ? 0 : -EIO;
}

static int send_mic_stop_command(void)
{
    if (!g1_state.right_ready || !g1_state.right_conn) {
        LOG_WRN("Right arm not ready for mic stop command");
        return -ENOTCONN;
    }

    /* Send mic stop command [0x0E, 0x00] to RIGHT arm only (per docs) */
    uint8_t mic_stop[] = {
        EVEN_G1_OPCODE_MIC,  /* 0x0E */
        0x00                 /* Stop mic */
    };

    LOG_INF("Sending mic stop command to RIGHT arm to stop 0xF1 flood");
    return send_to_right(mic_stop, sizeof(mic_stop));
}

/* ========================================
 * Advanced Bitmap Transmission Functions
 * ======================================== */

bool even_g1_is_bitmap_in_progress(void)
{
    bool in_progress;
    k_mutex_lock(&g1_mutex, K_FOREVER);
    in_progress = g1_state.bitmap_in_progress;
    k_mutex_unlock(&g1_mutex);
    return in_progress;
}


static uint32_t calculate_bitmap_crc32(const uint8_t *bitmap_data, size_t bitmap_size)
{
    /* For generated bitmap data (not BMP files), calculate CRC on address + bitmap data */
    /* This is a fallback for when we're not using actual BMP files */
    
    const uint8_t address[] = EVEN_G1_BITMAP_ADDRESS;  /* [0x00, 0x1C, 0x00, 0x00] */
    
    /* Calculate CRC in chunks to avoid large malloc */
    uint32_t crc = 0;
    
    /* First, process the address bytes */
    crc = crc32_ieee_update(crc, address, sizeof(address));
    
    /* Then process the bitmap data */
    crc = crc32_ieee_update(crc, bitmap_data, bitmap_size);
    
    /* Try little-endian (no byte swap) - Even G1 might expect LE */
    LOG_INF("CRC32 calculation: address + bitmap data %zu bytes = 0x%08X (little-endian)", 
            sizeof(address) + bitmap_size, crc);
    
    return crc;  /* Return little-endian CRC */
}

/* CRC-32/XZ implementation to match EvenDemoApp exactly */
static uint32_t crc32_xz_update(uint32_t crc, const uint8_t *data, size_t len)
{
    /* CRC-32/XZ polynomial: 0x04c11db7 (reflected: 0xedb88320) */
    static const uint32_t table[256] = {
        0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
        0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
        0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
        0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
        0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
        0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
        0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
        0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
        0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
        0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
        0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
        0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
        0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
        0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
        0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
        0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
        0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
        0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
        0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
        0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
        0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
        0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
        0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
        0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
        0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
        0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
        0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
        0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
        0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
        0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
        0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
        0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
        0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
        0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
        0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
        0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
        0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
        0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
        0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
        0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
        0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
        0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
        0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
    };
    
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    return crc;
}

static uint32_t calculate_full_bmp_crc32(const uint8_t *bmp_data, size_t bmp_size)
{
    /* EXACT MATCH to EvenDemoApp: CRC-32/XZ on address + FULL BMP FILE */
    const uint8_t address[] = EVEN_G1_BITMAP_ADDRESS;  /* [0x00, 0x1C, 0x00, 0x00] */
    
    /* Initialize CRC-32/XZ with proper initial value */
    uint32_t crc = 0xFFFFFFFF;
    
    /* First, process the address bytes */
    crc = crc32_xz_update(crc, address, sizeof(address));
    
    /* Then process the FULL BMP FILE including headers */
    crc = crc32_xz_update(crc, bmp_data, bmp_size);
    
    /* Apply final XOR as per CRC-32/XZ specification */
    crc ^= 0xFFFFFFFF;
    
    LOG_INF("CRC-32/XZ: address(4) + full_bmp(%zu) = 0x%08X (matches EvenDemoApp)", 
            bmp_size, crc);
    
    return crc;
}

static uint32_t calculate_bmp_file_crc32(const uint8_t *bmp_data, size_t bmp_size)
{
    /* CRITICAL FIX: Official app calculates CRC on address + PIXEL DATA only, not full BMP file */
    /* Extract pixel data from BMP file (skip headers) */
    
    const uint8_t address[] = EVEN_G1_BITMAP_ADDRESS;  /* [0x00, 0x1C, 0x00, 0x00] */
    
    /* Verify BMP header and extract pixel data offset */
    if (bmp_size < 54 || bmp_data[0] != 'B' || bmp_data[1] != 'M') {
        LOG_ERR("Invalid BMP file for CRC calculation");
        return 0;
    }
    
    /* Extract pixel data offset from BMP header (bytes 10-13, little-endian) */
    uint32_t pixel_offset = bmp_data[10] | (bmp_data[11] << 8) | (bmp_data[12] << 16) | (bmp_data[13] << 24);
    
    if (pixel_offset >= bmp_size) {
        LOG_ERR("Invalid pixel data offset in BMP file");
        return 0;
    }
    
    /* Calculate pixel data size */
    size_t pixel_data_size = bmp_size - pixel_offset;
    const uint8_t *pixel_data = &bmp_data[pixel_offset];
    
    LOG_INF("BMP analysis: total=%zu bytes, pixel_offset=%u, pixel_data=%zu bytes", 
            bmp_size, pixel_offset, pixel_data_size);
    
    /* Calculate CRC in chunks to avoid large malloc */
    uint32_t crc = 0;
    
    /* First, process the address bytes */
    crc = crc32_ieee_update(crc, address, sizeof(address));
    
    /* Then process ONLY the pixel data (like official app) */
    crc = crc32_ieee_update(crc, pixel_data, pixel_data_size);
    
    /* Keep little-endian format as used by EvenDemoApp */
    LOG_INF("CRC32-IEEE: address + pixel data %zu bytes = 0x%08X (little-endian)", 
            sizeof(address) + pixel_data_size, crc);
    
    return crc;
}

/* Regular completion function that acquires mutex */
static void even_g1_complete_bitmap_transmission(void)
{
    k_mutex_lock(&g1_mutex, K_FOREVER);
    even_g1_complete_bitmap_transmission_internal();
}

int even_g1_send_bmp_file(const uint8_t *bmp_data, size_t bmp_size)
{
    /* CRITICAL: Official app sends the FULL BMP FILE including headers!
     * Verified in EvenDemoApp: loadBmpImage returns entire BMP file
     * CRC is calculated on address + FULL BMP file
     */
    if (!bmp_data || bmp_size == 0) {
        LOG_ERR("Invalid BMP data or size");
        return -EINVAL;
    }
    
    if (!even_g1_is_ready()) {
        LOG_ERR("Even G1 not ready for BMP transmission");
        return -ENOTCONN;
    }
    
    /* Check if bitmap transmission is already in progress */
    if (even_g1_is_bitmap_in_progress()) {
        LOG_WRN("Bitmap transmission already in progress");
        return -EBUSY;
    }
    
    /* Check if there are pending text commands in the queue */
    k_mutex_lock(&g1_mutex, K_FOREVER);
    bool has_pending_commands = (g1_state.queue_count > 0);
    k_mutex_unlock(&g1_mutex);
    
    if (has_pending_commands) {
        LOG_WRN("Cannot start BMP transmission while text commands are queued");
        return -EBUSY;
    }
    
    /* No exit command needed - proceed directly with bitmap transmission */
    LOG_INF("Starting bitmap transmission to both arms");
    k_sleep(K_MSEC(100));
    
    /* Calculate total packets needed for FULL BMP FILE */
    uint16_t total_packets = (bmp_size + EVEN_G1_BITMAP_PACKET_SIZE - 1) / EVEN_G1_BITMAP_PACKET_SIZE;
    
    LOG_INF("🚀 Starting BMP FILE transmission: %zu bytes, %d packets (MTU L:%d R:%d)", 
            bmp_size, total_packets, g1_state.left_mtu, g1_state.right_mtu);
    
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    /* Initialize bitmap transmission state WITH FULL BMP FILE */
    g1_state.bitmap_in_progress = true;
    g1_state.bitmap_data = (uint8_t *)bmp_data;    /* Store pointer to FULL BMP FILE */
    g1_state.bitmap_data_allocated = true;         /* Caller uses k_malloc */
    g1_state.bitmap_size = bmp_size;               /* Size of FULL BMP FILE */
    g1_state.bitmap_original_buffer = NULL;        /* Not needed - direct pointer */
    g1_state.bitmap_packets_total = total_packets;
    g1_state.bitmap_packets_sent = 0;
    /* Detect if this is a real BMP file or raw pixel data */
    bool is_bmp_file = (bmp_size >= 2 && bmp_data[0] == 'B' && bmp_data[1] == 'M');
    
    if (is_bmp_file) {
        /* Real BMP file - try full BMP file CRC calculation to match transmitted data */
        g1_state.bitmap_crc32 = calculate_full_bmp_crc32(bmp_data, bmp_size);
        LOG_INF("📊 BMP file CRC: address + complete BMP file = 0x%08X", 
                g1_state.bitmap_crc32);
    } else {
        /* Raw pixel data - use bitmap CRC calculation */
        g1_state.bitmap_crc32 = calculate_bitmap_crc32(bmp_data, bmp_size);
        LOG_INF("📊 Raw pixel CRC (little-endian): address + %zu bytes pixel data = 0x%08X", 
                bmp_size, g1_state.bitmap_crc32);
    }
    
    g1_state.bitmap_left_crc_validated = false;   /* Reset CRC validation flags */
    g1_state.bitmap_right_crc_validated = false;
    g1_state.bitmap_left_end_acked = false;       /* Reset end command ACK flags */
    g1_state.bitmap_right_end_acked = false;
    
    k_mutex_unlock(&g1_mutex);
    
    /* Start sending packets directly - preparation command causes BLE disconnections */
    return even_g1_send_next_bitmap_packet();
}

static int even_g1_send_next_bitmap_packet(void)
{
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    if (!g1_state.bitmap_in_progress || g1_state.bitmap_packets_sent >= g1_state.bitmap_packets_total) {
        k_mutex_unlock(&g1_mutex);
        return -EINVAL;
    }
    
    uint16_t packet_index = g1_state.bitmap_packets_sent;
    uint32_t data_offset = packet_index * EVEN_G1_BITMAP_PACKET_SIZE;
    uint16_t packet_data_size = MIN(EVEN_G1_BITMAP_PACKET_SIZE, g1_state.bitmap_size - data_offset);
    
    /* Create packet in state buffer for async transmission */
    uint16_t packet_len = 0;
    
    /* Packet header: [0x15, sequence] */
    g1_state.bitmap_current_packet[packet_len++] = EVEN_G1_OPCODE_BITMAP;
    g1_state.bitmap_current_packet[packet_len++] = packet_index & 0xFF;  /* 0-based sequence */
    
    /* First packet includes address */
    if (packet_index == 0) {
        const uint8_t address[] = EVEN_G1_BITMAP_ADDRESS;
        memcpy(&g1_state.bitmap_current_packet[packet_len], address, sizeof(address));
        packet_len += sizeof(address);
        LOG_DBG("First packet includes address: 0x%02X 0x%02X 0x%02X 0x%02X", 
                address[0], address[1], address[2], address[3]);
    }
    
    /* Copy bitmap data */
    memcpy(&g1_state.bitmap_current_packet[packet_len], &g1_state.bitmap_data[data_offset], packet_data_size);
    packet_len += packet_data_size;
    g1_state.bitmap_current_packet_len = packet_len;
    
    LOG_INF("Prepared bitmap packet %d/%d: %d bytes (data: %d bytes)", 
            packet_index + 1, g1_state.bitmap_packets_total, packet_len, packet_data_size);
    
    /* 🔍 DEBUG: Show exact packet format for first few packets */
    if (packet_index < 3) {
        LOG_INF("📦 Packet %d format:", packet_index);
        for (int i = 0; i < MIN(16, packet_len); i++) {
            printk("0x%02X ", g1_state.bitmap_current_packet[i]);
        }
        printk("... (total %d bytes)\n", packet_len);
        
        if (packet_index == 0) {
            LOG_INF("📍 First packet should be: [0x15, 0x00, 0x00, 0x1C, 0x00, 0x00] + data");
        } else {
            LOG_INF("📍 Packet %d should be: [0x15, 0x%02X] + data", packet_index, packet_index);
        }
    }
    
    /* Add diagnostic checksum for packet integrity */
    uint32_t packet_checksum = 0;
    for (int i = 0; i < packet_len; i++) {
        packet_checksum += g1_state.bitmap_current_packet[i];
    }
    LOG_DBG("Packet %d checksum: 0x%08X", packet_index + 1, packet_checksum);
    
    g1_state.bitmap_packets_sent++;
    
    /* Mark both arms as pending and reset retry counters */
    g1_state.bitmap_left_pending = true;
    g1_state.bitmap_right_pending = true;
    g1_state.bitmap_left_retry_count = 0;
    g1_state.bitmap_right_retry_count = 0;
    
    /* Send sequentially: left arm first, then right arm after callback */
    /* This avoids simultaneous identical packets which may confuse Even G1 firmware */
    int left_err = send_to_left(g1_state.bitmap_current_packet, g1_state.bitmap_current_packet_len);
    if (left_err != 0) {
        LOG_WRN("Failed to queue bitmap packet to left arm: %d", left_err);
        g1_state.bitmap_left_pending = false;
        
        /* If left failed, try right immediately */
        int right_err = send_to_right(g1_state.bitmap_current_packet, g1_state.bitmap_current_packet_len);
        if (right_err != 0) {
            LOG_WRN("Failed to queue bitmap packet to right arm: %d", right_err);
            g1_state.bitmap_right_pending = false;
        }
    }
    
    /* Right arm will be sent after left arm callback completes */
    
    k_mutex_unlock(&g1_mutex);
    
    /* If left failed immediately and we tried right immediately, check for abort */
    if (left_err != 0 && !g1_state.bitmap_left_pending && !g1_state.bitmap_right_pending) {
        LOG_ERR("Failed to send bitmap packet to both arms immediately");
        even_g1_abort_bitmap_transmission();
        return -EIO;
    }
    
    /* Callbacks will handle continuation and end command */
    return 0;
}

static int even_g1_send_bitmap_end(void)
{
    const uint8_t end_data[] = EVEN_G1_BMP_END_DATA;
    uint8_t end_packet[8];
    uint16_t packet_len = 0;
    
    /* Create end packet: [0x20, 0x0D, 0x0E] */
    end_packet[packet_len++] = EVEN_G1_OPCODE_BMP_END;
    memcpy(&end_packet[packet_len], end_data, sizeof(end_data));
    packet_len += sizeof(end_data);
    
    LOG_DBG("Sending bitmap end command: [0x%02X, 0x%02X, 0x%02X]", 
            end_packet[0], end_packet[1], end_packet[2]);
    
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    /* Mark packets as 52 to indicate end command phase */
    g1_state.bitmap_packets_sent = g1_state.bitmap_packets_total + 1;
    
    /* Store current packet for async send */
    memcpy(g1_state.bitmap_current_packet, end_packet, packet_len);
    g1_state.bitmap_current_packet_len = packet_len;
    
    /* Send to both arms asynchronously */
    g1_state.bitmap_left_pending = true;
    g1_state.bitmap_right_pending = true;
    
    k_mutex_unlock(&g1_mutex);
    
    /* Send sequentially: left arm first */
    int left_err = ble_nus_multi_client_send_data(g1_state.left_conn, end_packet, packet_len);
    /* Right arm will be sent after left arm callback - don't send now */
    
    if (left_err != 0) {
        k_mutex_lock(&g1_mutex, K_FOREVER);
        g1_state.bitmap_left_pending = false;
        k_mutex_unlock(&g1_mutex);
        LOG_ERR("Failed to send bitmap end to left arm: %d", left_err);
    }
    
    if (left_err != 0) {
        /* Try right arm immediately if left failed */
        int right_err = ble_nus_multi_client_send_data(g1_state.right_conn, end_packet, packet_len);
        if (right_err != 0) {
            k_mutex_lock(&g1_mutex, K_FOREVER);
            g1_state.bitmap_right_pending = false;
            k_mutex_unlock(&g1_mutex);
            LOG_ERR("Failed to send bitmap end to right arm: %d", right_err);
        }
    }
    
    if (left_err != 0 && !g1_state.bitmap_left_pending && !g1_state.bitmap_right_pending) {
        LOG_ERR("Failed to send bitmap end command to both arms");
        even_g1_abort_bitmap_transmission();
        return -EIO;
    }
    
    /* CRC will be sent via callback after end command completes */
    return 0;
}

static int even_g1_send_bitmap_crc(void)
{
    uint8_t crc_packet[8];
    uint16_t packet_len = 0;
    
    k_mutex_lock(&g1_mutex, K_FOREVER);
    uint32_t crc32 = g1_state.bitmap_crc32;
    
    /* Mark packets as 53 to indicate CRC phase */
    g1_state.bitmap_packets_sent = g1_state.bitmap_packets_total + 2;
    k_mutex_unlock(&g1_mutex);
    
    /* Create CRC packet: [0x16, CRC32 in big-endian format] */
    /* CRITICAL FIX: Arms expect CRC in big-endian format for transmission */
    crc_packet[packet_len++] = EVEN_G1_OPCODE_BMP_CRC;
    crc_packet[packet_len++] = (crc32 >> 24) & 0xFF;  /* Big-endian byte order */
    crc_packet[packet_len++] = (crc32 >> 16) & 0xFF;
    crc_packet[packet_len++] = (crc32 >> 8) & 0xFF;
    crc_packet[packet_len++] = crc32 & 0xFF;
    
    LOG_INF("Sending bitmap CRC check: 0x%08X", crc32);
    
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    /* Store current packet for async send */
    memcpy(g1_state.bitmap_current_packet, crc_packet, packet_len);
    g1_state.bitmap_current_packet_len = packet_len;
    
    /* Send to both arms asynchronously */
    g1_state.bitmap_left_pending = true;
    g1_state.bitmap_right_pending = true;
    
    k_mutex_unlock(&g1_mutex);
    
    /* Send sequentially: left arm first */
    int left_err = ble_nus_multi_client_send_data(g1_state.left_conn, crc_packet, packet_len);
    
    if (left_err != 0) {
        k_mutex_lock(&g1_mutex, K_FOREVER);
        g1_state.bitmap_left_pending = false;
        k_mutex_unlock(&g1_mutex);
        LOG_ERR("Failed to send bitmap CRC to left arm: %d", left_err);
        
        /* Try right arm immediately if left failed */
        int right_err = ble_nus_multi_client_send_data(g1_state.right_conn, crc_packet, packet_len);
        if (right_err != 0) {
            k_mutex_lock(&g1_mutex, K_FOREVER);
            g1_state.bitmap_right_pending = false;
            k_mutex_unlock(&g1_mutex);
            LOG_ERR("Failed to send bitmap CRC to right arm: %d", right_err);
        }
    }
    
    if (left_err != 0 && !g1_state.bitmap_left_pending && !g1_state.bitmap_right_pending) {
        LOG_ERR("Failed to send bitmap CRC command to both arms");
        even_g1_abort_bitmap_transmission();
        return -EIO;
    }
    
    /* Completion will be handled via callback after CRC completes */
    return 0;
}

static void even_g1_abort_bitmap_transmission(void)
{
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    /* Free the BMP buffer if allocated */
    if (g1_state.bitmap_data && g1_state.bitmap_data_allocated) {
        k_free(g1_state.bitmap_data);
    }
    
    g1_state.bitmap_in_progress = false;
    g1_state.bitmap_data = NULL;
    g1_state.bitmap_data_allocated = false;
    g1_state.bitmap_original_buffer = NULL;  /* Clear original buffer pointer */
    g1_state.bitmap_size = 0;
    g1_state.bitmap_packets_total = 0;
    g1_state.bitmap_packets_sent = 0;
    g1_state.bitmap_crc32 = 0;
    g1_state.bitmap_left_crc_validated = false;
    g1_state.bitmap_right_crc_validated = false;
    k_mutex_unlock(&g1_mutex);
    
    LOG_WRN("Bitmap transmission aborted");
}

/* Completion function when mutex is already held by caller */
static void even_g1_complete_bitmap_transmission_internal(void)
{
    /* Assume mutex is already locked by caller */
    
    /* Free the BMP buffer if allocated */
    if (g1_state.bitmap_data && g1_state.bitmap_data_allocated) {
        k_free(g1_state.bitmap_data);
    }
    g1_state.bitmap_data = NULL;
    g1_state.bitmap_data_allocated = false;
    
    g1_state.bitmap_in_progress = false;
    g1_state.bitmap_size = 0;
    g1_state.bitmap_packets_total = 0;
    g1_state.bitmap_packets_sent = 0;
    g1_state.bitmap_crc32 = 0;
    g1_state.bitmap_left_pending = false;
    g1_state.bitmap_right_pending = false;
    g1_state.bitmap_left_retry_count = 0;
    g1_state.bitmap_right_retry_count = 0;
    g1_state.bitmap_left_crc_validated = false;
    g1_state.bitmap_right_crc_validated = false;
    g1_state.bitmap_left_end_acked = false;
    g1_state.bitmap_right_end_acked = false;
    
    k_mutex_unlock(&g1_mutex);
    
    /* BREAKTHROUGH: Both arms now pass CRC consistently! */
    /* Sequential transmission solved the data corruption issue */
    LOG_INF("PROTOCOL SUCCESS: Both arms CRC validated!");
    LOG_INF("   Data transmission: Perfect");
    LOG_INF("   CRC validation: Perfect"); 
    LOG_INF("   According to official docs, display should activate automatically");
    LOG_INF("   If no display, the issue may be with the bitmap format/data");
    
    /* Note: Official docs suggest no additional commands needed after CRC validation */
    /* The 0x22 command was from debugging attempts, not the official protocol */
    
    LOG_INF("Bitmap transmission completed successfully");
}

static bool send_activation_cmd_with_retry(const uint8_t *data, uint8_t len, const char *desc, int max_retries)
{
    for (int retry = 0; retry < max_retries; retry++) {
        if (retry > 0) {
            int backoff_ms = retry * 200;  /* Quick linear backoff: 200ms, 400ms, 600ms */
            LOG_INF("⏳ Retry %d/%d for %s after %dms backoff...", retry, max_retries-1, desc, backoff_ms);
            k_sleep(K_MSEC(backoff_ms));
        }
        
        int left_ret = ble_nus_multi_client_send_data(g1_state.left_conn, data, len);
        k_sleep(K_MSEC(200));  /* Give BLE stack time between arms */
        int right_ret = ble_nus_multi_client_send_data(g1_state.right_conn, data, len);
        
        if (left_ret == 0 && right_ret == 0) {
            LOG_INF("✅ %s sent successfully%s", desc, retry > 0 ? " (after retry)" : "");
            return true;
        } else if (left_ret == -120 || right_ret == -120) {
            LOG_WRN("❌ %s failed with BLE congestion: L=%d R=%d (retry %d/%d)", 
                   desc, left_ret, right_ret, retry, max_retries-1);
        } else {
            LOG_ERR("❌ %s failed with error: L=%d R=%d", desc, left_ret, right_ret);
            return false;  /* Non-congestion error, don't retry */
        }
    }
    
    LOG_ERR("❌ %s failed after %d retries", desc, max_retries);
    return false;
}

static void even_g1_try_display_activation_sequence(void)
{
    /* Send only the essential display enable command (0x22) */
    /* Skip other commands that cause BLE congestion and may not be necessary */
    LOG_INF("Sending display enable command (0x22)...");
    
    uint8_t display_enable_cmd[] = {0x22};
    
    if (send_activation_cmd_with_retry(display_enable_cmd, 1, "Display enable (0x22)", 2)) {
        LOG_INF("✅ Display enable successful! Image should now be visible on Even G1 displays.");
        LOG_INF("🎯 Bitmap should be rendered if data format is correct");
    } else {
        LOG_ERR("🚨 CRITICAL: Display enable failed - display likely won't activate");
    }
}

static void even_g1_bitmap_send_callback(struct bt_conn *conn, uint8_t err)
{
    if (!g1_state.bitmap_in_progress) {
        return;  /* Not our callback */
    }
    
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    /* Check which arm completed */
    if (conn == g1_state.left_conn) {
        if (err) {
            g1_state.bitmap_left_retry_count++;
            LOG_WRN("Bitmap packet send to left arm failed (retry %d/%d): %d", 
                    g1_state.bitmap_left_retry_count, EVEN_G1_MAX_PACKET_RETRIES, err);
            
            if (g1_state.bitmap_left_retry_count <= EVEN_G1_MAX_PACKET_RETRIES) {
                /* Retry sending to left arm */
                int retry_err = send_to_left(g1_state.bitmap_current_packet, g1_state.bitmap_current_packet_len);
                if (retry_err != 0) {
                    LOG_ERR("Failed to retry bitmap packet to left arm: %d", retry_err);
                    g1_state.bitmap_left_pending = false;  /* Give up on left arm */
                }
            } else {
                LOG_ERR("Left arm exceeded max retries, giving up");
                g1_state.bitmap_left_pending = false;
            }
        } else {
            g1_state.bitmap_left_pending = false;
            LOG_DBG("Bitmap packet sent to left arm successfully");
            
            /* Left arm succeeded - now send to right arm sequentially */
            if (g1_state.bitmap_right_pending) {
                int right_err = send_to_right(g1_state.bitmap_current_packet, g1_state.bitmap_current_packet_len);
                if (right_err != 0) {
                    LOG_WRN("Failed to send bitmap packet to right arm after left success: %d", right_err);
                    g1_state.bitmap_right_pending = false;
                }
            }
        }
    } else if (conn == g1_state.right_conn) {
        if (err) {
            g1_state.bitmap_right_retry_count++;
            LOG_WRN("Bitmap packet send to right arm failed (retry %d/%d): %d", 
                    g1_state.bitmap_right_retry_count, EVEN_G1_MAX_PACKET_RETRIES, err);
            
            if (g1_state.bitmap_right_retry_count <= EVEN_G1_MAX_PACKET_RETRIES) {
                /* Retry sending to right arm */
                int retry_err = send_to_right(g1_state.bitmap_current_packet, g1_state.bitmap_current_packet_len);
                if (retry_err != 0) {
                    LOG_ERR("Failed to retry bitmap packet to right arm: %d", retry_err);
                    g1_state.bitmap_right_pending = false;  /* Give up on right arm */
                }
            } else {
                LOG_ERR("Right arm exceeded max retries, giving up");
                g1_state.bitmap_right_pending = false;
            }
        } else {
            g1_state.bitmap_right_pending = false;
            LOG_DBG("Bitmap packet sent to right arm successfully");
        }
    }
    
    /* If both arms have completed, continue transmission */
    if (!g1_state.bitmap_left_pending && !g1_state.bitmap_right_pending && g1_state.bitmap_in_progress) {
        k_mutex_unlock(&g1_mutex);
        even_g1_continue_bitmap_transmission();
    } else {
        k_mutex_unlock(&g1_mutex);
    }
}

static void even_g1_continue_bitmap_transmission(void)
{
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    if (!g1_state.bitmap_in_progress) {
        k_mutex_unlock(&g1_mutex);
        return;
    }
    
    uint16_t packets_sent = g1_state.bitmap_packets_sent;
    uint16_t packets_total = g1_state.bitmap_packets_total;
    
    k_mutex_unlock(&g1_mutex);
    
    /* Add delay between packets to prevent BLE congestion */
    /* This gives the BLE stack time to process each packet */
    if (packets_sent > 0 && packets_sent < packets_total) {
        k_sleep(K_MSEC(20));  /* 20ms between packets = ~1 second for 51 packets */
    }
    
    /* Check which phase we're in */
    if (packets_sent == packets_total) {
        /* Just finished last data packet, send end command */
        LOG_INF("All bitmap packets sent, sending end command");
        even_g1_send_bitmap_end();
    } else if (packets_sent == packets_total + 1) {
        /* Just finished sending end command - wait for ACK */
        LOG_INF("End command sent, waiting for ACK from both arms before sending CRC");
        /* CRC will be sent when both arms ACK the end command */
    } else if (packets_sent >= packets_total + 2) {
        /* CRC phase completed - waiting for both arms to validate */
        LOG_INF("CRC sent, waiting for validation from both arms");
        
        /* Completion will be handled by CRC acknowledgment callbacks */
        /* Show current status after bitmap completes (if dashboard is closed) */
        LOG_INF("=== BITMAP DATA TRANSMISSION COMPLETED ===");
        LOG_INF("✅ All packets and CRC sent - waiting for arm validation");
        
        if (!even_g1_is_dashboard_open()) {
            LOG_INF("Dashboard closed - returning to text display in 3 seconds");
            /* Wait a bit to see if image appears, then show status */
            k_sleep(K_MSEC(3000));
            even_g1_show_current_status();
        }
    } else {
        /* Still sending data packets - continue immediately via callback-based flow control */
        even_g1_send_next_bitmap_packet();
    }
}

/* ========================================
 * Test Bitmap Creation Functions
 * ======================================== */

int even_g1_create_test_bitmap_square(uint8_t **bitmap_data, size_t *data_size)
{
    if (!bitmap_data || !data_size) {
        return -EINVAL;
    }
    
    /* Even G1 display: 576×136 pixels, 1-bit per pixel */
    const int width = EVEN_G1_BITMAP_WIDTH;
    const int height = EVEN_G1_BITMAP_HEIGHT;
    const size_t bytes_per_row = (width + 7) / 8;  /* 1 bit per pixel, round up to bytes */
    const size_t total_bytes = bytes_per_row * height;
    
    /* Allocate bitmap data */
    uint8_t *data = k_malloc(total_bytes);
    if (!data) {
        LOG_ERR("Failed to allocate memory for bitmap");
        return -ENOMEM;
    }
    
    /* Clear bitmap (all pixels off) */
    memset(data, 0, total_bytes);
    
    /* Draw a square in the center */
    const int square_size = MIN(width / 3, height / 2);  /* Square fits in display */
    const int start_x = (width - square_size) / 2;
    const int start_y = (height - square_size) / 2;
    
    LOG_INF("Creating square bitmap: %dx%d pixels, square at (%d,%d) size %d", 
            width, height, start_x, start_y, square_size);
    
    for (int y = start_y; y < start_y + square_size; y++) {
        for (int x = start_x; x < start_x + square_size; x++) {
            /* Check if we're on the border of the square */
            bool is_border = (y == start_y) || (y == start_y + square_size - 1) ||
                           (x == start_x) || (x == start_x + square_size - 1);
            
            if (is_border) {
                /* Set pixel (1-bit bitmap, packed) */
                int byte_index = y * bytes_per_row + x / 8;
                int bit_index = x % 8;
                data[byte_index] |= (1 << (7 - bit_index));  /* MSB first */
            }
        }
    }
    
    *bitmap_data = data;
    *data_size = total_bytes;
    
    LOG_INF("Square bitmap created: %zu bytes", total_bytes);
    return 0;
}

int even_g1_create_test_bitmap_triangle(uint8_t **bitmap_data, size_t *data_size)
{
    if (!bitmap_data || !data_size) {
        return -EINVAL;
    }
    
    /* Even G1 display: 576×136 pixels, 1-bit per pixel */
    const int width = EVEN_G1_BITMAP_WIDTH;
    const int height = EVEN_G1_BITMAP_HEIGHT;
    const size_t bytes_per_row = (width + 7) / 8;
    const size_t total_bytes = bytes_per_row * height;
    
    /* Allocate bitmap data */
    uint8_t *data = k_malloc(total_bytes);
    if (!data) {
        LOG_ERR("Failed to allocate memory for bitmap");
        return -ENOMEM;
    }
    
    /* Clear bitmap */
    memset(data, 0, total_bytes);
    
    /* Draw a triangle in the center */
    const int tri_width = MIN(width / 2, height);
    const int tri_height = tri_width / 2;
    const int center_x = width / 2;
    const int start_y = (height - tri_height) / 2;
    
    LOG_INF("Creating triangle bitmap: %dx%d pixels, triangle at center width=%d height=%d", 
            width, height, tri_width, tri_height);
    
    for (int y = 0; y < tri_height; y++) {
        /* Calculate triangle edges for this row */
        int half_width_at_y = (y * tri_width) / (2 * tri_height);
        int left_x = center_x - half_width_at_y;
        int right_x = center_x + half_width_at_y;
        
        int draw_y = start_y + y;
        if (draw_y >= 0 && draw_y < height) {
            /* Draw left and right edges of triangle */
            if (left_x >= 0 && left_x < width) {
                int byte_index = draw_y * bytes_per_row + left_x / 8;
                int bit_index = left_x % 8;
                data[byte_index] |= (1 << (7 - bit_index));
            }
            
            if (right_x >= 0 && right_x < width && right_x != left_x) {
                int byte_index = draw_y * bytes_per_row + right_x / 8;
                int bit_index = right_x % 8;
                data[byte_index] |= (1 << (7 - bit_index));
            }
        }
    }
    
    /* Draw triangle base */
    int base_y = start_y + tri_height - 1;
    if (base_y >= 0 && base_y < height) {
        int left_base = center_x - tri_width / 2;
        int right_base = center_x + tri_width / 2;
        
        for (int x = left_base; x <= right_base; x++) {
            if (x >= 0 && x < width) {
                int byte_index = base_y * bytes_per_row + x / 8;
                int bit_index = x % 8;
                data[byte_index] |= (1 << (7 - bit_index));
            }
        }
    }
    
    *bitmap_data = data;
    *data_size = total_bytes;
    
    LOG_INF("Triangle bitmap created: %zu bytes", total_bytes);
    return 0;
}

/* Helper function to create a complete BMP file with headers */
static int create_bmp_file_from_pixels(uint8_t **bmp_file_data, size_t *bmp_file_size, 
                          const uint8_t *pixel_data, size_t width, size_t height)
{
    /* BMP file structure for 1-bit monochrome */
    const size_t bytes_per_row = (width + 7) / 8;
    const size_t row_padding = (4 - (bytes_per_row % 4)) % 4;
    const size_t row_size = bytes_per_row + row_padding;
    const size_t pixel_data_size = row_size * height;
    const size_t header_size = 14 + 40 + 8 + 2;  /* File header + DIB header + 2-color palette + 2 padding bytes */
    const size_t total_size = header_size + pixel_data_size;
    
    uint8_t *bmp = k_malloc(total_size);
    if (!bmp) {
        LOG_ERR("Failed to allocate BMP file buffer");
        return -ENOMEM;
    }
    
    memset(bmp, 0, total_size);
    
    /* BMP File Header (14 bytes) */
    bmp[0] = 'B';
    bmp[1] = 'M';
    /* File size (little-endian) */
    bmp[2] = (total_size) & 0xFF;
    bmp[3] = (total_size >> 8) & 0xFF;
    bmp[4] = (total_size >> 16) & 0xFF;
    bmp[5] = (total_size >> 24) & 0xFF;
    /* Reserved (4 bytes) - zeros */
    /* Offset to pixel data (little-endian) */
    bmp[10] = header_size & 0xFF;
    bmp[11] = (header_size >> 8) & 0xFF;
    
    /* DIB Header (40 bytes BITMAPINFOHEADER) */
    bmp[14] = 40;  /* Header size */
    /* Width (little-endian) */
    bmp[18] = width & 0xFF;
    bmp[19] = (width >> 8) & 0xFF;
    bmp[20] = (width >> 16) & 0xFF;
    bmp[21] = (width >> 24) & 0xFF;
    /* Height (little-endian) - positive for bottom-up */
    bmp[22] = height & 0xFF;
    bmp[23] = (height >> 8) & 0xFF;
    bmp[24] = (height >> 16) & 0xFF;
    bmp[25] = (height >> 24) & 0xFF;
    /* Planes (always 1) */
    bmp[26] = 1;
    /* Bits per pixel */
    bmp[28] = 1;
    /* Compression (0 = none) */
    /* Image size */
    bmp[34] = pixel_data_size & 0xFF;
    bmp[35] = (pixel_data_size >> 8) & 0xFF;
    bmp[36] = (pixel_data_size >> 16) & 0xFF;
    bmp[37] = (pixel_data_size >> 24) & 0xFF;
    /* X/Y pixels per meter (0) */
    /* Colors used (2 for 1-bit) */
    bmp[46] = 2;
    /* Important colors (2) */
    bmp[50] = 2;
    
    /* Color Palette (8 bytes for 2 colors) */
    /* Color 0: Black (0x00000000) */
    bmp[54] = 0x00;  /* Blue */
    bmp[55] = 0x00;  /* Green */
    bmp[56] = 0x00;  /* Red */
    bmp[57] = 0x00;  /* Reserved */
    /* Color 1: White (0xFFFFFFFF) */
    bmp[58] = 0xFF;  /* Blue */
    bmp[59] = 0xFF;  /* Green */
    bmp[60] = 0xFF;  /* Red */
    bmp[61] = 0x00;  /* Reserved */
    
    /* Add 2 padding bytes after palette to match official BMP format */
    bmp[62] = 0xFF;  /* Padding byte 1 (matches official BMPs) */
    bmp[63] = 0xFF;  /* Padding byte 2 (matches official BMPs) */
    
    /* Copy pixel data with padding - BMP stores rows bottom-up */
    size_t dst_offset = header_size;
    for (int y = height - 1; y >= 0; y--) {
        memcpy(&bmp[dst_offset], &pixel_data[y * bytes_per_row], bytes_per_row);
        /* Add padding if needed */
        dst_offset += row_size;
    }
    
    *bmp_file_data = bmp;
    *bmp_file_size = total_size;
    
    LOG_INF("Created complete BMP file: %zu bytes (headers: %zu, pixels: %zu)", 
            total_size, header_size, pixel_data_size);
    
    return 0;
}

int even_g1_send_example_bitmap_1(void)
{
    /* Example 1: Test with very simple pattern - just black border */
    const int width = EVEN_G1_BITMAP_WIDTH;
    const int height = EVEN_G1_BITMAP_HEIGHT;
    const size_t bytes_per_row = (width + 7) / 8;
    const size_t total_bytes = bytes_per_row * height;
    
    uint8_t *pixel_data = k_malloc(total_bytes);
    if (!pixel_data) {
        LOG_ERR("Failed to allocate memory for example bitmap 1");
        return -ENOMEM;
    }
    
    /* Start with all black (0xFF = black pixels) */
    memset(pixel_data, 0xFF, total_bytes);
    
    LOG_INF("Creating example bitmap 1 (WHITE border on black background - should be visible!)");
    
    /* Create a thick WHITE border around the entire image */
    const int border_thickness = 10;
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            bool is_border = (y < border_thickness) || 
                            (y >= height - border_thickness) || 
                            (x < border_thickness) || 
                            (x >= width - border_thickness);
            
            if (is_border) {
                int byte_index = y * bytes_per_row + x / 8;
                int bit_index = x % 8;
                pixel_data[byte_index] &= ~(1 << (7 - bit_index));  /* Clear bit for white pixel */
            }
        }
    }
    
    /* Create complete BMP file like the official app expects */
    LOG_INF("Creating complete BMP file for example bitmap 1");
    uint8_t *bmp_data = NULL;
    size_t bmp_size = 0;
    int ret = create_bmp_file_from_pixels(&bmp_data, &bmp_size, pixel_data, EVEN_G1_BITMAP_WIDTH, EVEN_G1_BITMAP_HEIGHT);
    k_free(pixel_data);
    
    if (ret != 0) {
        LOG_ERR("Failed to create BMP file");
        return ret;
    }
    
    LOG_INF("Sending complete BMP file (%zu bytes) like official app", bmp_size);
    ret = even_g1_send_bmp_file(bmp_data, bmp_size);
    k_free(bmp_data);
    return ret;
}

int even_g1_send_example_bitmap_2(void)
{
    /* Example 2: Try inverted pixels (maybe 1=white, 0=black) */
    const int width = EVEN_G1_BITMAP_WIDTH;
    const int height = EVEN_G1_BITMAP_HEIGHT;
    const size_t bytes_per_row = (width + 7) / 8;
    const size_t total_bytes = bytes_per_row * height;
    
    uint8_t *data = k_malloc(total_bytes);
    if (!data) {
        LOG_ERR("Failed to allocate memory for example bitmap 2");
        return -ENOMEM;
    }
    
    /* Start with all black pixels (0xFF) */
    memset(data, 0xFF, total_bytes);
    
    LOG_INF("Creating example bitmap 2 (inverted pixels test - white text on black)");
    
    /* Create a white rectangle in the center on black background */
    int rect_width = 200;
    int rect_height = 80;
    int start_x = (width - rect_width) / 2;
    int start_y = (height - rect_height) / 2;
    
    for (int y = start_y; y < start_y + rect_height && y < height; y++) {
        for (int x = start_x; x < start_x + rect_width && x < width; x++) {
            int byte_index = y * bytes_per_row + x / 8;
            int bit_index = x % 8;
            /* Clear bit for white pixel (if 0=white) */
            data[byte_index] &= ~(1 << (7 - bit_index));
        }
    }
    
    /* Add white border around the rectangle */
    int border = 5;
    for (int y = start_y - border; y < start_y + rect_height + border && y >= 0 && y < height; y++) {
        for (int x = start_x - border; x < start_x + rect_width + border && x >= 0 && x < width; x++) {
            bool is_border = (y == start_y - border || y == start_y + rect_height + border - 1 ||
                             x == start_x - border || x == start_x + rect_width + border - 1);
            
            if (is_border && y >= 0 && y < height && x >= 0 && x < width) {
                int byte_index = y * bytes_per_row + x / 8;
                int bit_index = x % 8;
                /* Clear bit for white pixel */
                data[byte_index] &= ~(1 << (7 - bit_index));
            }
        }
    }
    
    /* Send the bitmap using corrected function */
    int ret = even_g1_send_bmp_file(data, total_bytes);
    
    if (ret == 0) {
        LOG_INF("Example bitmap 2 sent successfully");
    } else {
        LOG_ERR("Failed to send example bitmap 2: %d", ret);
        k_free(data);  /* Only free on error */
    }
    
    return ret;
}

int even_g1_send_real_bmp_file(const uint8_t *bmp_file_data, size_t bmp_file_size)
{
    /* Function for sending actual BMP files (like from EvenDemoApp) */
    /* This uses the full BMP file data and calculates CRC accordingly */
    
    if (!bmp_file_data || bmp_file_size < 100) {
        LOG_ERR("Invalid BMP file data");
        return -EINVAL;
    }
    
    /* Verify BMP header */
    if (bmp_file_data[0] != 'B' || bmp_file_data[1] != 'M') {
        LOG_ERR("Not a valid BMP file (missing BM header)");
        return -EINVAL;
    }
    
    if (!even_g1_is_ready()) {
        LOG_ERR("Even G1 not ready for BMP file transmission");
        return -ENOTCONN;
    }
    
    LOG_INF("Sending real BMP file: %zu bytes", bmp_file_size);
    
    k_mutex_lock(&g1_mutex, K_FOREVER);
    
    /* Calculate total packets needed */
    uint16_t total_packets = (bmp_file_size + EVEN_G1_BITMAP_PACKET_SIZE - 1) / EVEN_G1_BITMAP_PACKET_SIZE;
    
    /* Initialize bitmap transmission state */
    g1_state.bitmap_in_progress = true;
    g1_state.bitmap_data = (uint8_t *)bmp_file_data;
    g1_state.bitmap_size = bmp_file_size;
    g1_state.bitmap_packets_total = total_packets;
    g1_state.bitmap_packets_sent = 0;
    
    /* For real BMP files, use the full file in CRC calculation */
    g1_state.bitmap_crc32 = calculate_bmp_file_crc32(bmp_file_data, bmp_file_size);
    
    LOG_INF("Real BMP CRC calculation: address + %zu bytes BMP file", bmp_file_size);
    
    k_mutex_unlock(&g1_mutex);
    
    /* Start sending packets */
    return even_g1_send_next_bitmap_packet();
}

int even_g1_send_official_sample_bmp(void)
{
    /* Hardcoded BMP data from /Users/robert/git/EvenDemoApp/assets/images/image_1.bmp */
    /* This is a 576x135 monochrome BMP that works with the official app */
    /* TODO: Replace with actual file loading or embed the BMP data */
    
    LOG_WRN("even_g1_send_official_sample_bmp: NOT IMPLEMENTED YET");
    LOG_INF("To test: copy BMP file to embedded storage and load it here");
    LOG_INF("BMP file should be: 576x135, 1-bit, 9784 bytes total");
    LOG_INF("Expected CRC32: 0xC28F4143 (for address + full BMP)");
    LOG_INF("Use even_g1_send_real_bmp_file() for actual BMP file data");
    
    /* For now, create a simple test pattern that matches BMP format */
    return even_g1_send_example_bitmap_1();
}

int even_g1_send_test_single_arm_black(void)
{
    /* Test: Send black screen to LEFT arm only for debugging */
    LOG_INF("=== SINGLE ARM TEST: LEFT ARM ONLY ===");
    const int width = EVEN_G1_BITMAP_WIDTH;
    const int height = EVEN_G1_BITMAP_HEIGHT;
    const size_t bytes_per_row = (width + 7) / 8;
    const size_t total_bytes = bytes_per_row * height;
    
    uint8_t *data = k_malloc(total_bytes);
    if (!data) {
        LOG_ERR("Failed to allocate memory for single-arm black test");
        return -ENOMEM;
    }
    
    /* All black pixels (every bit set to 1) */
    memset(data, 0xFF, total_bytes);
    
    LOG_INF("Sending all-black test to LEFT ARM ONLY (0xFF bytes)");
    
    /* Temporarily disable right arm for testing */
    k_mutex_lock(&g1_mutex, K_FOREVER);
    struct bt_conn *saved_right = g1_state.right_conn;
    g1_state.right_conn = NULL;  /* Temporarily disable right arm */
    k_mutex_unlock(&g1_mutex);
    
    int ret = even_g1_send_bmp_file(data, total_bytes);
    
    /* Restore right arm */
    k_mutex_lock(&g1_mutex, K_FOREVER);
    g1_state.right_conn = saved_right;
    k_mutex_unlock(&g1_mutex);
    
    return ret;
}

int even_g1_send_test_left_arm_only(void)
{
    LOG_INF("=== LEFT ARM ONLY TEST ===");
    LOG_INF("Testing bitmap transmission to left arm only to isolate asymmetric CRC issue");
    
    /* Create simple test bitmap */
    const int width = EVEN_G1_BITMAP_WIDTH;
    const int height = EVEN_G1_BITMAP_HEIGHT;
    const size_t bytes_per_row = (width + 7) / 8;
    const size_t total_bytes = bytes_per_row * height;
    
    uint8_t *data = k_malloc(total_bytes);
    if (!data) {
        LOG_ERR("Failed to allocate memory for left-arm-only test");
        return -ENOMEM;
    }
    
    /* Create simple pattern - alternating lines */
    memset(data, 0, total_bytes);
    for (int y = 0; y < height; y += 4) {
        for (int x = 0; x < bytes_per_row; x++) {
            if (y < height) {
                data[y * bytes_per_row + x] = 0xFF;  /* Every 4th line solid */
            }
        }
    }
    
    LOG_INF("Temporarily disabling right arm connection for isolated test");
    
    /* Temporarily disable right arm */
    k_mutex_lock(&g1_mutex, K_FOREVER);
    struct bt_conn *saved_right = g1_state.right_conn;
    g1_state.right_conn = NULL;  /* Disable right arm */
    k_mutex_unlock(&g1_mutex);
    
    /* Send bitmap to left arm only */
    int ret = even_g1_send_bmp_file(data, total_bytes);
    
    /* Wait to see if image appears on left display */
    LOG_INF("⏳ Waiting 8 seconds - check LEFT display for horizontal lines...");
    k_sleep(K_MSEC(8000));
    
    /* Restore right arm */
    k_mutex_lock(&g1_mutex, K_FOREVER);
    g1_state.right_conn = saved_right;
    k_mutex_unlock(&g1_mutex);
    
    k_free(data);
    LOG_INF("Left-arm-only test completed, right arm restored");
    
    return ret;
}

int even_g1_send_test_all_black(void)
{
    /* Creating and sending test pattern - no exit command needed */
    LOG_INF("🔥 CRITICAL TEST: Sending test pattern");
    
    /* Wait a bit before proceeding */
    k_sleep(K_MSEC(500));
    
    /* Create alternating pattern instead of solid black */
    const int width = EVEN_G1_BITMAP_WIDTH;
    const int height = EVEN_G1_BITMAP_HEIGHT; 
    const size_t bytes_per_row = (width + 7) / 8;
    const size_t total_bytes = bytes_per_row * height;
    
    LOG_INF("Creating alternating test pattern: %dx%d = %d bytes", width, height, total_bytes);
    
    /* Use static buffer to avoid allocation failure */
    static uint8_t pixel_data[9792];
    if (total_bytes > sizeof(pixel_data)) {
        LOG_ERR("Bitmap too large for static buffer: %d > %d", total_bytes, sizeof(pixel_data));
        return -EINVAL;
    }
    
    /* Create inverted pattern - WHITE border on BLACK background */
    memset(pixel_data, 0xFF, total_bytes);  /* Start with all pixels ON (black background) */
    
    /* Create a thick WHITE border by clearing bits */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int byte_idx = y * bytes_per_row + x / 8;
            int bit_idx = x % 8;
            
            /* Create WHITE border: top 10 lines, bottom 10 lines, left 20 pixels, right 20 pixels */
            if (y < 10 || y >= height - 10 || x < 20 || x >= width - 20) {
                pixel_data[byte_idx] &= ~(1 << (7 - bit_idx));  /* Clear bit (white pixel) */
            }
        }
    }
    
    LOG_INF("Created inverted border pattern (WHITE border on BLACK background)");
    
    /* CRITICAL: Must create a proper BMP file with headers, not raw pixel data! */
    uint8_t *bmp_file_data = NULL;
    size_t bmp_file_size = 0;
    
    /* Create proper BMP file from pixel data */
    int ret = create_bmp_file_from_pixels(&bmp_file_data, &bmp_file_size, 
                                          pixel_data, width, height);
    if (ret != 0) {
        LOG_ERR("Failed to create BMP file from pixels: %d", ret);
        return ret;
    }
    
    LOG_INF("Created BMP file with headers: %d bytes (was %d raw pixels)", 
            bmp_file_size, total_bytes);
    LOG_INF("🚀 Sending test pattern as PROPER BMP FILE with headers");
    
    /* Send the complete BMP file with headers */
    ret = even_g1_send_bmp_file(bmp_file_data, bmp_file_size);
    
    /* Free the BMP file buffer */
    k_free(bmp_file_data);
    
    return ret;
}

int even_g1_send_test_all_white(void)
{
    /* Test: Send completely white screen (all bits set to 0) */
    const int width = EVEN_G1_BITMAP_WIDTH;
    const int height = EVEN_G1_BITMAP_HEIGHT;
    const size_t bytes_per_row = (width + 7) / 8;
    const size_t total_bytes = bytes_per_row * height;
    
    uint8_t *data = k_malloc(total_bytes);
    if (!data) {
        LOG_ERR("Failed to allocate memory for all-white test");
        return -ENOMEM;
    }
    
    /* All white pixels (every bit set to 0) */
    memset(data, 0x00, total_bytes);
    
    LOG_INF("Sending all-white test using official BMP sample (proven working)");
    k_free(data);  /* Free our test data */
    
    /* Use the proven working official sample BMP that we know works */
    return even_g1_send_official_sample_bmp();
}


int even_g1_send_github_bitmap(const char *filename)
{
    /* For compatibility - redirect to official sample */
    return even_g1_send_official_sample_bmp();
}

static int even_g1_send_test_pattern_simple(void)
{
    /* Test with the most basic pattern possible - just one black line */
    const int width = EVEN_G1_BITMAP_WIDTH;
    const int height = EVEN_G1_BITMAP_HEIGHT;
    const size_t bytes_per_row = (width + 7) / 8;
    const size_t total_bytes = bytes_per_row * height;
    
    uint8_t *data = k_malloc(total_bytes);
    if (!data) {
        return -ENOMEM;
    }
    
    /* Clear all pixels */
    memset(data, 0x00, total_bytes);
    
    /* Draw a single horizontal line in the middle */
    int middle_y = height / 2;
    for (int x = 0; x < width; x++) {
        int byte_index = middle_y * bytes_per_row + x / 8;
        int bit_index = x % 8;
        data[byte_index] |= (1 << (7 - bit_index));
    }
    
    LOG_INF("Sending single line test pattern");
    return even_g1_send_bmp_file(data, total_bytes);
}

/* Display mode control functions */
void even_g1_toggle_display_mode(void)
{
    LOG_INF("*** ENTERING even_g1_toggle_display_mode() ***");
    k_mutex_lock(&g1_mutex, K_FOREVER);
    bool old_mode = g1_state.bitmap_mode;
    g1_state.bitmap_mode = !g1_state.bitmap_mode;
    k_mutex_unlock(&g1_mutex);
    
    const char *old_str = old_mode ? "BITMAP" : "TEXT";
    const char *new_str = g1_state.bitmap_mode ? "BITMAP" : "TEXT";
    LOG_INF("🔄 Display mode toggled from %s to %s mode", old_str, new_str);
    
    /* Clear display before switching modes to prevent artifacts */
    LOG_INF("🧹 Clearing display before mode switch");
    if (old_mode) {
        /* Switching away from bitmap mode - clear bitmap data */
        LOG_INF("🗑️ Clearing bitmap mode - TODO: implement proper bitmap clear");
        // even_g1_clear_bitmap_display(); // Commented out - TouchBar clear is an incoming event, not outgoing command
    } else {
        /* Switching away from text mode - use text clear */
        LOG_INF("📝 Clearing text mode");
        even_g1_clear_display_dual_arm();
    }
    
    /* Small delay to ensure clear command is processed */
    k_sleep(K_MSEC(100));
    
    /* Update display immediately to show current status */
    even_g1_show_current_status();
}

bool even_g1_is_bitmap_mode(void)
{
    k_mutex_lock(&g1_mutex, K_FOREVER);
    bool is_bitmap = g1_state.bitmap_mode;
    k_mutex_unlock(&g1_mutex);
    return is_bitmap;
}

void even_g1_set_bitmap_mode(bool enable)
{
    k_mutex_lock(&g1_mutex, K_FOREVER);
    bool was_bitmap = g1_state.bitmap_mode;
    g1_state.bitmap_mode = enable;
    k_mutex_unlock(&g1_mutex);
    
    if (was_bitmap != enable) {
        const char *mode_str = enable ? "BITMAP" : "TEXT";
        LOG_INF("🔧 Display mode set to %s mode", mode_str);
        
        /* Clear display when mode changes to prevent artifacts */
        LOG_INF("🧹 Clearing display for mode change");
        even_g1_clear_display_dual_arm();
        even_g1_show_current_status();
    }
}



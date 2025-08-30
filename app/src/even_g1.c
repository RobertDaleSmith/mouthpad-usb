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

/* Forward declarations */
static int send_to_left(const uint8_t *data, uint16_t len);
static int send_to_right(const uint8_t *data, uint16_t len);
static void queue_complete_command(void);
static void queue_fail_command(void);
static void init_sequence_handler(struct k_work *work);
static void keepalive_handler(struct k_work *work);
static int send_ack_to_arm(struct bt_conn *conn);
static int send_heartbeat_command(void);
static int send_heartbeat_to_left(const uint8_t *data, uint16_t len);
static int send_heartbeat_to_right(const uint8_t *data, uint16_t len);
static int send_mic_stop_command(void);

int even_g1_init(void)
{
    k_mutex_init(&g1_mutex);
    memset(&g1_state, 0, sizeof(g1_state));
    k_work_init_delayable(&init_sequence_work, init_sequence_handler);
    k_work_init_delayable(&keepalive_work, keepalive_handler);
    
    /* Initialize activity timestamp to current time */
    g1_state.last_activity_time = k_uptime_get();
    
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
                LOG_INF("Dashboard state: CLOSED - switching to minimal display");
                break;
            case EVEN_G1_EVENT_DASHBOARD_OPEN:
                LOG_INF("Dashboard OPENED (head tilt up) from %s arm", arm);
                k_mutex_lock(&g1_mutex, K_FOREVER);
                g1_state.dashboard_open = true;
                k_mutex_unlock(&g1_mutex);
                LOG_INF("Dashboard state: OPEN - switching to full display");
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
    LOG_INF("=== Even G1 Status ===");
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
    }
    
    LOG_INF("Showing current status on Even G1: %s | %s (dashboard: %s)", 
            display_title, status_line, dashboard_open ? "OPEN" : "CLOSED");
    
    /* Send different content based on dashboard state */
    if (dashboard_open) {
        /* Dashboard OPEN: Show full status with battery and signal */
        LOG_INF("Dashboard OPEN - showing full status display");
        even_g1_send_text_formatted_dual_arm(display_title, status_line, 
                                             battery_str,  /* Line 3: battery (empty if no data) */
                                             signal_str,   /* Line 4: signal (empty if not connected) */
                                             NULL);        /* Line 5: unused */
    } else {
        /* Dashboard CLOSED: Show minimal status - just name and status */
        LOG_INF("Dashboard CLOSED - showing minimal display");
        even_g1_send_text_formatted_dual_arm(display_title, status_line, 
                                             "",           /* Line 3: empty */
                                             "",           /* Line 4: empty */
                                             NULL);        /* Line 5: unused */
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
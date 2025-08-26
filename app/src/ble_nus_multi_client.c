/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_nus_multi_client.h"
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/services/nus_client.h>
#include <bluetooth/services/nus.h>
#include <bluetooth/gatt_dm.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <string.h>

LOG_MODULE_REGISTER(ble_nus_multi_client, LOG_LEVEL_INF);

#define MAX_NUS_CONNECTIONS 4

/* NUS Client connection entry */
typedef struct {
    struct bt_conn *conn;
    struct bt_nus_client nus_client;
    bool active;
    bool discovered;
    uint16_t mtu;
} nus_connection_t;

/* NUS connections array */
static nus_connection_t nus_connections[MAX_NUS_CONNECTIONS];
static struct k_mutex nus_mutex;

/* Discovery queue for handling multiple simultaneous discovery requests */
#define MAX_DISCOVERY_QUEUE 4
static struct bt_conn *discovery_queue[MAX_DISCOVERY_QUEUE];
static int discovery_queue_head = 0;
static int discovery_queue_tail = 0;
static int discovery_queue_count = 0;
static bool discovery_in_progress = false;

/* Callback functions for external modules */
static ble_nus_multi_data_received_cb_t data_received_cb;
static ble_nus_multi_discovery_complete_cb_t discovery_complete_cb;
static ble_nus_multi_mtu_exchange_cb_t mtu_exchange_cb;

/* Forward declarations */
static uint8_t nus_data_received(struct bt_nus_client *nus, const uint8_t *data, uint16_t len);
static void nus_data_sent(struct bt_nus_client *nus, uint8_t err, const uint8_t *const data, uint16_t len);
static void discovery_complete(struct bt_gatt_dm *dm, void *context);
static void discovery_service_not_found(struct bt_conn *conn, void *context);
static void discovery_error(struct bt_conn *conn, int err, void *context);
static void exchange_func(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params);

/* Discovery queue management functions */
static bool queue_discovery_request(struct bt_conn *conn);
static struct bt_conn *dequeue_discovery_request(void);
static void process_next_discovery(void);
static void start_discovery_for_connection(struct bt_conn *conn);

/* Find NUS connection by bt_conn */
static nus_connection_t *find_nus_connection(struct bt_conn *conn)
{
    for (int i = 0; i < MAX_NUS_CONNECTIONS; i++) {
        if (nus_connections[i].active && nus_connections[i].conn == conn) {
            return &nus_connections[i];
        }
    }
    return NULL;
}

/* Find NUS connection by nus_client */
static nus_connection_t *find_nus_connection_by_client(struct bt_nus_client *nus)
{
    for (int i = 0; i < MAX_NUS_CONNECTIONS; i++) {
        if (nus_connections[i].active && &nus_connections[i].nus_client == nus) {
            return &nus_connections[i];
        }
    }
    return NULL;
}

/* Find free NUS connection slot */
static nus_connection_t *find_free_nus_connection(void)
{
    for (int i = 0; i < MAX_NUS_CONNECTIONS; i++) {
        if (!nus_connections[i].active) {
            return &nus_connections[i];
        }
    }
    return NULL;
}

/* NUS Client callbacks */
static uint8_t nus_data_received(struct bt_nus_client *nus, const uint8_t *data, uint16_t len)
{
    nus_connection_t *nus_conn = find_nus_connection_by_client(nus);
    if (!nus_conn) {
        LOG_ERR("NUS data received from unknown connection");
        return BT_GATT_ITER_CONTINUE;
    }
    
    LOG_DBG("NUS Multi-Client data received: %d bytes from %p", len, nus_conn->conn);

    if (data_received_cb) {
        data_received_cb(nus_conn->conn, data, len);
    }

    return BT_GATT_ITER_CONTINUE;
}

static void nus_data_sent(struct bt_nus_client *nus, uint8_t err, const uint8_t *const data, uint16_t len)
{
    nus_connection_t *nus_conn = find_nus_connection_by_client(nus);
    if (!nus_conn) {
        LOG_ERR("NUS data sent callback from unknown connection");
        return;
    }

    if (err) {
        LOG_ERR("NUS send error %d for connection %p", err, nus_conn->conn);
    } else {
        LOG_DBG("NUS data sent successfully: %d bytes to %p", len, nus_conn->conn);
    }
}

/* Discovery callbacks */
static void discovery_complete(struct bt_gatt_dm *dm, void *context)
{
    nus_connection_t *nus_conn = (nus_connection_t *)context;
    if (!nus_conn) {
        LOG_ERR("Discovery complete with null context");
        bt_gatt_dm_data_release(dm);
        return;
    }

    LOG_INF("NUS service discovered for connection %p", nus_conn->conn);

    int err = bt_nus_handles_assign(dm, &nus_conn->nus_client);
    if (err) {
        LOG_ERR("Could not assign handles for connection %p: %d", nus_conn->conn, err);
    } else {
        LOG_INF("NUS handles assigned for connection %p", nus_conn->conn);
        nus_conn->discovered = true;
        
        /* Enable notifications */
        err = bt_nus_subscribe_receive(&nus_conn->nus_client);
        if (err) {
            LOG_ERR("Failed to enable TX notifications for connection %p: %d", nus_conn->conn, err);
        } else {
            LOG_INF("TX notifications enabled for connection %p", nus_conn->conn);
        }
        
        /* Call discovery complete callback */
        if (discovery_complete_cb) {
            discovery_complete_cb(nus_conn->conn);
        }
    }

    bt_gatt_dm_data_release(dm);
    
    /* Mark discovery as complete and process next queued request */
    discovery_in_progress = false;
    process_next_discovery();
}

static void discovery_service_not_found(struct bt_conn *conn, void *context)
{
    LOG_WRN("NUS service not found for connection %p", conn);
    
    /* Mark discovery as complete and process next queued request */
    discovery_in_progress = false;
    process_next_discovery();
}

static void discovery_error(struct bt_conn *conn, int err, void *context)
{
    LOG_ERR("Error while discovering NUS for connection %p: %d", conn, err);
    
    /* Mark discovery as complete and process next queued request */
    discovery_in_progress = false;
    process_next_discovery();
}

/* Discovery callback structure */
struct bt_gatt_dm_cb multi_discovery_cb = {
    .completed = discovery_complete,
    .service_not_found = discovery_service_not_found,
    .error_found = discovery_error,
};

/* MTU exchange callback */
static void exchange_func(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params)
{
    ARG_UNUSED(params);

    k_mutex_lock(&nus_mutex, K_FOREVER);
    
    nus_connection_t *nus_conn = find_nus_connection(conn);
    if (!nus_conn) {
        LOG_ERR("MTU exchange callback for unknown connection %p", conn);
        k_mutex_unlock(&nus_mutex);
        return;
    }

    if (!err) {
        uint16_t mtu = bt_gatt_get_mtu(conn);
        nus_conn->mtu = mtu;
        printk("*** MTU EXCHANGE SUCCESSFUL for %p: MTU = %d bytes ***\n", conn, mtu);
        LOG_INF("MTU exchange done for connection %p, MTU: %d", conn, mtu);

        if (mtu_exchange_cb) {
            mtu_exchange_cb(conn, mtu);
        }
    } else {
        printk("*** MTU EXCHANGE FAILED for %p: error = %d ***\n", conn, err);
        LOG_WRN("MTU exchange failed for connection %p (err %" PRIu8 ")", conn, err);
    }
    
    k_mutex_unlock(&nus_mutex);
}

/* Public API implementations */
int ble_nus_multi_client_init(void)
{
    k_mutex_init(&nus_mutex);
    memset(nus_connections, 0, sizeof(nus_connections));
    
    LOG_INF("NUS Multi-Client module initialized");
    return 0;
}

int ble_nus_multi_client_add_connection(struct bt_conn *conn)
{
    if (!conn) {
        return -EINVAL;
    }
    
    k_mutex_lock(&nus_mutex, K_FOREVER);
    
    /* Check if connection already exists */
    if (find_nus_connection(conn)) {
        LOG_WRN("NUS connection already exists for %p", conn);
        k_mutex_unlock(&nus_mutex);
        return -EALREADY;
    }
    
    /* Find free slot */
    nus_connection_t *nus_conn = find_free_nus_connection();
    if (!nus_conn) {
        LOG_ERR("No free NUS connection slots");
        k_mutex_unlock(&nus_mutex);
        return -ENOMEM;
    }
    
    /* Initialize NUS client for this connection */
    struct bt_nus_client_init_param init = {
        .cb = {
            .received = nus_data_received,
            .sent = nus_data_sent,
        }
    };
    
    int err = bt_nus_client_init(&nus_conn->nus_client, &init);
    if (err) {
        LOG_ERR("NUS Client initialization failed for connection %p: %d", conn, err);
        k_mutex_unlock(&nus_mutex);
        return err;
    }
    
    nus_conn->conn = bt_conn_ref(conn);
    nus_conn->active = true;
    nus_conn->discovered = false;
    nus_conn->mtu = 0;
    
    LOG_INF("Added NUS connection for %p", conn);
    k_mutex_unlock(&nus_mutex);
    return 0;
}

int ble_nus_multi_client_remove_connection(struct bt_conn *conn)
{
    if (!conn) {
        return -EINVAL;
    }
    
    k_mutex_lock(&nus_mutex, K_FOREVER);
    
    nus_connection_t *nus_conn = find_nus_connection(conn);
    if (!nus_conn) {
        LOG_WRN("NUS connection not found for %p", conn);
        k_mutex_unlock(&nus_mutex);
        return -ENOENT;
    }
    
    bt_conn_unref(nus_conn->conn);
    memset(nus_conn, 0, sizeof(nus_connection_t));
    
    LOG_INF("Removed NUS connection for %p", conn);
    k_mutex_unlock(&nus_mutex);
    return 0;
}

void ble_nus_multi_client_discover(struct bt_conn *conn)
{
    k_mutex_lock(&nus_mutex, K_FOREVER);
    
    nus_connection_t *nus_conn = find_nus_connection(conn);
    if (!nus_conn) {
        LOG_ERR("NUS connection not found for discovery %p", conn);
        k_mutex_unlock(&nus_mutex);
        return;
    }
    
    /* Check if discovery is already in progress */
    if (discovery_in_progress) {
        LOG_INF("Discovery in progress, queueing request for connection %p", conn);
        if (!queue_discovery_request(conn)) {
            LOG_ERR("Failed to queue discovery request for connection %p", conn);
        }
    } else {
        /* Start discovery immediately */
        LOG_INF("Starting immediate NUS discovery for connection %p", conn);
        start_discovery_for_connection(conn);
    }
    
    k_mutex_unlock(&nus_mutex);
}

int ble_nus_multi_client_send_data(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
    if (!conn || !data || len == 0) {
        return -EINVAL;
    }
    
    k_mutex_lock(&nus_mutex, K_FOREVER);
    
    nus_connection_t *nus_conn = find_nus_connection(conn);
    if (!nus_conn) {
        LOG_ERR("NUS connection not found for send %p", conn);
        k_mutex_unlock(&nus_mutex);
        return -ENOTCONN;
    }
    
    if (!nus_conn->discovered) {
        LOG_WRN("NUS not discovered yet for connection %p", conn);
        k_mutex_unlock(&nus_mutex);
        return -EAGAIN;
    }
    
    int err = bt_nus_client_send(&nus_conn->nus_client, data, len);
    k_mutex_unlock(&nus_mutex);
    
    if (err) {
        LOG_ERR("Failed to send NUS data to connection %p: %d", conn, err);
    } else {
        LOG_DBG("NUS data sent to connection %p: %d bytes", conn, len);
    }
    
    return err;
}

bool ble_nus_multi_client_is_discovered(struct bt_conn *conn)
{
    if (!conn) {
        return false;
    }
    
    k_mutex_lock(&nus_mutex, K_FOREVER);
    
    nus_connection_t *nus_conn = find_nus_connection(conn);
    bool discovered = nus_conn ? nus_conn->discovered : false;
    
    k_mutex_unlock(&nus_mutex);
    return discovered;
}

int ble_nus_multi_client_exchange_mtu(struct bt_conn *conn)
{
    if (!conn) {
        return -EINVAL;
    }
    
    static struct bt_gatt_exchange_params exchange_params = {
        .func = exchange_func,
    };
    
    LOG_INF("Requesting MTU exchange for connection %p", conn);
    return bt_gatt_exchange_mtu(conn, &exchange_params);
}

void ble_nus_multi_client_register_data_received_cb(ble_nus_multi_data_received_cb_t cb)
{
    data_received_cb = cb;
}

void ble_nus_multi_client_register_discovery_complete_cb(ble_nus_multi_discovery_complete_cb_t cb)
{
    discovery_complete_cb = cb;
}

void ble_nus_multi_client_register_mtu_exchange_cb(ble_nus_multi_mtu_exchange_cb_t cb)
{
    mtu_exchange_cb = cb;
}

int ble_nus_multi_client_get_connection_count(void)
{
    int count = 0;
    
    k_mutex_lock(&nus_mutex, K_FOREVER);
    
    for (int i = 0; i < MAX_NUS_CONNECTIONS; i++) {
        if (nus_connections[i].active) {
            count++;
        }
    }
    
    k_mutex_unlock(&nus_mutex);
    return count;
}

/* Discovery queue management functions */
static bool queue_discovery_request(struct bt_conn *conn)
{
    if (discovery_queue_count >= MAX_DISCOVERY_QUEUE) {
        LOG_WRN("Discovery queue full, cannot queue connection %p", conn);
        return false;
    }
    
    discovery_queue[discovery_queue_tail] = bt_conn_ref(conn);
    discovery_queue_tail = (discovery_queue_tail + 1) % MAX_DISCOVERY_QUEUE;
    discovery_queue_count++;
    
    LOG_INF("Queued discovery for connection %p (queue count: %d)", conn, discovery_queue_count);
    return true;
}

static struct bt_conn *dequeue_discovery_request(void)
{
    if (discovery_queue_count == 0) {
        return NULL;
    }
    
    struct bt_conn *conn = discovery_queue[discovery_queue_head];
    discovery_queue[discovery_queue_head] = NULL;
    discovery_queue_head = (discovery_queue_head + 1) % MAX_DISCOVERY_QUEUE;
    discovery_queue_count--;
    
    LOG_INF("Dequeued discovery for connection %p (queue count: %d)", conn, discovery_queue_count);
    return conn;
}

static void process_next_discovery(void)
{
    if (discovery_in_progress) {
        LOG_DBG("Discovery still in progress, waiting");
        return;
    }
    
    struct bt_conn *conn = dequeue_discovery_request();
    if (conn) {
        LOG_INF("Processing next queued discovery for connection %p", conn);
        start_discovery_for_connection(conn);
        bt_conn_unref(conn); /* Release the reference from queue */
    }
}

static void start_discovery_for_connection(struct bt_conn *conn)
{
    k_mutex_lock(&nus_mutex, K_FOREVER);
    
    nus_connection_t *nus_conn = find_nus_connection(conn);
    if (!nus_conn) {
        LOG_ERR("NUS connection not found for discovery %p", conn);
        k_mutex_unlock(&nus_mutex);
        return;
    }
    
    LOG_INF("Starting NUS discovery for connection %p", conn);
    discovery_in_progress = true;
    
    int err = bt_gatt_dm_start(conn, BT_UUID_NUS_SERVICE, &multi_discovery_cb, nus_conn);
    if (err) {
        LOG_ERR("Could not start NUS discovery for connection %p: %d", conn, err);
        discovery_in_progress = false;
        k_mutex_unlock(&nus_mutex);
        process_next_discovery(); /* Try next in queue */
    } else {
        k_mutex_unlock(&nus_mutex);
    }
}
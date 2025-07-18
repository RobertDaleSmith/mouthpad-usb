// BLE UART Central implementation
#include "ble_uart_c.h"
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>

LOG_MODULE_REGISTER(ble_uart_c, LOG_LEVEL_INF);

// Nordic UART Service UUID
#define BT_UUID_NUS_VAL \
    BT_UUID_128_ENCODE(0x6e400001, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

// Nordic UART TX Characteristic UUID
#define BT_UUID_NUS_TX_VAL \
    BT_UUID_128_ENCODE(0x6e400003, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

// Nordic UART RX Characteristic UUID
#define BT_UUID_NUS_RX_VAL \
    BT_UUID_128_ENCODE(0x6e400002, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

static ble_uart_data_callback_t data_callback = NULL;
static ble_uart_state_t current_state = BLE_UART_STATE_DISCONNECTED;
static ble_uart_device_t current_device = {0};
static char target_name[32] = "";
static bt_addr_le_t target_address = {0};
static bool target_address_set = false;

// More conservative connection parameters for better compatibility
static const struct bt_le_conn_param conservative_conn_param = {
    .interval_min = BT_GAP_INIT_CONN_INT_MIN,  // 30ms
    .interval_max = BT_GAP_INIT_CONN_INT_MAX,  // 50ms
    .latency = 0,
    .timeout = 400,  // 400ms supervision timeout
};

// GATT discovery callbacks
static uint8_t discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            struct bt_gatt_discover_params *params)
{
    if (!attr) {
        LOG_INF("NUS service discovery complete");
        return BT_GATT_ITER_STOP;
    }

    const struct bt_gatt_service_val *gatt_service = attr->user_data;
    
    if (bt_uuid_cmp(params->uuid, BT_UUID_DECLARE_16(BT_UUID_NUS_VAL)) == 0) {
        LOG_INF("NUS service found");
        current_device.nus_service_handle = attr->handle;
        
        // Start discovering NUS TX characteristic
        struct bt_gatt_discover_params *discover_params = 
            (struct bt_gatt_discover_params *)params;
        discover_params->uuid = BT_UUID_DECLARE_16(BT_UUID_NUS_TX_VAL);
        discover_params->start_handle = gatt_service->start_handle;
        discover_params->end_handle = gatt_service->end_handle;
        discover_params->type = BT_GATT_DISCOVER_CHARACTERISTIC;
        
        return bt_gatt_discover(conn, discover_params);
    }
    
    if (bt_uuid_cmp(params->uuid, BT_UUID_DECLARE_16(BT_UUID_NUS_TX_VAL)) == 0) {
        const struct bt_gatt_chrc *chrc = attr->user_data;
        LOG_INF("NUS TX characteristic found");
        current_device.nus_tx_handle = chrc->value_handle;
        current_device.nus_tx_cccd_handle = chrc->value_handle + 1;
        
        // Enable notifications for TX
        struct bt_gatt_subscribe_params *subscribe_params = 
            (struct bt_gatt_subscribe_params *)params;
        subscribe_params->value_handle = chrc->value_handle;
        subscribe_params->ccc_handle = chrc->value_handle + 1;
        subscribe_params->value = BT_GATT_CCC_NOTIFY;
        subscribe_params->notify = nus_tx_notify_callback;
        
        return bt_gatt_subscribe(conn, subscribe_params);
    }
    
    return BT_GATT_ITER_STOP;
}

// NUS TX notification callback
static uint8_t nus_tx_notify_callback(struct bt_conn *conn,
                                     struct bt_gatt_subscribe_params *params,
                                     const void *data, uint16_t length)
{
    if (!data) {
        LOG_INF("NUS TX notifications disabled");
        current_device.notifications_enabled = false;
        return BT_GATT_ITER_STOP;
    }
    
    LOG_INF("NUS TX data received: %d bytes", length);
    current_device.notifications_enabled = true;
    
    if (data_callback) {
        data_callback((const uint8_t *)data, length);
    }
    
    return BT_GATT_ITER_CONTINUE;
}

// Connection callback
static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("Connection failed: %d (0x%02x)", err, err);
        
        // Handle specific error codes
        switch (err) {
            case BT_HCI_ERR_CONN_FAIL_TO_ESTAB:
                LOG_ERR("Connection failed to establish - parameters may be unacceptable");
                break;
            case BT_HCI_ERR_CONN_TIMEOUT:
                LOG_ERR("Connection timeout - device may be out of range");
                break;
            case BT_HCI_ERR_UNACCEPT_CONN_PARAM:
                LOG_ERR("Connection parameters unacceptable to peer");
                break;
            default:
                LOG_ERR("Unknown connection error");
                break;
        }
        
        current_state = BLE_UART_STATE_ERROR;
        return;
    }
    
    LOG_INF("Connected to NUS device");
    current_device.conn = bt_conn_ref(conn);
    current_state = BLE_UART_STATE_CONNECTED;
    
    // Start service discovery
    struct bt_gatt_discover_params *discover_params = 
        (struct bt_gatt_discover_params *)k_malloc(sizeof(*discover_params));
    discover_params->uuid = BT_UUID_DECLARE_16(BT_UUID_NUS_VAL);
    discover_params->start_handle = BT_ATT_FIRST_ATTR_HANDLE;
    discover_params->end_handle = BT_ATT_LAST_ATTR_HANDLE;
    discover_params->type = BT_GATT_DISCOVER_PRIMARY;
    discover_params->func = discover_func;
    
    bt_gatt_discover(conn, discover_params);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("Disconnected: %d (0x%02x)", reason, reason);
    
    // Handle specific disconnect reasons
    switch (reason) {
        case BT_HCI_ERR_REMOTE_USER_TERM_CONN:
            LOG_INF("Remote device terminated connection");
            break;
        case BT_HCI_ERR_LOCALHOST_TERM_CONN:
            LOG_INF("Local host terminated connection");
            break;
        case BT_HCI_ERR_CONN_TIMEOUT:
            LOG_INF("Connection supervision timeout");
            break;
        default:
            LOG_INF("Other disconnect reason");
            break;
    }
    
    current_state = BLE_UART_STATE_DISCONNECTED;
    if (current_device.conn) {
        bt_conn_unref(current_device.conn);
        current_device.conn = NULL;
    }
}

static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
};

// Scan callback
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
                   struct net_buf_simple *buf)
{
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
    
    LOG_INF("Found device: %s", addr_str);
    
    // Check if this is our target device
    if (target_address_set && bt_addr_le_cmp(addr, &target_address) == 0) {
        LOG_INF("Found target NUS device");
        bt_le_scan_stop();
        
        // Use conservative connection parameters
        int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, 
                                   &conservative_conn_param, &current_device.conn);
        if (err) {
            LOG_ERR("Failed to initiate connection: %d", err);
        }
    }
}

void ble_uart_c_init(ble_uart_data_callback_t callback)
{
    data_callback = callback;
    bt_conn_cb_register(&conn_callbacks);
    LOG_INF("BLE UART client initialized");
}

void ble_uart_c_start_scan(void)
{
    current_state = BLE_UART_STATE_DISCOVERING;
    LOG_INF("Starting NUS device scan");
    
    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_PASSIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };
    
    bt_le_scan_start(&scan_param, scan_cb);
}

void ble_uart_c_stop_scan(void)
{
    bt_le_scan_stop();
    LOG_INF("NUS device scan stopped");
}

bool ble_uart_c_connect(const bt_addr_le_t *addr)
{
    if (!addr) {
        return false;
    }
    
    LOG_INF("Connecting to NUS device");
    
    // Use conservative connection parameters
    int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, 
                               &conservative_conn_param, &current_device.conn);
    if (err) {
        LOG_ERR("Failed to initiate connection: %d", err);
        return false;
    }
    return true;
}

void ble_uart_c_disconnect(void)
{
    if (current_device.conn) {
        bt_conn_disconnect(current_device.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

ble_uart_state_t ble_uart_c_get_state(void)
{
    return current_state;
}

bool ble_uart_c_is_connected(void)
{
    return current_state == BLE_UART_STATE_CONNECTED;
}

const ble_uart_device_t* ble_uart_c_get_device(void)
{
    return &current_device;
}

bool ble_uart_c_send_data(const uint8_t *data, uint16_t len)
{
    if (!current_device.conn || !current_device.nus_rx_handle) {
        return false;
    }
    
    struct bt_gatt_write_params write_params = {
        .func = NULL,
        .handle = current_device.nus_rx_handle,
        .offset = 0,
        .data = data,
        .length = len,
    };
    
    return bt_gatt_write(current_device.conn, &write_params) == 0;
}

void ble_uart_c_on_ble_evt(struct bt_conn *conn, uint8_t evt_type, struct bt_gatt_attr *attr, void *buf)
{
    // Handle BLE events if needed
}

void ble_uart_c_set_target_name(const char *name)
{
    strncpy(target_name, name, sizeof(target_name) - 1);
    target_name[sizeof(target_name) - 1] = '\0';
}

void ble_uart_c_set_target_address(const bt_addr_le_t *addr)
{
    if (addr) {
        bt_addr_le_copy(&target_address, addr);
        target_address_set = true;
    }
}
// BLE HID Central implementation
#include "ble_hid_c.h"
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>

LOG_MODULE_REGISTER(ble_hid_c, LOG_LEVEL_INF);

// Use the standard HID UUIDs from Zephyr
// HID Service UUID is already defined as BT_UUID_HIDS_VAL (0x1812)
// HID Report Characteristic UUID is already defined as BT_UUID_HIDS_REPORT_VAL (0x2a4d)

static ble_hid_report_callback_t report_callback = NULL;
static ble_hid_state_t current_state = BLE_HID_STATE_DISCONNECTED;
static ble_hid_device_t current_device = {0};
static char target_name[32] = "";
static bt_addr_le_t target_address = {0};
static bool target_address_set = false;

// Static allocation for GATT discovery parameters
static struct bt_gatt_discover_params discover_params;

// Service discovery timeout timer
static struct k_timer discovery_timeout_timer;
static void discovery_timeout_handler(struct k_timer *dummy)
{
    LOG_WRN("HID service discovery timeout - aborting discovery");
    // Note: bt_gatt_discover_cancel is not available in this Zephyr version
    // The discovery will timeout naturally
}

// Connection parameters removed - main.c handles connection parameters
// static const struct bt_le_conn_param conservative_conn_param = { ... };

// Forward declaration of notification callback
static uint8_t hid_notify_callback(struct bt_conn *conn,
                                  struct bt_gatt_subscribe_params *params,
                                  const void *data, uint16_t length);

// GATT discovery callbacks
static uint8_t discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            struct bt_gatt_discover_params *params)
{
    if (!attr) {
        LOG_INF("HID service discovery complete");
        k_timer_stop(&discovery_timeout_timer);
        return BT_GATT_ITER_STOP;
    }

    const struct bt_gatt_service_val *gatt_service = attr->user_data;
    
    if (bt_uuid_cmp(params->uuid, BT_UUID_DECLARE_16(BT_UUID_HIDS_VAL)) == 0) {
        LOG_INF("HID service found at handle %u", attr->handle);
        current_device.hid_service_handle = attr->handle;
        
        // Start discovering HID report characteristic
        struct bt_gatt_discover_params *discover_params = 
            (struct bt_gatt_discover_params *)params;
        discover_params->uuid = BT_UUID_DECLARE_16(BT_UUID_HIDS_REPORT_VAL);
        discover_params->start_handle = attr->handle + 1;
        discover_params->end_handle = gatt_service->end_handle;
        discover_params->type = BT_GATT_DISCOVER_CHARACTERISTIC;
        
        return bt_gatt_discover(conn, discover_params);
    }
    
    if (bt_uuid_cmp(params->uuid, BT_UUID_DECLARE_16(BT_UUID_HIDS_REPORT_VAL)) == 0) {
        const struct bt_gatt_chrc *chrc = attr->user_data;
        LOG_INF("HID report characteristic found at handle %u", attr->handle);
        current_device.hid_report_handle = chrc->value_handle;
        current_device.hid_report_cccd_handle = chrc->value_handle + 1;
        
        // Enable notifications
        struct bt_gatt_subscribe_params *subscribe_params = 
            (struct bt_gatt_subscribe_params *)params;
        subscribe_params->value_handle = chrc->value_handle;
        subscribe_params->ccc_handle = chrc->value_handle + 1;
        subscribe_params->value = BT_GATT_CCC_NOTIFY;
        subscribe_params->notify = hid_notify_callback;
        
        return bt_gatt_subscribe(conn, subscribe_params);
    }
    
    return BT_GATT_ITER_STOP;
}

// HID notification callback
static uint8_t hid_notify_callback(struct bt_conn *conn,
                                  struct bt_gatt_subscribe_params *params,
                                  const void *data, uint16_t length)
{
    if (!data) {
        LOG_INF("HID notifications disabled");
        current_device.notifications_enabled = false;
        return BT_GATT_ITER_STOP;
    }
    
    LOG_INF("HID report received: %d bytes", length);
    current_device.notifications_enabled = true;
    
    // Log the raw data as hex for debugging
    LOG_INF("HID: Raw data: ");
    for (int i = 0; i < length && i < 16; i++) {
        LOG_INF("%02X ", ((uint8_t *)data)[i]);
    }
    if (length > 16) {
        LOG_INF("...");
    }
    LOG_INF("");
    
    if (report_callback) {
        report_callback((const uint8_t *)data, length);
    }
    
    return BT_GATT_ITER_CONTINUE;
}

// Connection callback - removed (now handled by main.c)
// static void connected(struct bt_conn *conn, uint8_t err) { ... }

// Disconnect callback - removed (now handled by main.c)
// static void disconnected(struct bt_conn *conn, uint8_t reason) { ... }

// Connection callbacks removed - main.c handles connections
// static struct bt_conn_cb conn_callbacks = {
//     .connected = connected,
//     .disconnected = disconnected,
// };

// Scan callback - removed connection creation logic
// Connection is now handled by main.c
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
                   struct net_buf_simple *buf)
{
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
    
    LOG_INF("Found device: %s", addr_str);
    
    // Check if this is our target device
    if (target_address_set && bt_addr_le_cmp(addr, &target_address) == 0) {
        LOG_INF("Found target HID device");
        // Connection will be handled by main.c
    }
}

void ble_hid_c_init(ble_hid_report_callback_t callback)
{
    report_callback = callback;
    // Connection callbacks are now handled by main.c
    // bt_conn_cb_register(&conn_callbacks);
    
    // Initialize discovery timeout timer
    k_timer_init(&discovery_timeout_timer, discovery_timeout_handler, NULL);
    
    LOG_INF("BLE HID client initialized");
}

void ble_hid_c_start_scan(void)
{
    current_state = BLE_HID_STATE_DISCOVERING;
    LOG_INF("Starting HID device scan");
    
    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_PASSIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };
    
    bt_le_scan_start(&scan_param, scan_cb);
}

void ble_hid_c_stop_scan(void)
{
    bt_le_scan_stop();
    LOG_INF("HID device scan stopped");
}

// Connection function removed - main.c handles connections
// bool ble_hid_c_connect(const bt_addr_le_t *addr) { ... }

void ble_hid_c_disconnect(void)
{
    if (current_device.conn) {
        bt_conn_disconnect(current_device.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

ble_hid_state_t ble_hid_c_get_state(void)
{
    return current_state;
}

bool ble_hid_c_is_connected(void)
{
    return current_state == BLE_HID_STATE_CONNECTED;
}

const ble_hid_device_t* ble_hid_c_get_device(void)
{
    return &current_device;
}

void ble_hid_c_on_ble_evt(struct bt_conn *conn, uint8_t evt_type, struct bt_gatt_attr *attr, void *buf)
{
    // Handle BLE events if needed
}

void ble_hid_c_set_target_name(const char *name)
{
    strncpy(target_name, name, sizeof(target_name) - 1);
    target_name[sizeof(target_name) - 1] = '\0';
}

void ble_hid_c_set_target_address(const bt_addr_le_t *addr)
{
    if (addr) {
        bt_addr_le_copy(&target_address, addr);
        target_address_set = true;
    }
}

// Function to handle connection established by main.c
void ble_hid_c_on_connected(struct bt_conn *conn)
{
    LOG_INF("HID client notified of connection");
    current_device.conn = bt_conn_ref(conn);
    current_state = BLE_HID_STATE_CONNECTED;
    
    // Start service discovery with timeout
    discover_params.uuid = BT_UUID_DECLARE_16(BT_UUID_HIDS_VAL);
    discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discover_params.type = BT_GATT_DISCOVER_PRIMARY;
    discover_params.func = discover_func;
    
    int discover_err = bt_gatt_discover(conn, &discover_params);
    if (discover_err) {
        LOG_ERR("Failed to start HID service discovery: %d", discover_err);
        return;
    }
    
    // Start discovery timeout timer (5 seconds)
    k_timer_start(&discovery_timeout_timer, K_SECONDS(5), K_NO_WAIT);
}

// Function to handle disconnection from main.c
void ble_hid_c_on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("HID client notified of disconnection: %d (0x%02x)", reason, reason);
    
    // Stop discovery timeout timer
    k_timer_stop(&discovery_timeout_timer);
    
    current_state = BLE_HID_STATE_DISCONNECTED;
    if (current_device.conn) {
        bt_conn_unref(current_device.conn);
        current_device.conn = NULL;
    }
}
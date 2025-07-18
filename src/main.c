#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include "ble_config.h"
#include "ble_hid_c.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* LED0 is the blue LED */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

// BLE connection state
static struct bt_conn *current_conn = NULL;
static bool ble_connected = false;
static int scan_count = 0;
static int connection_attempts = 0;
static const int max_connection_attempts = MAX_CONNECTION_ATTEMPTS;

// Connection parameters - using defaults for initial connection
// static const struct bt_le_conn_param conservative_conn_param = { ... };

// Better connection parameters to request after connection
static const struct bt_le_conn_param preferred_conn_param = {
    .interval_min = 24,   // 30ms (24 * 1.25ms)
    .interval_max = 40,   // 50ms (40 * 1.25ms)
    .latency = 0,
    .timeout = 4000,      // 4s supervision timeout for better stability
};

// Forward declarations
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
                   struct net_buf_simple *buf);

// Connection parameter optimization timer
static struct k_timer conn_param_timer;
static void conn_param_timer_handler(struct k_timer *dummy);

// HID report callback
static void hid_report_callback(const uint8_t *data, uint16_t len);

// Simple BLE connection callback
static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("BLE: Connection failed: %d (0x%02x)\n", err, err);
        
        // Handle specific error codes
        switch (err) {
            case BT_HCI_ERR_CONN_FAIL_TO_ESTAB:
                printk("BLE: Connection failed to establish - parameters may be unacceptable\n");
                break;
            case BT_HCI_ERR_CONN_TIMEOUT:
                printk("BLE: Connection timeout - device may be out of range\n");
                break;
            case BT_HCI_ERR_UNACCEPT_CONN_PARAM:
                printk("BLE: Connection parameters unacceptable to peer\n");
                break;
            default:
                printk("BLE: Unknown connection error\n");
                break;
        }
        
        connection_attempts++;
        if (connection_attempts >= max_connection_attempts) {
            printk("BLE: Max connection attempts reached, restarting scan\n");
            connection_attempts = 0;
            // Restart scanning after a delay
            k_sleep(K_SECONDS(2));
            bt_le_scan_start(NULL, scan_cb);
        }
        return;
    }
    
    current_conn = bt_conn_ref(conn);
    ble_connected = true;
    connection_attempts = 0;  // Reset attempts on successful connection
    printk("BLE: Device connected successfully!\n");
    
    // Notify HID client of connection
    ble_hid_c_on_connected(conn);
    printk("BLE: HID client notified of connection\n");
    
    // Request better connection parameters after connection
    printk("BLE: Requesting optimized connection parameters...\n");
    int param_err = bt_conn_le_param_update(conn, &preferred_conn_param);
    if (param_err) {
        printk("BLE: Failed to request connection parameter update: %d\n", param_err);
    } else {
        printk("BLE: Connection parameter update request sent\n");
    }
    
    // Start periodic connection parameter optimization (less frequent)
    k_timer_start(&conn_param_timer, K_SECONDS(10), K_SECONDS(30));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("BLE: Device disconnected, reason: %d (0x%02x)\n", reason, reason);
    
    // Handle specific disconnect reasons
    switch (reason) {
        case BT_HCI_ERR_REMOTE_USER_TERM_CONN:
            printk("BLE: Remote device terminated connection\n");
            break;
        case BT_HCI_ERR_LOCALHOST_TERM_CONN:
            printk("BLE: Local host terminated connection\n");
            break;
        case BT_HCI_ERR_CONN_TIMEOUT:
            printk("BLE: Connection supervision timeout\n");
            break;
        default:
            printk("BLE: Other disconnect reason\n");
            break;
    }
    
    // Notify HID client of disconnection
    ble_hid_c_on_disconnected(conn, reason);
    
    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
    ble_connected = false;
    
    // Stop the connection parameter optimization timer
    k_timer_stop(&conn_param_timer);
    
    printk("BLE: DEBUG - Restarting scan after disconnection\n");
    
    // Restart scanning after a short delay
    k_sleep(K_MSEC(500));
    bt_le_scan_start(NULL, scan_cb);
}

// Connection parameter update request callback
static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
    printk("BLE: Connection parameter update request\n");
    printk("BLE: Requested - Interval: %d-%d, Latency: %d, Timeout: %d\n",
           param->interval_min, param->interval_max, param->latency, param->timeout);
    
    // Accept the parameters but ensure they're within reasonable bounds
    if (param->interval_min < 6) {  // Minimum 7.5ms
        param->interval_min = 6;
        printk("BLE: Adjusted interval_min to 6\n");
    }
    if (param->interval_max > 3200) {  // Maximum 4s
        param->interval_max = 3200;
        printk("BLE: Adjusted interval_max to 3200\n");
    }
    if (param->timeout < 400) {  // Minimum 400ms supervision timeout
        param->timeout = 400;
        printk("BLE: Adjusted timeout to 400ms minimum\n");
    }
    if (param->timeout > 3200) {  // Maximum 4s supervision timeout
        param->timeout = 3200;
        printk("BLE: Adjusted timeout to 3200ms maximum\n");
    }
    
    printk("BLE: Final parameters - Interval: %d-%d, Latency: %d, Timeout: %d\n",
           param->interval_min, param->interval_max, param->latency, param->timeout);
    printk("BLE: Accepting connection parameters\n");
    return true;
}

// Connection parameter updated callback
static void le_param_updated(struct bt_conn *conn, uint16_t interval,
                            uint16_t latency, uint16_t timeout)
{
    printk("BLE: Connection parameters updated\n");
    printk("BLE: New interval: %d, latency: %d, timeout: %d\n",
           interval, latency, timeout);
}

// Connection parameter optimization timer handler
static void conn_param_timer_handler(struct k_timer *dummy)
{
    if (current_conn && ble_connected) {
        printk("BLE: Periodic connection parameter optimization...\n");
        int err = bt_conn_le_param_update(current_conn, &preferred_conn_param);
        if (err) {
            printk("BLE: Periodic parameter update failed: %d\n", err);
        }
    }
}

// DFU command - enters bootloader mode
static int cmd_dfu(const struct shell *shell, size_t argc, char *argv[])
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    
    shell_print(shell, "Entering DFU mode...\n");
    shell_print(shell, "Device will restart in bootloader mode.\n");
    shell_print(shell, "You can now run 'make flash' to update firmware.\n");
    
    // Small delay to ensure the message is printed
    k_sleep(K_MSEC(100));
    
    // For Nordic devices, try multiple methods to trigger DFU mode
    // Method 1: Set GPREGRET with different magic values
    NRF_POWER->GPREGRET = 0xB1;  // Common Nordic DFU magic value
    
    // Method 2: Also try setting GPREGRET2 (if available)
    #ifdef NRF_POWER_GPREGRET2_Msk
    NRF_POWER->GPREGRET2 = 0xB1;
    #endif
    
    // Method 3: Set a different magic value that some bootloaders use
    NRF_POWER->GPREGRET = 0xFE;  // Alternative magic value
    
    shell_print(shell, "DFU mode flags set, rebooting...\n");
    
    // Small delay to ensure the message is printed
    k_sleep(K_MSEC(100));
    
    // Reboot to enter bootloader mode
    sys_reboot(SYS_REBOOT_COLD);
    
    return 0;
}

// HID report callback - called when HID data is received
static void hid_report_callback(const uint8_t *data, uint16_t len)
{
    printk("HID: Report received - %d bytes\n", len);
    
    // Log the raw data as hex
    printk("HID: Data: ");
    for (int i = 0; i < len && i < 16; i++) {  // Limit to first 16 bytes for readability
        printk("%02X ", data[i]);
    }
    if (len > 16) {
        printk("...");
    }
    printk("\n");
    
    // Try to interpret as common HID report types
    if (len >= 1) {
        uint8_t report_id = data[0];
        printk("HID: Report ID: 0x%02X\n", report_id);
        
        // Common HID report interpretations
        if (len >= 2) {
            switch (report_id) {
                case 0x01:  // Mouse report
                    if (len >= 4) {
                        int8_t x = (int8_t)data[1];
                        int8_t y = (int8_t)data[2];
                        uint8_t buttons = data[3];
                        printk("HID: Mouse - X:%d, Y:%d, Buttons:0x%02X\n", x, y, buttons);
                    }
                    break;
                    
                case 0x02:  // Keyboard report
                    if (len >= 8) {
                        uint8_t modifiers = data[1];
                        uint8_t keys[6];
                        memcpy(keys, &data[3], 6);
                        printk("HID: Keyboard - Modifiers:0x%02X, Keys:[%02X %02X %02X %02X %02X %02X]\n",
                               modifiers, keys[0], keys[1], keys[2], keys[3], keys[4], keys[5]);
                    }
                    break;
                    
                default:
                    printk("HID: Unknown report type\n");
                    break;
            }
        }
    }
}

static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
    .le_param_req = le_param_req,
    .le_param_updated = le_param_updated,
};

// Shell command registration
SHELL_CMD_ARG_REGISTER(dfu, NULL, "Enter DFU mode (bootloader)", cmd_dfu, 0, 0);

// Device filtering options
static bool target_specific_device = false;
static bt_addr_le_t target_device_addr = {0};
static char target_device_name[32] = {0};

// MouthPad detection function
static bool is_mouthpad_device(const char *device_name)
{
    if (!device_name) return false;
    
    // Check for MouthPad patterns (case-insensitive using multiple variations)
    if (strstr(device_name, "mouthpad") || strstr(device_name, "MouthPad") ||
        strstr(device_name, "MOUTHPAD") || strstr(device_name, "mouthPad") ||
        strstr(device_name, "Mouthpad") || strstr(device_name, "MOUTHPAD")) {
        return true;
    }
    
    // Check for space-separated variations
    if (strstr(device_name, "mouth pad") || strstr(device_name, "Mouth Pad") ||
        strstr(device_name, "MOUTH PAD") || strstr(device_name, "Mouth pad")) {
        return true;
    }
    
    // Check for hyphenated variations
    if (strstr(device_name, "mouth-pad") || strstr(device_name, "Mouth-Pad") ||
        strstr(device_name, "MOUTH-PAD") || strstr(device_name, "Mouth-pad")) {
        return true;
    }
    
    // Check for underscore variations
    if (strstr(device_name, "mouth_pad") || strstr(device_name, "Mouth_Pad") ||
        strstr(device_name, "MOUTH_PAD") || strstr(device_name, "Mouth_pad")) {
        return true;
    }
    
    // Check for RDS MouthPad variations
    if (strstr(device_name, "rdsmouthpad") || strstr(device_name, "RDSMouthPad") ||
        strstr(device_name, "RDSMOUTHPAD") || strstr(device_name, "RdsMouthPad")) {
        return true;
    }
    
    // Check for RDS space-separated variations
    if (strstr(device_name, "rds mouthpad") || strstr(device_name, "RDS MouthPad") ||
        strstr(device_name, "RDS MOUTHPAD") || strstr(device_name, "Rds MouthPad")) {
        return true;
    }
    
    return false;
}

// Simple scan callback
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
                   struct net_buf_simple *buf)
{
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
    
    // Parse advertising data to get device name and services
    char device_name[32] = "Unknown";
    bool has_hid_service = false;
    bool has_uart_service = false;
    
    // Parse advertising data
    struct net_buf_simple_state state;
    net_buf_simple_save(buf, &state);
    
    while (buf->len > 1) {
        uint8_t len = net_buf_simple_pull_u8(buf);
        if (len == 0) break;
        
        uint8_t type = net_buf_simple_pull_u8(buf);
        len--;
        
        switch (type) {
            case BT_DATA_NAME_SHORTENED:
            case BT_DATA_NAME_COMPLETE:
                if (len <= sizeof(device_name) - 1) {
                    memcpy(device_name, net_buf_simple_pull_mem(buf, len), len);
                    device_name[len] = '\0';
                }
                break;
            case BT_DATA_UUID16_ALL:
            case BT_DATA_UUID16_SOME:
                for (int i = 0; i < len; i += 2) {
                    uint16_t uuid = net_buf_simple_pull_le16(buf);
                    if (uuid == BT_UUID_HIDS_VAL) {
                        has_hid_service = true;
                    }
                    // Check for Nordic UART Service (0x6E400001)
                    if (uuid == 0x6E40) {
                        has_uart_service = true;
                    }
                }
                break;
            default:
                net_buf_simple_pull_mem(buf, len);
                break;
        }
    }
    
    net_buf_simple_restore(buf, &state);
    
    // Only process devices that have HID service
    if (!has_hid_service) {
        return; // Skip non-HID devices (no debug output for non-HID)
    }
    
    scan_count++;
    
    // Debug: Show HID devices found
    printk("BLE: DEBUG - Found HID device: %s, Name: %s, UART: %s\n", 
           addr_str, device_name, has_uart_service ? "YES" : "NO");
    
    // Print detailed device information for HID devices only
    printk("BLE: HID Device #%d: %s\n", scan_count, addr_str);
    printk("BLE:   Name: %s\n", device_name);
    printk("BLE:   RSSI: %d\n", rssi);
    printk("BLE:   Services: HID=YES, UART=%s\n", 
           has_uart_service ? "YES" : "NO");
    
    // Debug MouthPad detection
    bool is_mouthpad = is_mouthpad_device(device_name);
    printk("BLE: DEBUG - MouthPad detection: %s\n", is_mouthpad ? "YES" : "NO");
    
    // Check if this matches our target criteria
    bool should_connect = false;
    
    if (target_specific_device) {
        // Connect to specific target device
        if (bt_addr_le_cmp(addr, &target_device_addr) == 0) {
            should_connect = true;
            printk("BLE: Found target device!\n");
        }
    } else {
        // Priority 1: Auto-connect to MouthPad devices (highest priority)
        if (is_mouthpad) {
            should_connect = true;
            printk("BLE: Found MouthPad device - attempting connection\n");
        }
        // Priority 2: Auto-connect to HID devices with UART features
        else if (has_hid_service && has_uart_service) {
            should_connect = true;
            printk("BLE: Found HID+UART device - attempting connection\n");
        }
        // Priority 3: Enhanced name pattern matching for other HID devices
        else if (strstr(device_name, "mouse") || strstr(device_name, "Mouse") ||
                 strstr(device_name, "HID") || strstr(device_name, "hid") ||
                 strstr(device_name, "pad") || strstr(device_name, "Pad") ||
                 strstr(device_name, "RDS") || strstr(device_name, "rds") ||
                 strstr(device_name, "Mouth") || strstr(device_name, "mouth")) {
            should_connect = true;
            printk("BLE: Found HID device by name pattern - attempting connection\n");
        }
    }
    
    printk("BLE: DEBUG - Should connect: %s, Already connected: %s, Attempts: %d/%d\n",
           should_connect ? "YES" : "NO", ble_connected ? "YES" : "NO", 
           connection_attempts, max_connection_attempts);
    
    // Attempt connection if criteria met
    if (should_connect && !ble_connected && connection_attempts < max_connection_attempts) {
        printk("BLE: Attempting to connect to %s (attempt %d/%d)\n", 
               device_name, connection_attempts + 1, max_connection_attempts);
        
        // Stop scanning first
        bt_le_scan_stop();
        printk("BLE: Scanning stopped\n");
        
        // Small delay to ensure scanning is fully stopped
        k_sleep(K_MSEC(100));
        
        // Store the target address for HID client
        ble_hid_c_set_target_address(addr);
        
        // Create connection using main connection logic
        printk("BLE: Creating connection to HID device\n");
        printk("BLE: Using default connection parameters\n");
        
        int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, 
                                   NULL, &current_conn);
        if (err) {
            printk("BLE: Failed to initiate connection: %d\n", err);
            printk("BLE: Error details - addr type: %d, addr: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   addr->type, addr->a.val[5], addr->a.val[4], addr->a.val[3],
                   addr->a.val[2], addr->a.val[1], addr->a.val[0]);
            connection_attempts++;
            // Restart scanning after a delay
            k_sleep(K_SECONDS(1));
            bt_le_scan_start(NULL, scan_cb);
        } else {
            printk("BLE: Connection initiated successfully\n");
        }
    }
}

// Function to set target device (can be called from shell or config)
void ble_set_target_device(const char *addr_str, const char *name)
{
    if (addr_str && strlen(addr_str) > 0) {
        bt_addr_le_from_str(addr_str, BT_ADDR_LE_PUBLIC, &target_device_addr);
        target_specific_device = true;
        printk("BLE: Target device set to %s\n", addr_str);
    }
    
    if (name && strlen(name) > 0) {
        strncpy(target_device_name, name, sizeof(target_device_name) - 1);
        target_device_name[sizeof(target_device_name) - 1] = '\0';
        printk("BLE: Looking for device named: %s\n", name);
    }
}

void main(void)
{
    int ret;
    int counter = 0;
    
    printk("=== BLE-USB Bridge Starting ===\n");
    
    /* Initialize LED */
    if (!gpio_is_ready_dt(&led)) {
        printk("ERROR: LED device not ready\n");
        return;
    }
    
    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        printk("ERROR: Failed to configure LED: %d\n", ret);
        return;
    }
    
    printk("LED initialized successfully\n");
    
    // Initialize Bluetooth
    ret = bt_enable(NULL);
    if (ret) {
        printk("ERROR: Bluetooth init failed: %d\n", ret);
        return;
    }
    
    printk("Bluetooth initialized successfully\n");
    
    // Register connection callbacks
    bt_conn_cb_register(&conn_callbacks);
    
    // Initialize connection parameter optimization timer
    k_timer_init(&conn_param_timer, conn_param_timer_handler, NULL);
    
    // Initialize HID client
    ble_hid_c_init(hid_report_callback);
    
    // Initialize settings
    settings_subsys_init();
    settings_load();
    
    // Initialize target device based on configuration
#ifdef TARGET_DEVICE_ADDR
    ble_set_target_device(TARGET_DEVICE_ADDR, NULL);
    printk("BLE: Configured to target device: %s\n", TARGET_DEVICE_ADDR);
#elif defined(TARGET_DEVICE_NAME)
    ble_set_target_device(NULL, TARGET_DEVICE_NAME);
    printk("BLE: Configured to target device named: %s\n", TARGET_DEVICE_NAME);
#elif defined(AUTO_CONNECT_MOUTHPAD)
    printk("BLE: Auto-connecting to MouthPad devices (recommended for distribution)\n");
#elif defined(AUTO_CONNECT_HID_UART)
    printk("BLE: Auto-connecting to HID+UART devices\n");
#else
    printk("BLE: No target device configured - will connect to any HID device\n");
#endif
    
    printk("All components initialized\n");
    
    // Start scanning for BLE devices with more conservative parameters
    printk("Starting BLE device scan (HID devices only)...\n");
    
    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_PASSIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };
    
    ret = bt_le_scan_start(&scan_param, scan_cb);
    if (ret) {
        printk("ERROR: Failed to start scan: %d\n", ret);
        return;
    }
    
    printk("BLE scan started successfully - looking for HID devices...\n");
    printk("BLE: Using conservative connection parameters (30-50ms interval, 800ms timeout)\n");
    
    /* Main loop - blink LED and handle events */
    while (1) {
        gpio_pin_toggle_dt(&led);
        
        // Log status every 10 seconds
        if (counter % 10 == 0) {
            ble_hid_state_t hid_state = ble_hid_c_get_state();
            const char* hid_state_str = "Unknown";
            switch (hid_state) {
                case BLE_HID_STATE_DISCONNECTED: hid_state_str = "Disconnected"; break;
                case BLE_HID_STATE_DISCOVERING: hid_state_str = "Discovering"; break;
                case BLE_HID_STATE_CONNECTED: hid_state_str = "Connected"; break;
                case BLE_HID_STATE_ERROR: hid_state_str = "Error"; break;
            }
            printk("Status: BLE=%s, HID=%s, HID_Devices=%d, Attempts=%d/%d\n", 
                   ble_connected ? "Connected" : "Scanning", 
                   hid_state_str,
                   scan_count, connection_attempts, max_connection_attempts);
        }
        
        k_sleep(K_SECONDS(1));
        counter++;
    }
}
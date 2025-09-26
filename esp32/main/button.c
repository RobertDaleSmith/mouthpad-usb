#include "button.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "BUTTON";

// Seeed XIAO ESP32-S3 user button is connected to GPIO0 (BOOT button)
#define BUTTON_GPIO_PIN         GPIO_NUM_0
#define BUTTON_ACTIVE_LEVEL     0  // Button is active low (pulls to GND when pressed)

// Button timing configuration (in milliseconds)
#define BUTTON_DEBOUNCE_TIME_MS     50
#define BUTTON_DOUBLE_CLICK_TIME_MS 500
#define BUTTON_LONG_PRESS_TIME_MS   3000

// Button state tracking
typedef struct {
    bool is_pressed;
    bool was_pressed;
    uint32_t press_start_time;
    uint32_t last_release_time;
    bool pending_single_click;
    esp_timer_handle_t double_click_timer;
    esp_timer_handle_t long_press_timer;
    button_event_callback_t callback;
} button_state_t;

static button_state_t s_button_state = {0};
static QueueHandle_t s_button_queue = NULL;

typedef struct {
    button_event_t event;
} button_queue_msg_t;

static void button_task(void *arg);
static void button_gpio_isr_handler(void *arg);
static void button_double_click_timeout(void *arg);
static void button_long_press_timeout(void *arg);
static uint32_t button_get_time_ms(void);

esp_err_t button_init(button_event_callback_t callback)
{
    if (!callback) {
        ESP_LOGE(TAG, "Button callback is required");
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize button state
    memset(&s_button_state, 0, sizeof(button_state_t));
    s_button_state.callback = callback;

    // Create button event queue
    s_button_queue = xQueueCreate(10, sizeof(button_queue_msg_t));
    if (!s_button_queue) {
        ESP_LOGE(TAG, "Failed to create button queue");
        return ESP_ERR_NO_MEM;
    }

    // Configure GPIO
    gpio_config_t gpio_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // Enable internal pull-up
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE     // Interrupt on both edges
    };
    esp_err_t ret = gpio_config(&gpio_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure button GPIO: %s", esp_err_to_name(ret));
        vQueueDelete(s_button_queue);
        return ret;
    }

    // Create double-click timer
    esp_timer_create_args_t double_click_timer_args = {
        .callback = button_double_click_timeout,
        .arg = NULL,
        .name = "button_double_click",
        .dispatch_method = ESP_TIMER_TASK
    };
    ret = esp_timer_create(&double_click_timer_args, &s_button_state.double_click_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create double-click timer: %s", esp_err_to_name(ret));
        vQueueDelete(s_button_queue);
        return ret;
    }

    // Create long-press timer
    esp_timer_create_args_t long_press_timer_args = {
        .callback = button_long_press_timeout,
        .arg = NULL,
        .name = "button_long_press",
        .dispatch_method = ESP_TIMER_TASK
    };
    ret = esp_timer_create(&long_press_timer_args, &s_button_state.long_press_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create long-press timer: %s", esp_err_to_name(ret));
        esp_timer_delete(s_button_state.double_click_timer);
        vQueueDelete(s_button_queue);
        return ret;
    }

    // Install GPIO ISR handler
    ret = gpio_install_isr_service(ESP_INTR_FLAG_EDGE);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(ret));
        esp_timer_delete(s_button_state.double_click_timer);
        esp_timer_delete(s_button_state.long_press_timer);
        vQueueDelete(s_button_queue);
        return ret;
    }

    // Add GPIO ISR handler
    ret = gpio_isr_handler_add(BUTTON_GPIO_PIN, button_gpio_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add GPIO ISR handler: %s", esp_err_to_name(ret));
        esp_timer_delete(s_button_state.double_click_timer);
        esp_timer_delete(s_button_state.long_press_timer);
        vQueueDelete(s_button_queue);
        return ret;
    }

    // Create button processing task with lower priority to avoid watchdog issues
    BaseType_t task_ret = xTaskCreate(button_task, "button_task", 3072, NULL, 3, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        gpio_isr_handler_remove(BUTTON_GPIO_PIN);
        esp_timer_delete(s_button_state.double_click_timer);
        esp_timer_delete(s_button_state.long_press_timer);
        vQueueDelete(s_button_queue);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Button module initialized (GPIO%d)", BUTTON_GPIO_PIN);
    return ESP_OK;
}

esp_err_t button_deinit(void)
{
    if (s_button_queue) {
        // Stop timers
        esp_timer_stop(s_button_state.double_click_timer);
        esp_timer_stop(s_button_state.long_press_timer);

        // Delete timers
        esp_timer_delete(s_button_state.double_click_timer);
        esp_timer_delete(s_button_state.long_press_timer);

        // Remove GPIO ISR handler
        gpio_isr_handler_remove(BUTTON_GPIO_PIN);

        // Delete queue
        vQueueDelete(s_button_queue);
        s_button_queue = NULL;

        ESP_LOGI(TAG, "Button module deinitialized");
    }

    return ESP_OK;
}

static void IRAM_ATTR button_gpio_isr_handler(void *arg)
{
    // Read current GPIO state
    // bool current_state = (gpio_get_level(BUTTON_GPIO_PIN) == BUTTON_ACTIVE_LEVEL);

    // Simple debouncing in ISR - just trigger task processing
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    button_queue_msg_t msg = {0}; // Dummy message to wake up task
    xQueueSendFromISR(s_button_queue, &msg, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

static void button_task(void *arg)
{
    button_queue_msg_t msg;
    TickType_t last_check_time = xTaskGetTickCount();

    while (1) {
        // Wait for GPIO interrupt or timeout for periodic checking (shorter timeout to avoid watchdog)
        if (xQueueReceive(s_button_queue, &msg, pdMS_TO_TICKS(5)) == pdTRUE ||
            (xTaskGetTickCount() - last_check_time) >= pdMS_TO_TICKS(5)) {

            last_check_time = xTaskGetTickCount();

            // Read current button state with debouncing
            bool current_pressed = (gpio_get_level(BUTTON_GPIO_PIN) == BUTTON_ACTIVE_LEVEL);
            uint32_t current_time = button_get_time_ms();

            // Debounce logic - use time-based debouncing instead of blocking delay
            static uint32_t last_state_change_time = 0;
            if (current_pressed != s_button_state.is_pressed) {
                if (current_time - last_state_change_time >= BUTTON_DEBOUNCE_TIME_MS) {
                    // State change confirmed after debouncing time has passed
                    s_button_state.was_pressed = s_button_state.is_pressed;
                    s_button_state.is_pressed = current_pressed;
                    last_state_change_time = current_time;

                    if (current_pressed && !s_button_state.was_pressed) {
                        // Button pressed
                        s_button_state.press_start_time = button_get_time_ms();

                        // Start long-press timer
                        esp_timer_stop(s_button_state.long_press_timer);
                        esp_timer_start_once(s_button_state.long_press_timer,
                                           BUTTON_LONG_PRESS_TIME_MS * 1000);

                    } else if (!current_pressed && s_button_state.was_pressed) {
                        // Button released
                        esp_timer_stop(s_button_state.long_press_timer);

                        uint32_t press_duration = button_get_time_ms() - s_button_state.press_start_time;

                        if (press_duration < BUTTON_LONG_PRESS_TIME_MS) {
                            // Short press - check for double-click
                            uint32_t time_since_last_release = button_get_time_ms() - s_button_state.last_release_time;

                            if (s_button_state.pending_single_click &&
                                time_since_last_release < BUTTON_DOUBLE_CLICK_TIME_MS) {
                                // Double-click detected
                                esp_timer_stop(s_button_state.double_click_timer);
                                s_button_state.pending_single_click = false;

                                if (s_button_state.callback) {
                                    s_button_state.callback(BUTTON_EVENT_DOUBLE_CLICK);
                                }
                            } else {
                                // Potential single-click - start timer to wait for double-click
                                s_button_state.pending_single_click = true;
                                esp_timer_stop(s_button_state.double_click_timer);
                                esp_timer_start_once(s_button_state.double_click_timer,
                                                   BUTTON_DOUBLE_CLICK_TIME_MS * 1000);
                            }
                        }

                        s_button_state.last_release_time = button_get_time_ms();
                    }
                }
            }
        }
    }
}

static void button_double_click_timeout(void *arg)
{
    if (s_button_state.pending_single_click) {
        s_button_state.pending_single_click = false;

        if (s_button_state.callback) {
            s_button_state.callback(BUTTON_EVENT_SINGLE_CLICK);
        }
    }
}

static void button_long_press_timeout(void *arg)
{
    if (s_button_state.is_pressed) {
        // Long-press detected
        s_button_state.pending_single_click = false;

        if (s_button_state.callback) {
            s_button_state.callback(BUTTON_EVENT_LONG_PRESS);
        }
    }
}

static uint32_t button_get_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
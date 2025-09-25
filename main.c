#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "index_html.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "config_data.h"
#include <ctype.h>
//#include "mdns.h"
//#include "cJSON.h"
#include <inttypes.h>
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include <inttypes.h>

static const char *TAG = "MAIN";
static const char *TAG_RS485_DATA = "RS485_DATA";
static const char *TAG_BTN = "BUTTON STATUS";
uint8_t panel_new_address = 0x00;
bool polling_task_started = false;
static keypad_config_t keypad_configs[MAX_KEYPADS];
uint8_t keypads_count = 0;
//static const char *BUTTON_TAG = "Button Reset";
// Store the last submitted config
keypad_config_t last_submitted_config;
bool last_config_valid = false;

TaskHandle_t polling_task = NULL;
TaskHandle_t zone_polling_task = NULL;  // Added for zone polling task
wifi_config_t wifi_config = {0};
httpd_handle_t server = NULL;
uint8_t frame[FRAME_SIZE];
uint8_t keypad_ids_Buff[MAX_KEYPADS];
uint8_t keypad_id_cnt = 0;
uint16_t last_button_states[MAX_KEYPADS][NUM_BUTTONS] = {0};
static uint16_t last_scene_values[MAX_SLAVES+1][MAX_ZONES+1] = {0};

typedef struct {
    bool active;
    TimerHandle_t timer;
    uint8_t poll_id;
    uint8_t button;
    uint32_t start_ms;
} press_state_t;

press_state_t press_states[MAX_KEYPADS][NUM_BUTTONS];

static SemaphoreHandle_t frame_mutex = NULL;
static uint8_t current_frame[FRAME_SIZE];
static bool command_pending = false;
uint8_t poll_id=0, btn1_value=0;
bool report = false;

uint8_t matched_button = 0;
uint8_t led_keypadNumber = 0;
uint8_t group_index = 0;
// Each keypad keeps its own LED state
static uint16_t led_states[MAX_KEYPADS] = {0};
SemaphoreHandle_t modbus_mutex_uart1;  // For keypads (UART1)
SemaphoreHandle_t modbus_mutex_uart2;  // For controller (UART2)
SemaphoreHandle_t polling_semaphore;
bool polling_task_ready = false;
// Frame management for keypad polling (similar to zone polling)
static SemaphoreHandle_t keypad_frame_mutex = NULL;
static uint8_t keypad_current_frame[FRAME_SIZE];
static bool keypad_command_pending = false;
// Function to calculate Modbus CRC-16
uint16_t modbus_crc16(uint8_t *data, uint8_t len) {
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

void init_uart() {
    uart_config_t uart_config = {
        .baud_rate = BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_driver_install(UART_NUM1, 256, 0, 0, NULL, 0);
    uart_param_config(UART_NUM1, &uart_config);
    uart_set_pin(UART_NUM1, TXD_PIN1, RXD_PIN1, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_set_direction(RS485_ENABLE_PIN1, GPIO_MODE_OUTPUT);
    // gpio_set_direction(RS485_WRITE_PIN1, GPIO_MODE_OUTPUT);
    // gpio_set_direction(RS485_READ_PIN1, GPIO_MODE_OUTPUT);
    RS485_READ_MODE1
}

void init_uart1() {
    uart_config_t uart_config = {
        .baud_rate = BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_driver_install(UART_NUM2, 256, 0, 0, NULL, 0);
    uart_param_config(UART_NUM2, &uart_config);
    uart_set_pin(UART_NUM2, TXD_PIN2, RXD_PIN2, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    //gpio_set_direction(RS485_ENABLE_PIN2, GPIO_MODE_OUTPUT);
    gpio_set_direction(RS485_WRITE_PIN2, GPIO_MODE_OUTPUT);
    gpio_set_direction(RS485_READ_PIN2, GPIO_MODE_OUTPUT);
    RS485_READ_MODE2
}

void initialize_frame_management(void) {
    frame_mutex = xSemaphoreCreateMutex();
    if (frame_mutex == NULL) {
        ESP_LOGE(TAG_RS485_DATA, "Failed to create frame mutex");
    }
    command_pending = false;
    memset(current_frame, 0, FRAME_SIZE);
}
void initialize_keypad_frame_management(void) {
    keypad_frame_mutex = xSemaphoreCreateMutex();
    if (keypad_frame_mutex == NULL) {
        ESP_LOGE(TAG_RS485_DATA, "Failed to create keypad frame mutex");
    }
    keypad_command_pending = false;
    memset(keypad_current_frame, 0, FRAME_SIZE);
}
static void keypad_polling_handler(void) {
    uint8_t rx_data[BUF_SIZE];
    uint8_t btnRead[8] = {0xFD, 0x03, 0x10, 0x0B, 0x00, 0x01, 0xE5, 0x34};
    uint8_t clean_data[FRAME_SIZE];

    if (!take_mutex_with_timeout(modbus_mutex_uart1, "UART1", pdMS_TO_TICKS(100))) {
        return;
    }

    RS485_WRITE_MODE1;
    vTaskDelay(pdMS_TO_TICKS(15));

    uart_write_bytes(UART_NUM1, (const char *)btnRead, FRAME_SIZE);
    uart_wait_tx_done(UART_NUM1, pdMS_TO_TICKS(50));

    RS485_READ_MODE1;
    vTaskDelay(pdMS_TO_TICKS(10));

    int len = uart_read_bytes(UART_NUM1, rx_data, sizeof(rx_data), pdMS_TO_TICKS(100));
    give_mutex(modbus_mutex_uart1, "UART1");

    if (len > 0 && clean_keypads_response(rx_data, len, clean_data, FRAME_SIZE)) {
        check_and_log_button_changes(clean_data);  // already does scene send
    }

    kick_watchdog();
}

bool system_healthy = false;
TimerHandle_t task_watchdog_timer = NULL;
// ==================== ROBUST MUTEX HANDLING ====================
bool take_mutex_with_timeout(SemaphoreHandle_t mutex, const char* mutex_name, TickType_t timeout) {
    BaseType_t result = xSemaphoreTake(mutex, timeout);
    if (result != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire %s mutex within timeout", mutex_name);
        return false;
    }
    return true;
}

void give_mutex(SemaphoreHandle_t mutex, const char* mutex_name) {
    if (xSemaphoreGive(mutex) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to release %s mutex", mutex_name);
    }
}

// ==================== WATCHDOG FUNCTIONS ====================
void kick_watchdog() {
    system_healthy = true;
}

void system_watchdog_callback(TimerHandle_t xTimer) {
    if (!system_healthy) {
        ESP_LOGE(TAG, "Watchdog timeout - System appears stuck, restarting...");
        esp_restart();
    }
    system_healthy = false;
}

void send_modbus_led_command_internal(uint16_t value) {
    uint8_t led_cmd[8];
    led_cmd[0] = led_keypadNumber ? led_keypadNumber : 0x00;
    led_cmd[1] = 0x06;
    led_cmd[2] = 0x10;
    led_cmd[3] = 0x08;
    led_cmd[4] = (value >> 8) & 0xFF;
    led_cmd[5] = value & 0xFF;

    uint16_t crc = modbus_crc16(led_cmd, 6);
    led_cmd[6] = crc & 0xFF;
    led_cmd[7] = (crc >> 8) & 0xFF;

    RS485_WRITE_MODE1;
    vTaskDelay(pdMS_TO_TICKS(25));

    int bytes_written = uart_write_bytes(UART_NUM1, (const char *)led_cmd, FRAME_SIZE);
    if (bytes_written != FRAME_SIZE) {
        ESP_LOGW("LED", "UART write incomplete: %d/%d", bytes_written, FRAME_SIZE);
    }

    esp_err_t tx_result = uart_wait_tx_done(UART_NUM1, pdMS_TO_TICKS(100));
    if (tx_result != ESP_OK) {
        ESP_LOGW("LED", "UART TX wait failed: %d", tx_result);
    }

    vTaskDelay(pdMS_TO_TICKS(30));
    RS485_READ_MODE1;
    
    kick_watchdog();
}

int scene_to_trigger = -1;
uint8_t send_cmd_buff[FRAME_SIZE];

void delete_keypad_data_from_nvs(uint8_t keypadNumber) {
    nvs_handle_t nvs;
    char key[16];
    snprintf(key, sizeof(key), "keypad%d", keypadNumber);

    esp_err_t err = nvs_open("keypad_ns", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, key);
        if (err == ESP_OK) {
            nvs_commit(nvs);
            ESP_LOGI("NVM", "Deleted NVS entry for %s", key);
        } else {
            ESP_LOGW("NVM", "Failed to erase key %s (err=%s)", key, esp_err_to_name(err));
        }
        nvs_close(nvs);
    } else {
        ESP_LOGW("NVM", "Failed to open NVS for deletion");
        return;
    }

    for (int i = 0; i < keypads_count; i++) {
        if (keypad_configs[i].keypadNumber == keypadNumber) {
            for (int j = i; j < keypads_count - 1; j++) {
                memcpy(&keypad_configs[j], &keypad_configs[j + 1], sizeof(keypad_config_t));
                memcpy(&last_button_states[j], &last_button_states[j + 1], sizeof(last_button_states[0]));
                memcpy(&last_scene_values[j], &last_scene_values[j + 1], sizeof(last_scene_values[0]));
            }
            keypads_count--;
            memset(&keypad_configs[keypads_count], 0, sizeof(keypad_config_t));
            memset(&last_button_states[keypads_count], 0, sizeof(last_button_states[0]));
            memset(&last_scene_values[keypads_count], 0, sizeof(last_scene_values[0]));
            break;
        }
    }

    ESP_LOGI("NVM", "Keypad %d removed from RAM and polling list", keypadNumber);

    err = nvs_open("keypad_ns", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
        if (err == ESP_OK) {
            ESP_LOGI("NVM", "NVS compacted successfully for keypad_ns");
        } else {
            ESP_LOGW("NVM", "Failed to compact NVS: %s", esp_err_to_name(err));
        }
        nvs_close(nvs);
    }

    // If no keypads left, reset flags and tasks
    if (keypads_count == 0) {
        ESP_LOGW("NVM", "No keypads left, stopping polling");

        polling_task_started = false;
        polling_task_ready   = false;

        if (polling_task) {
            vTaskDelete(polling_task);
            polling_task = NULL;
        }
        if (zone_polling_task) {
            vTaskDelete(zone_polling_task);
            zone_polling_task = NULL;
        }
    }
}

// Holds last known status for each button
bool pressed = false;
//static uint32_t press_hold_start_time = 0;

void check_and_log_button_changes(uint8_t *clean_data) {
    uint8_t panel_id = active_panel_address = clean_data[0];
    uint8_t active_btn;
    
    if(clean_data[5] == 0x01) active_btn = 1;
    else if(clean_data[5] == 0x02) active_btn = 2;
    else if(clean_data[5] == 0x04) active_btn = 3;
    else if(clean_data[5] == 0x08) active_btn = 4;
    else if(clean_data[5] == 0x10) active_btn = 5;
    else if(clean_data[5] == 0x20) active_btn = 6;
    else if(clean_data[5] == 0x40) active_btn = 7;
    else if(clean_data[5] == 0x80) active_btn = 8;
    else active_btn = 0; // No button pressed

    for (int i = 0; i < keypads_count; i++) {
        keypad_config_t *cfg = &keypad_configs[i];
        if (cfg->keypadNumber == panel_id) {
            if (active_btn != 0) {
                ESP_LOGI(TAG_BTN, "Keypad %d, Button %d PRESSED",
                         panel_id, active_btn);
               // toggle_green_led();
                // Trigger the associated action
                ESP_LOGW(TAG_BTN, "-CHECK-1--");
                int key_mode = cfg->keyMode[active_btn - 1];     // 0 = Scene, 1 = Channel
                ESP_LOGW(TAG_BTN, "-CHECK-2--");
                int key_value = cfg->keyValue[active_btn - 1];   // Scene or channel number
                ESP_LOGW(TAG_BTN, "-CHECK-3--");
                int zone = cfg->zoneNumber;
                ESP_LOGW(TAG_BTN, "-CHECK-4--");
                int slaveId = cfg->slaveId;
                ESP_LOGW(TAG_BTN, "-CHECK-5--");
                web_active_keypad_no = panel_id;
                ESP_LOGW(TAG_BTN, "-CHECK-6--");
                if (key_mode == 0) {
                    send_scene_to_controller(slaveId, zone, key_value);
                    ESP_LOGW(TAG_BTN, "-CHECK-7--");
                }
                
                ESP_LOGI("PANNEL DATA MATCH", "Panel: %d, Zone: %d, Button: %d => Mode: %s, Value: %d",
                         panel_id, zone, active_btn,
                         (key_mode == 0) ? "Scene" : "Channel",
                         key_value);
                matched_button = active_btn;
            }
            return;
        }
    }

    ESP_LOGW(TAG_BTN, "Unknown panel ID: %d", panel_id);
}

// Modified keypad polling function using dynamic handler approach
static void dynamic_keypad_polling_handler(void) {
    uint8_t rx_data[256];
    uint8_t frame_to_send[FRAME_SIZE];
    
    if (!take_mutex_with_timeout(modbus_mutex_uart1, "UART1", pdMS_TO_TICKS(200))) {
        ESP_LOGW(TAG_RS485_DATA, "Failed to take UART1 mutex in dynamic_keypad_polling_handler");
        return;
    }
    
    // Always send polling frame - LED commands are now sent immediately
    frame_to_send[0] = 0xFD;  // Broadcast address for keypads
    frame_to_send[1] = 0x03;
    frame_to_send[2] = 0x10;
    frame_to_send[3] = 0x0B;
    frame_to_send[4] = 0x00;
    frame_to_send[5] = 0x01;
    uint16_t crc = modbus_crc16(frame_to_send, 6);
    frame_to_send[6] = crc & 0xFF;
    frame_to_send[7] = (crc >> 8) & 0xFF;
    
    ESP_LOGI(TAG_RS485_DATA, "Sending KEYPAD POLLING frame");
    
    RS485_WRITE_MODE1;
    vTaskDelay(pdMS_TO_TICKS(15));
    
    uart_write_bytes(UART_NUM1, (const char *)frame_to_send, FRAME_SIZE);
    uart_wait_tx_done(UART_NUM1, pdMS_TO_TICKS(50));
    
    RS485_READ_MODE1;
    vTaskDelay(pdMS_TO_TICKS(10));
    
    int len = uart_read_bytes(UART_NUM1, rx_data, sizeof(rx_data), pdMS_TO_TICKS(100));
    
    if (len > 0) {
        ESP_LOGI(TAG_RS485_DATA, "Keypad Response (%d bytes):", len);
        for(int i = 0; i < len; i++) {
            printf("%02X ", rx_data[i]);
        }
        printf("\n");
        
        // Process polling response (button read)
        uint8_t clean_data[FRAME_SIZE];
        if (clean_keypads_response(rx_data, len, clean_data, FRAME_SIZE)) {
            check_and_log_button_changes(clean_data);
        }
    } else {
        ESP_LOGW(TAG_RS485_DATA, "No keypad response received");
    }
    
    give_mutex(modbus_mutex_uart1, "UART1");
    kick_watchdog();
}

bool clean_keypads_response(uint8_t *rx_data, int len, uint8_t *clean_buf, int org_len) {
    if (len < org_len) {
        return false; // Invalid data length
    }
    
    int start_index = 0;
    
    // Skip leading zero bytes
    while (start_index < len && rx_data[start_index] == 0x00) {
        start_index++;
    }
    
    // Calculate remaining bytes after skipping zeros
    int remaining_bytes = len - start_index;
    
    if (remaining_bytes >= org_len) {
        memcpy(clean_buf, &rx_data[start_index], org_len);

        return true;
    } else {
        return false;
    }
}

bool clean_modbus_response(uint8_t *rx_data, int len, uint8_t *clean_buf) {
    if (len < MODBUS_RESP_LEN-1) {
        return false;
    }
    int start_index = 0;
    while (start_index < len && rx_data[start_index] == 0x00) {
        start_index++;
    }
    int remaining_bytes = len - start_index;
    
    if (remaining_bytes >= MODBUS_RESP_LEN) {
        memcpy(clean_buf, &rx_data[start_index], MODBUS_RESP_LEN);
        // for(int i=0;i<MODBUS_RESP_LEN;i++)
        // {
        //     printf("%02X ", clean_buf[i]);
        // }
        // printf("\n");
        return true;
    } else {
        return false;
    }
}

void poll_all_keypads() {
    uint8_t rx_data[BUF_SIZE];
    
    if (!take_mutex_with_timeout(modbus_mutex_uart1, "UART1", pdMS_TO_TICKS(100))) {
        return;
    }
    
    //FD 03 10 0B 00 01 E5 34
    uint8_t btnRead[8] = {0xFD, 0x03, 0x10, 0x0B, 0x00, 0x01, 0xE5, 0x34};
    
    RS485_WRITE_MODE1;
    vTaskDelay(pdMS_TO_TICKS(15)); // Slightly increased delay
    
    int bytes_written = uart_write_bytes(UART_NUM1, (const char *)btnRead, FRAME_SIZE);
    if (bytes_written != FRAME_SIZE) {
        ESP_LOGW(TAG_RS485_DATA, "UART write incomplete: %d/%d", bytes_written, FRAME_SIZE);
    }
    
    esp_err_t tx_result = uart_wait_tx_done(UART_NUM1, pdMS_TO_TICKS(50));
    if (tx_result != ESP_OK) {
        ESP_LOGW(TAG_RS485_DATA, "UART TX wait failed: %d", tx_result);
    }
    
    RS485_READ_MODE1;
    vTaskDelay(pdMS_TO_TICKS(10)); // Reduced delay
    
    int len = uart_read_bytes(UART_NUM1, rx_data, sizeof(rx_data), pdMS_TO_TICKS(100));
    
    give_mutex(modbus_mutex_uart1, "UART1");
    
    if (len > 0) {
        uint8_t clean_data[FRAME_SIZE];
        if (clean_keypads_response(rx_data, len, clean_data, FRAME_SIZE)) {
            check_and_log_button_changes(clean_data);
        }
    }
    
    kick_watchdog(); // Mark system as healthy
}

void polling_response_task(void *arg) {
    ESP_LOGI(TAG_RS485_DATA, "Keypad polling task started");
    polling_task_ready = true;
    xSemaphoreGive(polling_semaphore);
    
    if (keypads_count == 0) {
        ESP_LOGI(TAG_RS485_DATA, "No Data for Keypad Polling");
        vTaskDelete(NULL);
    }
    
    // Initialize keypad frame management if not already done
    if (keypad_frame_mutex == NULL) {
        initialize_keypad_frame_management();
    }
    
    // Initial delay
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Task loop with error handling
    while (1) {
        if (uxTaskGetStackHighWaterMark(NULL) < 512) { // Stack check
            ESP_LOGE(TAG_RS485_DATA, "Keypad polling task stack low!");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        
        dynamic_keypad_polling_handler();
        vTaskDelay(pdMS_TO_TICKS(100)); // Polling interval
    }
}
static TickType_t led_command_start_time = 0;
void check_led_command_timeout() {
    if (led_command_start_time > 0 && 
        (xTaskGetTickCount() - led_command_start_time > pdMS_TO_TICKS(5000))) {
        ESP_LOGW("WATCHDOG", "LED command taking too long, forcing task resume");
        if (polling_task != NULL) {
            vTaskResume(polling_task);
        }
        led_command_start_time = 0;
    }
}
bool task_suspended_in_zone_check = false;
TaskHandle_t saved_polling_task = NULL;

// --------- Scene command ---------

// Global variables for frame management


static uint8_t pending_slaveId, pending_zone, pending_scene;



// Modified polling function that handles both polling and commands
static void dynamic_polling_handler(void) {
    uint8_t rx_data[256];
    uint8_t frame_to_send[FRAME_SIZE];
    
    if (!take_mutex_with_timeout(modbus_mutex_uart2, "UART2", pdMS_TO_TICKS(200))) {
        ESP_LOGW(TAG_RS485_DATA, "Failed to take UART2 mutex in dynamic_polling_handler");
        return;
    }
    
    // Check if we have a command to send instead of polling
    if (frame_mutex != NULL && xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (command_pending) {
            // Use the command frame
            memcpy(frame_to_send, current_frame, FRAME_SIZE);
            command_pending = false; // Reset flag after using the command
            ESP_LOGI(TAG_RS485_DATA, "Sending COMMAND frame");
        } else {
            // Use normal polling frame
            uint16_t reg = ZONE_READ_REG_VAL + 1;
            frame_to_send[0] = default_slave_id;
            frame_to_send[1] = 0x03;
            frame_to_send[2] = (reg >> 8) & 0xFF;
            frame_to_send[3] = reg & 0xFF;
            frame_to_send[4] = 0x00;
            frame_to_send[5] = MAX_ZONES & 0xFF;
            uint16_t crc = modbus_crc16(frame_to_send, 6);
            frame_to_send[6] = crc & 0xFF;
            frame_to_send[7] = (crc >> 8) & 0xFF;
            ESP_LOGI(TAG_RS485_DATA, "Sending MODBUS POLLING frame");
        }
        xSemaphoreGive(frame_mutex);
    } else {
        // Fallback to polling if mutex can't be acquired
        uint16_t reg = ZONE_READ_REG_VAL + 1;
        frame_to_send[0] = default_slave_id;
        frame_to_send[1] = 0x03;
        frame_to_send[2] = (reg >> 8) & 0xFF;
        frame_to_send[3] = reg & 0xFF;
        frame_to_send[4] = 0x00;
        frame_to_send[5] = MAX_ZONES & 0xFF;
        uint16_t crc = modbus_crc16(frame_to_send, 6);
        frame_to_send[6] = crc & 0xFF;
        frame_to_send[7] = (crc >> 8) & 0xFF;
        ESP_LOGI(TAG_RS485_DATA, "Sending POLLING frame (fallback)");
    }
    
    RS485_WRITE_MODE2;
    vTaskDelay(pdMS_TO_TICKS(15));
    
    uart_write_bytes(UART_NUM2, (const char *)frame_to_send, FRAME_SIZE);
    uart_wait_tx_done(UART_NUM2, pdMS_TO_TICKS(50));
    
    RS485_READ_MODE2;
    vTaskDelay(pdMS_TO_TICKS(10));
    
    int len = uart_read_bytes(UART_NUM2, rx_data, sizeof(rx_data), pdMS_TO_TICKS(100));
    
    // Log the sent frame and response
    //ESP_LOGI(TAG_RS485_DATA, "Sent: %02X %02X %02X %02X %02X %02X %02X %02X", 
             //frame_to_send[0], frame_to_send[1], frame_to_send[2], frame_to_send[3],
            // frame_to_send[4], frame_to_send[5], frame_to_send[6], frame_to_send[7]);
    
    if (len > 0) {
        //ESP_LOGI(TAG_RS485_DATA, "Response (%d bytes):", len);
        for(int i = 0; i < len; i++) {
            printf("%02X ", rx_data[i]);
        }
        printf("\n");
        
        // Process response based on what we sent
        if (frame_to_send[1] == 0x03) { // Polling response
            uint8_t clean_data[150];
            if (clean_modbus_response(rx_data, len, clean_data) && clean_data[1] == 0x03) {
                check_and_log_zones_changes(default_slave_id, clean_data, 0);
            }
        } else if (frame_to_send[1] == 0x06) { // Command response
            ESP_LOGI(TAG_RS485_DATA, "Command response received");
            // You can add command response handling here if needed
        }
    } else {
        ESP_LOGW(TAG_RS485_DATA, "No response received");
    }
    
    give_mutex(modbus_mutex_uart2, "UART2");
    kick_watchdog();
}

// Modified scene sending function - now just prepares the command
void send_scene_to_controller(uint8_t slaveId, uint8_t zone, uint8_t scene_no) {
    if (frame_mutex == NULL) {
        ESP_LOGE(TAG_BTN, "Frame mutex not initialized");
        return;
    }
    
    if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Prepare the command frame
        uint8_t scene_to_trigger = scene_no == 0 ? 0 : scene_no;
        uint16_t decimal_value = ZONE_WRITE_REG_VAL + zone;
        
        current_frame[0] = slaveId;
        current_frame[1] = 0x06;
        current_frame[2] = (decimal_value >> 8) & 0xFF;
        current_frame[3] = decimal_value & 0xFF;
        current_frame[4] = 0x00;
        current_frame[5] = scene_to_trigger;
        
        uint16_t crc = modbus_crc16(current_frame, 6);
        current_frame[6] = crc & 0xFF;
        current_frame[7] = (crc >> 8) & 0xFF;
        
        // Set command pending flag
        command_pending = true;
        
        ESP_LOGI(TAG_RS485_DATA, "Command prepared: Scene %d to Zone %d, ID %d", 
                 scene_to_trigger, zone, slaveId);
        
        xSemaphoreGive(frame_mutex);
    } else {
        ESP_LOGE(TAG_BTN, "Failed to acquire frame mutex for scene command");
    }
}

// Modified polling task
void polling_response_task_controller(void *arg) {
    ESP_LOGI(TAG_RS485_DATA, "Dynamic polling task started");
    
    // Initialize frame management if not already done
    if (frame_mutex == NULL) {
        initialize_frame_management();
    }
    
    if (keypads_count == 0) { 
        ESP_LOGI(TAG_RS485_DATA, "No keypads configured, exiting polling task");
        vTaskDelete(NULL);
    }
    
    // Initial delay
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    while (1) {
        int watermark = uxTaskGetStackHighWaterMark(NULL);
        if (watermark < 512) {
            ESP_LOGE(TAG_RS485_DATA, "Polling task stack low: %d", watermark);
        }
        
        dynamic_polling_handler();
        vTaskDelay(pdMS_TO_TICKS(100)); // Polling interval
    }
}

// Modified LED command function to use the frame management system
void send_modbus_led_command(uint16_t value) {
    // This function is now deprecated since we use immediate sending
    // But keep it for compatibility with existing code
    if (led_keypadNumber != 0) {
        send_led_command_immediately(led_keypadNumber, value);
        led_keypadNumber = 0;
    }
}
// Also update the send_led_group_on and send_led_group_off functions to work with individual LED states:
void send_led_group_on(uint8_t keypadNumber, const uint8_t* leds, uint8_t count, uint8_t backlight_on) {
    int keypad_index = -1;
    for (int i = 0; i < keypads_count; i++) {
        if (keypad_configs[i].keypadNumber == keypadNumber) {
            keypad_index = i;
            break;
        }
    }
    
    if (keypad_index < 0) {
        ESP_LOGE("LED", "Keypad %d not found for LED ON command", keypadNumber);
        return;
    }

    uint16_t *state = &led_states[keypad_index];
    ESP_LOGI("LED", "Current LED state for keypad %d: 0x%04X", keypadNumber, *state);

    // Turn ON the specified LEDs
    for (uint8_t i = 0; i < count; i++) {
        if (leds[i] >= 1 && leds[i] <= 8) {
            *state |= (1 << (leds[i] - 1));
            ESP_LOGI("LED", "Turning ON LED %d for keypad %d", leds[i], keypadNumber);
        }
    }
    
    // Handle backlight
    if (backlight_on) {
        *state |= (1 << 8);
        ESP_LOGI("LED", "Backlight ON for keypad %d", keypadNumber);
    } else {
        *state &= ~(1 << 8);
        ESP_LOGI("LED", "Backlight OFF for keypad %d", keypadNumber);
    }

    ESP_LOGI("LED", "New LED state for keypad %d: 0x%04X", keypadNumber, *state);
    send_led_command_immediately(keypadNumber, *state);
}

void send_led_group_off(uint8_t keypadNumber, const uint8_t* leds, uint8_t count, uint8_t backlight_on) {
    int keypad_index = -1;
    for (int i = 0; i < keypads_count; i++) {
        if (keypad_configs[i].keypadNumber == keypadNumber) {
            keypad_index = i;
            break;
        }
    }
    
    if (keypad_index < 0) {
        ESP_LOGE("LED", "Keypad %d not found for LED OFF command", keypadNumber);
        return;
    }

    uint16_t *state = &led_states[keypad_index];
    ESP_LOGI("LED", "Current LED state for keypad %d: 0x%04X", keypadNumber, *state);

    // Turn OFF the specified LEDs
    for (uint8_t i = 0; i < count; i++) {
        if (leds[i] >= 1 && leds[i] <= 8) {
            *state &= ~(1 << (leds[i] - 1));
            ESP_LOGI("LED", "Turning OFF LED %d for keypad %d", leds[i], keypadNumber);
        }
    }
    
    // Handle backlight
    if (backlight_on) {
        *state |= (1 << 8);
        ESP_LOGI("LED", "Keeping backlight ON for keypad %d", keypadNumber);
    } else {
        *state &= ~(1 << 8);
        ESP_LOGI("LED", "Turning OFF backlight for keypad %d", keypadNumber);
    }

    ESP_LOGI("LED", "New LED state for keypad %d: 0x%04X", keypadNumber, *state);
    send_led_command_immediately(keypadNumber, *state);
}

// Add this new function to send LED commands immediately (bypassing the frame queue)
void send_led_command_immediately(uint8_t keypadNumber, uint16_t value) {
    if (!take_mutex_with_timeout(modbus_mutex_uart1, "UART1_LED_IMMEDIATE", pdMS_TO_TICKS(500))) {
        ESP_LOGE("LED", "Failed to acquire UART1 mutex for immediate LED command");
        return;
    }
    
    ESP_LOGI("LED", "Sending immediate LED command to keypad %d: value 0x%04X", keypadNumber, value);
    
    uint8_t led_cmd[8];
    led_cmd[0] = keypadNumber;
    led_cmd[1] = 0x06;
    led_cmd[2] = 0x10;
    led_cmd[3] = 0x08;
    led_cmd[4] = (value >> 8) & 0xFF;
    led_cmd[5] = value & 0xFF;

    uint16_t crc = modbus_crc16(led_cmd, 6);
    led_cmd[6] = crc & 0xFF;
    led_cmd[7] = (crc >> 8) & 0xFF;

    RS485_WRITE_MODE1;
    vTaskDelay(pdMS_TO_TICKS(25));

    int bytes_written = uart_write_bytes(UART_NUM1, (const char *)led_cmd, FRAME_SIZE);
    if (bytes_written != FRAME_SIZE) {
        ESP_LOGW("LED", "UART write incomplete: %d/%d", bytes_written, FRAME_SIZE);
    }

    esp_err_t tx_result = uart_wait_tx_done(UART_NUM1, pdMS_TO_TICKS(100));
    if (tx_result != ESP_OK) {
        ESP_LOGW("LED", "UART TX wait failed: %d", tx_result);
    }

    vTaskDelay(pdMS_TO_TICKS(30));
    RS485_READ_MODE1;
    
    // Read response to clear the buffer
    uint8_t rx_data[32];
    int len = uart_read_bytes(UART_NUM1, rx_data, sizeof(rx_data), pdMS_TO_TICKS(50));
    if (len > 0) {
        ESP_LOGI("LED", "LED command response received (%d bytes)", len);
    }
    
    give_mutex(modbus_mutex_uart1, "UART1_LED_IMMEDIATE");
    kick_watchdog();
}

// Add a function to initialize LED states for all keypads (call this during startup)
void initialize_led_states() {
    for (int i = 0; i < keypads_count; i++) {
        led_states[i] = 0x0000; // All LEDs off initially
        if (BACKLIGHT) {
            led_states[i] |= (1 << 8); // Backlight on if configured
        }
        ESP_LOGI("LED_INIT", "Initialized LED state for keypad %d: 0x%04X", 
                 keypad_configs[i].keypadNumber, led_states[i]);
    }
}
// Add this new function to update keypad LEDs based on scene
void update_keypad_leds_for_scene(uint8_t keypadNumber, uint8_t scene_id) {
    ESP_LOGI("LED_UPDATE", "Updating LEDs for keypad %d, scene %d", keypadNumber, scene_id);
    
    // Find the keypad configuration
    keypad_config_t *cfg = NULL;
    int keypad_index = -1;
    for (int i = 0; i < keypads_count; i++) {
        if (keypad_configs[i].keypadNumber == keypadNumber) {
            cfg = &keypad_configs[i];
            keypad_index = i;
            break;
        }
    }
    
    if (cfg == NULL) {
        ESP_LOGE("LED_UPDATE", "Keypad %d not found in configuration", keypadNumber);
        return;
    }
    
    if (scene_id == 0x00) {
        // Scene 0 means turn off all LEDs for this keypad
        uint8_t all_leds[] = {1, 2, 3, 4, 5, 6, 7, 8};
        send_led_group_off(keypadNumber, all_leds, 8, BACKLIGHT);
        ESP_LOGI("LED_UPDATE", "Scene 0: Turning OFF all LEDs for keypad %d", keypadNumber);
        
        // Update the stored LED state for this keypad
        if (keypad_index >= 0) {
            led_states[keypad_index] = 0x0000; // All LEDs off
            if (BACKLIGHT) {
                led_states[keypad_index] |= (1 << 8); // Keep backlight if enabled
            }
        }
    } else {
        // Find which button corresponds to this scene
        uint8_t button_to_turn_on = 0;
        for (int btn = 0; btn < NUM_BUTTONS; btn++) {
            if (cfg->keyMode[btn] == 0 && cfg->keyValue[btn] == scene_id) {
                button_to_turn_on = btn + 1;
                ESP_LOGI("LED_UPDATE", "Scene %d matches button %d on keypad %d", 
                         scene_id, button_to_turn_on, keypadNumber);
                break;
            }
        }
        
        if (button_to_turn_on > 0) {
            // Update the stored LED state for this keypad
            if (keypad_index >= 0) {
                // Turn ON the specific button's LED (don't turn off others)
                led_states[keypad_index] |= (1 << (button_to_turn_on - 1));
                
                // Ensure backlight is ON if configured
                if (BACKLIGHT) {
                    led_states[keypad_index] |= (1 << 8);
                }
                
                ESP_LOGI("LED_UPDATE", "Keypad %d LED state updated: 0x%04X (Button %d ON)", 
                         keypadNumber, led_states[keypad_index], button_to_turn_on);
                
                // Send the updated LED state to the keypad immediately
                send_led_command_immediately(keypadNumber, led_states[keypad_index]);
            }
        } else {
            ESP_LOGW("LED_UPDATE", "No button configured for scene %d on keypad %d", scene_id, keypadNumber);
        }
    }
}

// Also update the check_and_log_zones_changes function to handle multiple keypads per zone:
void check_and_log_zones_changes(uint8_t slaveId, uint8_t *response, int len) {
    if (response[1] != 0x03) {
        ESP_LOGW("ZONES", "Invalid function code: %02X", response[1]);
        return;
    }

    int byteCount = response[2];
    int zones_in_response = byteCount / 2;
    
    ESP_LOGI("ZONES", "Processing %d zones for slave ID=%d", zones_in_response, slaveId);

    // Scene mapping table
    struct {
        uint16_t bitmask;
        uint8_t scene_id;
    } scene_mapping[] = {
        {0x0001, 0x00}, {0x0002, 0x01}, {0x0004, 0x02}, {0x0008, 0x03},
        {0x0010, 0x04}, {0x0020, 0x05}, {0x0040, 0x06}, {0x0080, 0x07},
        {0x0100, 0x08}, {0x0200, 0x09}, {0x0400, 0x0A}, {0x0800, 0x0B},
        {0x1000, 0x0C}, {0x2000, 0x0D}, {0x4000, 0x0E}, {0x8000, 0x0F}
    };

    for (int z = 0; z < zones_in_response; z++) {
        int offset = 3 + (z * 2);
        uint16_t scene_value = (response[offset] << 8) | response[offset + 1];
        int zoneNumber = z + 1;

        if (slaveId > MAX_SLAVES || zoneNumber > MAX_ZONES) {
            ESP_LOGW("ZONES", "Invalid slaveId=%d zone=%d", slaveId, zoneNumber);
            continue;
        }

        // Only process if scene value changed
        if (scene_value == last_scene_values[slaveId][zoneNumber]) {
            continue;
        }

        // Update stored scene
        last_scene_values[slaveId][zoneNumber] = scene_value;

        // Decode scene ID
        uint8_t scene_id = 0xFF;
        for (int i = 0; i < 16; i++) {
            if (scene_value & scene_mapping[i].bitmask) {
                scene_id = scene_mapping[i].scene_id;
                break;
            }
        }

        if (scene_id == 0xFF) {
            ESP_LOGW("ZONES", "No valid scene found for value 0x%04X in zone %d", scene_value, zoneNumber);
            continue;
        }

        ESP_LOGI("ZONES", "Slave ID=%d, Zone=%d -> Scene %02X (raw: 0x%04X)",
                 slaveId, zoneNumber, scene_id, scene_value);

        // Update ALL keypads that belong to this slaveId and zoneNumber
        bool any_match = false;
        for (int k = 0; k < keypads_count; k++) {
            keypad_config_t *cfg = &keypad_configs[k];
            if (cfg->slaveId == slaveId && cfg->zoneNumber == zoneNumber) {
                ESP_LOGI("ZONES", "Updating keypad %d for Slave=%d Zone=%d Scene=%d", 
                         cfg->keypadNumber, slaveId, zoneNumber, scene_id);
                
                update_keypad_leds_for_scene(cfg->keypadNumber, scene_id);
                any_match = true;
            }
        }

        if (!any_match) {
            ESP_LOGW("ZONES", "No keypads found for Slave=%d Zone=%d", slaveId, zoneNumber);
        }
    }
}


void update_keypad_leds_for_zone(keypad_config_t *keypad, uint16_t zone_value) {
    if (zone_value == 0) {
        // Turn off all LEDs for this zone
        uint8_t all_leds[] = {1, 2, 3, 4, 5, 6, 7, 8};
        send_led_group_off(keypad->keypadNumber, all_leds, 8, 1);
    } else {
        // Find which button corresponds to this scene value
        for (int btn = 0; btn < NUM_BUTTONS; btn++) {
            if (keypad->keyMode[btn] == 0 && keypad->keyValue[btn] == zone_value) {
                // Turn on this LED, turn off others
                uint8_t all_leds[] = {1, 2, 3, 4, 5, 6, 7, 8};
                uint8_t on_led = btn + 1;
                
                send_led_group_off(keypad->keypadNumber, all_leds, 8, 1);
                send_led_group_on(keypad->keypadNumber, &on_led, 1, 1);
                break;
            }
        }
    }
}


esp_err_t active_panel_get_handler(httpd_req_t *req) {
    char resp[32];
    snprintf(resp, sizeof(resp), "%d", web_active_keypad_no);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(5000));
    web_active_keypad_no = 0;
}

// === HTTP Handlers ===
esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

bool is_keypad_in_nvs(uint8_t keypadNumber) {
    nvs_handle_t nvs;
    char key[16];
    snprintf(key, sizeof(key), "keypad%d", keypadNumber);

    bool exists = false;
    if (nvs_open("keypad_ns", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(keypad_config_t);
        keypad_config_t dummy;
        if (nvs_get_blob(nvs, key, &dummy, &len) == ESP_OK) {
            exists = true;
        }
        nvs_close(nvs);
    }
    return exists;
}

esp_err_t submit_post_handler(httpd_req_t *req) {
    char buffer[1024];
    int ret, remaining = req->content_len;
    int total_read = 0;
    while (remaining > 0) {
        int recv_size = remaining < (sizeof(buffer) - total_read - 1) ? remaining : (sizeof(buffer) - total_read - 1);
        ret = httpd_req_recv(req, buffer + total_read, recv_size);        
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        remaining -= ret;
        total_read += ret;
    }
    buffer[total_read] = '\0';
    ESP_LOGI(TAG, "Received form data: %s", buffer);
    keypad_config_t config;
    memset(&config, 0, sizeof(config));
    // Parse URL-encoded form data
    char *token = strtok(buffer, "&");
    while (token != NULL) {
        char *key = token;
        char *value = strchr(token, '=');
        if (value) {
            *value = '\0';
            value++;
            if (strcmp(key, "slaveId") == 0) {
                config.slaveId = default_slave_id = atoi(value);
            } 
            else if (strcmp(key, "keypadNumber") == 0) {
                config.keypadNumber = atoi(value);
            } 
            else if (strcmp(key, "zoneNumber") == 0) {
                config.zoneNumber = atoi(value);
            }
            else {
                // Handle key modes and values
                for (int i = 1; i <= 8; i++) {
                    char mode_key[16], value_key[16];
                    snprintf(mode_key, sizeof(mode_key), "key%dMode", i);
                    snprintf(value_key, sizeof(value_key), "key%dValue", i);
                    
                    if (strcmp(key, mode_key) == 0) {
                        config.keyMode[i-1] = (strcmp(value, "channel") == 0) ? 1 : 0;
                    }
                    else if (strcmp(key, value_key) == 0) {
                        config.keyValue[i-1] = atoi(value);
                    }
                }
            }
        }
        token = strtok(NULL, "&");
    }
    // Log parsed config
    ESP_LOGI(TAG, "Slave ID : %d, Keypad No: %d, Zone: %d", config.slaveId, config.keypadNumber, config.zoneNumber);
    panel_new_address = config.keypadNumber;
    for (int i = 0; i < 8; i++) {
        ESP_LOGI(TAG, "Key %d -> Mode: %s (%d), Value: %d", 
                 i + 1, config.keyMode[i] ? "channel" : "scene", config.keyMode[i], config.keyValue[i]);
    }
    memcpy(&last_submitted_config, &config, sizeof(config));
    last_config_valid = true;
    // === Send response back to client ===
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    // Configure panel address only if keypad does not already exist in NVS
    // Configure panel address only if keypad does not already exist in NVS
    if (!is_keypad_in_nvs(config.keypadNumber)) {
        configure_panel_address();
    } else {
        ESP_LOGI(TAG, "Keypad %d already exists in NVS, updating RAM copy", config.keypadNumber);
        
        // Update the RAM copy with the new configuration
        bool found = false;
        for (int i = 0; i < keypads_count; i++) {
            if (keypad_configs[i].keypadNumber == config.keypadNumber) {
                // Update existing entry in RAM
                memcpy(&keypad_configs[i], &config, sizeof(keypad_config_t));
                found = true;
                ESP_LOGI(TAG, "Updated keypad %d in RAM (existing entry)", config.keypadNumber);
                break;
            }
        }
        
        if (!found && keypads_count < MAX_KEYPADS) {
            // Add as new entry in RAM
            memcpy(&keypad_configs[keypads_count], &config, sizeof(keypad_config_t));
            keypads_count++;
            ESP_LOGI(TAG, "Added keypad %d to RAM (new entry, total: %d)", config.keypadNumber, keypads_count);
        }
        
        // Also update NVS with the new configuration
        nvs_handle_t nvs;
        char key[16];
        snprintf(key, sizeof(key), "keypad%d", config.keypadNumber);
        
        if (nvs_open("keypad_ns", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_blob(nvs, key, &config, sizeof(keypad_config_t));
            nvs_commit(nvs);
            nvs_close(nvs);
            ESP_LOGI(TAG, "Updated keypad %d in NVS", config.keypadNumber);
        }
    }

    // === Start/Restart polling tasks if needed ===
    if (polling_task_started) {
        // Tasks are already running - just update the configuration
        ESP_LOGI(TAG_RS485_DATA, "Polling tasks already running, configuration updated");
    } else {
        // Tasks need to be started
        ESP_LOGI(TAG_RS485_DATA, "Starting polling tasks for the first time");
        
        // Make sure any existing task handles are cleared
        polling_task = NULL;
        zone_polling_task = NULL;
        
        // Start tasks with proper delays
        vTaskDelay(pdMS_TO_TICKS(50));
        xTaskCreate(polling_response_task, "keypad_polling", 8192, NULL, 6, &polling_task);
        
        vTaskDelay(pdMS_TO_TICKS(50));
        xTaskCreate(polling_response_task_controller, "zone_polling", 8192, NULL, 6, &zone_polling_task);
        
        polling_task_started = true;
        
        // Wait for keypad polling task to be ready
        if (xSemaphoreTake(polling_semaphore, pdMS_TO_TICKS(2000)) == pdTRUE) {
            ESP_LOGI(TAG_RS485_DATA, "Polling task is ready");
        } else {
            ESP_LOGE(TAG_RS485_DATA, "Polling task failed to start");
        }
    }
    return ESP_OK;
}


static void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    char ssid[32];
    strcpy(ssid, AP_SSID);
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen((char *)wifi_config.ap.ssid);
    strncpy((char *)wifi_config.ap.password, AP_PASS, sizeof(wifi_config.ap.password));
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP started. SSID:%s Password:%s IP:192.168.4.1", (char *)wifi_config.ap.ssid, (char *)wifi_config.ap.password);
    
}

httpd_handle_t start_MODBUS_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t active_panel_uri = {
            .uri      = "/active_panel",
            .method   = HTTP_GET,
            .handler  = active_panel_get_handler
        };
        httpd_register_uri_handler(server, &active_panel_uri);
        httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler };
        httpd_uri_t submit_uri = { .uri = "/submit", .method = HTTP_POST, .handler = submit_post_handler };
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &submit_uri);
        ESP_LOGI("WEB", "Modbus config server started");
    }
    return server;
}

//=========Configuraing Panel Address==========
void configure_panel_address() {
    if (xSemaphoreTake(modbus_mutex_uart1, pdMS_TO_TICKS(3000)) == pdTRUE) {
        RS485_WRITE_MODE1;
        uint8_t new_address[8] = {0xFF, 0x06, 0x10, 0x50, 0x00, panel_new_address, 0x00, 0x00};
        uint16_t crc = modbus_crc16(new_address, 6);
        new_address[6] = crc & 0xFF;
        new_address[7] = (crc >> 8) & 0xFF;
        uart_write_bytes(UART_NUM1, (const char*)new_address, FRAME_SIZE);
        uart_wait_tx_done(UART_NUM1, pdMS_TO_TICKS(50));
        //ESP_LOGI(TAG_RS485_DATA, "New address frame:");
       // for (int i = 0; i < FRAME_SIZE; i++) printf("%02X ", new_address[i]);
      //  printf("\n");

        // Send save address command
        uint8_t save_address[8] = {0xFF, 0x06, 0x21, 0x99, 0x10, 0x50, 0x4B, 0xFB};
        uart_write_bytes(UART_NUM1, (const char*)save_address, FRAME_SIZE);
        uart_wait_tx_done(UART_NUM1, pdMS_TO_TICKS(50));
        vTaskDelay(pdMS_TO_TICKS(20));

        // --- Start ACK waiting window ---
        uint8_t rx_data[1024];
        uint8_t target_ack[8] = {0xFF, 0x06, 0x10, 0x50, 0xFF, 0xFF, 0x99, 0x75};
        bool ack_received = false;
        int64_t start = esp_timer_get_time();
        int64_t timeout = 60 * 1000000; // 60 seconds

        while ((esp_timer_get_time() - start) < timeout) {
            RS485_READ_MODE1;
            vTaskDelay(pdMS_TO_TICKS(10));
            int len = uart_read_bytes(UART_NUM1, rx_data, sizeof(rx_data), pdMS_TO_TICKS(50));
            if (len > 0) {
                for (int i = 0; i <= len - 8; i++) {
                    if (memcmp(&rx_data[i], target_ack, 8) == 0) {
                        ESP_LOGI(TAG_RS485_DATA, "Panel ACK received for %02X", panel_new_address);

                        // --- Build final config ---
                        keypad_config_t cfg;
                        bool found_existing = false;
                        memset(&cfg, 0, sizeof(cfg));

                        // Step 1: Try to find existing in RAM
                        for (int j = 0; j < keypads_count; j++) {
                            if (keypad_configs[j].keypadNumber == panel_new_address) {
                                memcpy(&cfg, &keypad_configs[j], sizeof(cfg));
                                found_existing = true;
                                break;
                            }
                        }

                        // Step 2: If not found, use last submitted config
                        if (!found_existing && last_config_valid) {
                            memcpy(&cfg, &last_submitted_config, sizeof(cfg));
                        }

                        // Step 3: Always update addressing info
                        cfg.keypadNumber = panel_new_address;
                        cfg.slaveId      = default_slave_id;
                        // Keep zoneNumber from submitted config (don’t force to 1)

                        // --- Save to NVS ---
                        nvs_handle_t nvs;
                        char key[16];
                        snprintf(key, sizeof(key), "keypad%d", panel_new_address);
                        if (nvs_open("keypad_ns", NVS_READWRITE, &nvs) == ESP_OK) {
                            nvs_set_blob(nvs, key, &cfg, sizeof(cfg));
                            nvs_commit(nvs);
                            nvs_close(nvs);
                            ESP_LOGI(TAG, "Saved keypad config for %s", key);
                        }

                        // --- Update in RAM ---
                        bool found = false;
                        for (int j = 0; j < keypads_count; j++) {
                            if (keypad_configs[j].keypadNumber == panel_new_address) {
                                memcpy(&keypad_configs[j], &cfg, sizeof(cfg));
                                found = true;
                                break;
                            }
                        }
                        if (!found && keypads_count < MAX_KEYPADS) {
                            memcpy(&keypad_configs[keypads_count], &cfg, sizeof(cfg));
                            keypads_count++;
                            ESP_LOGI(TAG, "Added new keypad %d (total: %d)", panel_new_address, keypads_count);
                            keypad_ids_Buff[keypad_id_cnt++] = panel_new_address;
                        }

                        ack_received = true;
                        break;
                    }
                }
            }
            if (ack_received) break;
            RS485_WRITE_MODE1;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        if (!ack_received) {
            ESP_LOGW(TAG_RS485_DATA, "No ACK received from panel after 60 seconds, skipping NVS update");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        xSemaphoreGive(modbus_mutex_uart1);
    } else {
        ESP_LOGE(TAG_RS485_DATA, "Failed to acquire UART1 mutex for panel configuration");
    }
}


bool save_keypad_config_to_nvs(uint8_t slaveId,
                               uint8_t keypadNumber,
                               uint8_t zoneNumber,
                               keypad_config_t *cfg) {
    nvs_handle_t nvs;
    char key[16];
    snprintf(key, sizeof(key), "keypad%d", keypadNumber);
    bool is_new = true;
    if (nvs_open("keypad_ns", NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_blob(nvs, key, cfg, sizeof(keypad_config_t)) == ESP_OK) {
            nvs_commit(nvs);
            ESP_LOGI("NVM", "Saved config for %s", key);
            // check if already exists in RAM
            is_new = true;
            for (int i = 0; i < keypads_count; i++) {
                if (keypad_configs[i].keypadNumber == keypadNumber) {
                    memcpy(&keypad_configs[i], cfg, sizeof(keypad_config_t));
                    is_new = false;   // existing keypad updated
                    break;
                }
            }
            if (is_new) {
                if (keypads_count < MAX_KEYPADS) {
                    memcpy(&keypad_configs[keypads_count], cfg, sizeof(keypad_config_t));
                    //keypads_count++;   // increment here so tasks see it immediately
                } else {
                    ESP_LOGE("NVM", "Max keypads reached!");
                    is_new = false;
                }
            }
        }
        nvs_close(nvs);
    }
    return is_new;
}
void read_all_keypad_configs_from_nvs() {
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", "keypad_ns", NVS_TYPE_BLOB, &it);
    keypads_count = 0; // Reset count
    ESP_LOGI(TAG, "==== Reading all keypad configurations from NVS ====");
    
    while (err == ESP_OK && it != NULL && keypads_count < MAX_KEYPADS) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        size_t len = sizeof(keypad_config_t);
        nvs_handle_t nvs;
        if (nvs_open("keypad_ns", NVS_READONLY, &nvs) == ESP_OK) {
            if (nvs_get_blob(nvs, info.key, &keypad_configs[keypads_count], &len) == ESP_OK) {
                keypad_config_t *cfg = &keypad_configs[keypads_count];
                ESP_LOGI("NVM", "Slave ID : %d | Key: %s | ID: %d, Zone: %d", cfg->slaveId, info.key, cfg->keypadNumber, cfg->zoneNumber);
                keypad_ids_Buff[keypad_id_cnt++] = cfg->keypadNumber; 
                for (int i = 0; i < 8; i++) {
                    ESP_LOGI("NVM", "  Key%d: Mode=%s (%d), Value=%d",
                             i + 1,
                             cfg->keyMode[i] ? "channel" : "scene",
                             cfg->keyMode[i],
                             cfg->keyValue[i]);
                }  
                keypads_count++;
            }
            nvs_close(nvs);
        }
        err = nvs_entry_next(&it);
    }
    
    if (it) {
        nvs_release_iterator(it);
    }
    
    ESP_LOGI(TAG, "==== Loaded %d keypad configurations ====", keypads_count);
    
    // Print summary of all loaded keypads
    for (int i = 0; i < keypads_count; i++) {
        keypad_config_t *cfg = &keypad_configs[i];
        ESP_LOGI("NVS_SUMMARY", "Slave ID %d Keypad %d (Zone %d) has %d keys configured", cfg->slaveId, 
                cfg->keypadNumber, cfg->zoneNumber, 8);
        default_slave_id = cfg->slaveId;
    }
    
// Replace the task creation section in read_all_keypad_configs_from_nvs() with:
if (keypads_count && modbus_mutex_uart1 != NULL && modbus_mutex_uart2 != NULL) {
    // Clear any existing task handles
    polling_task = NULL;
    zone_polling_task = NULL;
    
    // Add longer delay before starting tasks
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Create tasks with larger stack sizes
    xTaskCreate(polling_response_task, "keypad_polling", 8192, NULL, 4, &polling_task); // Increased stack
    vTaskDelay(pdMS_TO_TICKS(200));
    xTaskCreate(polling_response_task_controller, "zone_polling", 8192, NULL, 4, &zone_polling_task); // Increased stack
    
    polling_task_started = true;
    
    // Wait for task readiness
    if (xSemaphoreTake(polling_semaphore, pdMS_TO_TICKS(3000)) == pdTRUE) {
        ESP_LOGI(TAG, "Polling tasks started successfully");
    } else {
        ESP_LOGE(TAG, "Polling task failed to signal readiness");
    }
}
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting application...");
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize UARTs
    init_uart();
    init_uart1();

    // Initialize mutexes
    modbus_mutex_uart1 = xSemaphoreCreateMutex();
    modbus_mutex_uart2 = xSemaphoreCreateMutex();
    polling_semaphore = xSemaphoreCreateBinary();
    
    // Initialize frame management
    initialize_frame_management();
    initialize_keypad_frame_management();
    wifi_init_softap();
    start_MODBUS_webserver();
    // Load keypad configurations
    read_all_keypad_configs_from_nvs();
    initialize_led_states();
    // Create polling tasks if keypads are configured
    // if (keypads_count > 0) {
    //     xTaskCreate(polling_response_task, "keypad_polling", 4096, NULL, 5, &polling_task);
    //     xTaskCreate(polling_response_task_controller, "controller_polling", 4096, NULL, 5, &zone_polling_task);
    //     polling_task_started = true;
    // }
    
    // Start watchdog timer
    task_watchdog_timer = xTimerCreate("watchdog", pdMS_TO_TICKS(5000), pdTRUE, NULL, system_watchdog_callback);
    xTimerStart(task_watchdog_timer, 0);
    
    ESP_LOGI(TAG, "Application started successfully");
    
    // Main loop - just kick watchdog periodically
    while (1) {
        kick_watchdog();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}




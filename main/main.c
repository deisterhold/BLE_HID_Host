#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "esp_hidh_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "classic_hid_host";
static const uint8_t INQUIRY_LENGTH = 8;
static const esp_bd_addr_t TARGET_MOUSE_ADDRESS = {0x75, 0x47, 0x7D, 0x57, 0x5A, 0x3C};
static const int LEFT_STRIP_GPIO = 18;
static const int RIGHT_STRIP_GPIO = 21;
static const uint32_t STRIP_PIXEL_COUNT = 8;
static const int64_t BLINK_INTERVAL_US = 300000;

#define LEFT_BLINK_BIT BIT0
#define RIGHT_BLINK_BIT BIT1

static led_strip_handle_t left_strip;
static led_strip_handle_t right_strip;
static EventGroupHandle_t blink_event_group;

static volatile bool scan_requested;
static volatile bool scan_running;
static volatile bool connect_after_discovery;
static volatile bool connection_pending;
static volatile bool device_connected;
static esp_bd_addr_t active_address;
static uint8_t previous_keys[6];
static uint8_t previous_modifiers;
static uint8_t previous_mouse_buttons;

static void log_address(const uint8_t *address)
{
    ESP_LOGI(TAG, "%02X:%02X:%02X:%02X:%02X:%02X",
             address[0], address[1], address[2],
             address[3], address[4], address[5]);
}

static bool is_peripheral(const esp_bt_gap_cb_param_t *param)
{
    uint32_t cod = 0;

    for (int index = 0; index < param->disc_res.num_prop; ++index) {
        const esp_bt_gap_dev_prop_t *property = &param->disc_res.prop[index];
        if (property->type == ESP_BT_GAP_DEV_PROP_COD && property->val != NULL &&
            property->len >= (int)sizeof(cod)) {
            memcpy(&cod, property->val, sizeof(cod));
            return esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
        }
    }

    return false;
}

static const char *key_name(uint8_t usage, bool shifted, char *buffer, size_t size)
{
    if (usage >= 0x04 && usage <= 0x1d) {
        const char letter = (char)('a' + usage - 0x04);
        buffer[0] = shifted ? (char)(letter - 'a' + 'A') : letter;
        buffer[1] = '\0';
        return buffer;
    }
    if (usage >= 0x1e && usage <= 0x27) {
        static const char unshifted[] = "1234567890";
        static const char shifted_digits[] = "!@#$%^&*()";
        buffer[0] = shifted ? shifted_digits[usage - 0x1e] : unshifted[usage - 0x1e];
        buffer[1] = '\0';
        return buffer;
    }

    switch (usage) {
    case 0x28: return "ENTER";
    case 0x29: return "ESC";
    case 0x2a: return "BACKSPACE";
    case 0x2b: return "TAB";
    case 0x2c: return "SPACE";
    case 0x4f: return "RIGHT";
    case 0x50: return "LEFT";
    case 0x51: return "DOWN";
    case 0x52: return "UP";
    default:
        snprintf(buffer, size, "USAGE_0x%02X", usage);
        return buffer;
    }
}

static void on_left_button(bool pressed)
{
    if (!pressed) {
        ESP_LOGI(TAG, "Left button released; starting left strip blink");
        xEventGroupClearBits(blink_event_group, RIGHT_BLINK_BIT);
        xEventGroupSetBits(blink_event_group, LEFT_BLINK_BIT);
    }
}

static void on_right_button(bool pressed)
{
    if (!pressed) {
        ESP_LOGI(TAG, "Right button released; starting right strip blink");
        xEventGroupClearBits(blink_event_group, LEFT_BLINK_BIT);
        xEventGroupSetBits(blink_event_group, RIGHT_BLINK_BIT);
    }
}

static void on_middle_button(bool pressed)
{
    (void)pressed;
}

static void on_bottom_button(bool pressed)
{
    if (!pressed) {
        ESP_LOGI(TAG, "Bottom button released; cancelling both blink patterns");
        xEventGroupClearBits(blink_event_group, LEFT_BLINK_BIT | RIGHT_BLINK_BIT);
    }
}

static void on_back_button(bool pressed)
{
    ESP_LOGI(TAG, "MOUSE BACK %s", pressed ? "DOWN" : "UP");
}

static void on_forward_button(bool pressed)
{
    (void)pressed;
}

static void log_mouse_report_id_3_buttons(uint8_t buttons)
{
    static const struct {
        uint8_t mask;
        void (*handler)(bool pressed);
    } button_handlers[] = {
        {0x10, on_left_button},
        {0x20, on_right_button},
        {0x04, on_bottom_button},
    };

    for (size_t index = 0; index < sizeof(button_handlers) / sizeof(button_handlers[0]); ++index) {
        const bool was_pressed = (previous_mouse_buttons & button_handlers[index].mask) != 0;
        const bool is_pressed = (buttons & button_handlers[index].mask) != 0;
        if (was_pressed != is_pressed) {
            button_handlers[index].handler(is_pressed);
        }
    }
    previous_mouse_buttons = buttons;
}

static void log_keyboard_report(const uint8_t *data, uint16_t length)
{
    if (length < 8) {
        ESP_LOGW(TAG, "Short boot keyboard report: %u bytes", length);
        return;
    }

    const uint8_t modifiers = data[0];
    const bool shifted = (modifiers & 0x22) != 0;
    if (modifiers != previous_modifiers) {
        ESP_LOGI(TAG, "KEY MODIFIERS: 0x%02X", modifiers);
        previous_modifiers = modifiers;
    }

    for (size_t index = 0; index < 6; ++index) {
        const uint8_t usage = data[index + 2];
        bool was_down = false;
        if (usage == 0 || usage == 1 || usage == 2) {
            continue;
        }
        for (size_t previous = 0; previous < 6; ++previous) {
            if (previous_keys[previous] == usage) {
                was_down = true;
                break;
            }
        }
        if (!was_down) {
            char name[20];
            ESP_LOGI(TAG, "KEY DOWN: %s (usage 0x%02X)",
                     key_name(usage, shifted, name, sizeof(name)), usage);
        }
    }

    for (size_t previous = 0; previous < 6; ++previous) {
        const uint8_t usage = previous_keys[previous];
        bool still_down = false;
        if (usage == 0 || usage == 1 || usage == 2) {
            continue;
        }
        for (size_t index = 0; index < 6; ++index) {
            if (data[index + 2] == usage) {
                still_down = true;
                break;
            }
        }
        if (!still_down) {
            char name[20];
            ESP_LOGI(TAG, "KEY UP: %s (usage 0x%02X)",
                     key_name(usage, shifted, name, sizeof(name)), usage);
        }
    }

    memcpy(previous_keys, data + 2, sizeof(previous_keys));
}

static void log_mouse_report(const uint8_t *data, uint16_t length)
{
    if (length < 3) {
        ESP_LOGW(TAG, "Short boot mouse report: %u bytes", length);
        return;
    }

    const uint8_t buttons = data[0];
    const int8_t x = (int8_t)data[1];
    const int8_t y = (int8_t)data[2];
    const int8_t wheel = length >= 4 ? (int8_t)data[3] : 0;
    static const struct {
        uint8_t mask;
        void (*handler)(bool pressed);
    } button_handlers[] = {
        {1U << 0, on_left_button},
        {1U << 1, on_right_button},
        {1U << 2, on_middle_button},
        {1U << 3, on_back_button},
        {1U << 4, on_forward_button},
    };

    for (size_t index = 0; index < sizeof(button_handlers) / sizeof(button_handlers[0]); ++index) {
        const bool was_pressed = (previous_mouse_buttons & button_handlers[index].mask) != 0;
        const bool is_pressed = (buttons & button_handlers[index].mask) != 0;
        if (was_pressed != is_pressed) {
            button_handlers[index].handler(is_pressed);
        }
    }

    if (x != 0 || y != 0 || wheel != 0) {
        ESP_LOGI(TAG, "MOUSE MOTION: dx=%d dy=%d wheel=%d", x, y, wheel);
    }
    previous_mouse_buttons = buttons;
}

static void hid_host_callback(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param)
{
    switch (event) {
    case ESP_HIDH_INIT_EVT:
        if (param->init.status == ESP_HIDH_OK) {
            ESP_LOGI(TAG, "Classic HID host ready; scanning for target mouse");
            scan_requested = true;
        } else {
            ESP_LOGE(TAG, "HID host initialization failed: %d", param->init.status);
        }
        break;

    case ESP_HIDH_OPEN_EVT:
        if (param->open.conn_status == ESP_HIDH_CONN_STATE_CONNECTING) {
            break;
        }
        connection_pending = false;
        if (param->open.status == ESP_HIDH_OK &&
            param->open.conn_status == ESP_HIDH_CONN_STATE_CONNECTED) {
            memcpy(active_address, param->open.bd_addr, sizeof(active_address));
            device_connected = true;
            memset(previous_keys, 0, sizeof(previous_keys));
            previous_modifiers = 0;
            previous_mouse_buttons = 0;
            ESP_LOGI(TAG, "HID connected:");
            log_address(active_address);
            esp_err_t result = esp_bt_hid_host_set_protocol(active_address, ESP_HIDH_BOOT_MODE);
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Could not request boot protocol: %s", esp_err_to_name(result));
            }
        } else {
            ESP_LOGW(TAG, "HID connection failed: status=%d", param->open.status);
            scan_requested = true;
        }
        break;

    case ESP_HIDH_DATA_IND_EVT: {
        const uint8_t *data = param->data_ind.data;
        const uint16_t length = param->data_ind.len;
        if (data == NULL || length == 0) {
            break;
        }

        ESP_LOGD(TAG, "HID INPUT: mode=%s length=%u",
             param->data_ind.proto_mode == ESP_HIDH_BOOT_MODE ? "BOOT" : "REPORT",
             length);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, length, ESP_LOG_DEBUG);
        if (length == 3 && data[0] == 0x03) {
            log_mouse_report_id_3_buttons(data[1]);
        } else if (param->data_ind.proto_mode == ESP_HIDH_BOOT_MODE) {
            if (length == 8) {
                log_keyboard_report(data, length);
            } else if (length == 3 || length == 4) {
                log_mouse_report(data, length);
            }
        }
        break;
    }

    case ESP_HIDH_SET_PROTO_EVT:
        if (param->set_proto.status == ESP_HIDH_OK) {
            ESP_LOGI(TAG, "Mouse accepted boot protocol request");
            esp_bt_hid_host_get_protocol(active_address);
        } else {
            ESP_LOGW(TAG, "Mouse rejected boot protocol request: status=%d",
                     param->set_proto.status);
        }
        break;

    case ESP_HIDH_GET_PROTO_EVT:
        if (param->get_proto.status == ESP_HIDH_OK) {
            ESP_LOGI(TAG, "Mouse protocol mode: %s",
                     param->get_proto.proto_mode == ESP_HIDH_BOOT_MODE ? "BOOT" : "REPORT");
        } else {
            ESP_LOGW(TAG, "Could not read mouse protocol mode: status=%d",
                     param->get_proto.status);
        }
        break;

    case ESP_HIDH_CLOSE_EVT:
        ESP_LOGI(TAG, "HID disconnected: status=%d reason=0x%02X",
                 param->close.status, param->close.reason);
        device_connected = false;
        connection_pending = false;
        scan_requested = true;
        break;

    case ESP_HIDH_VC_UNPLUG_EVT:
        ESP_LOGI(TAG, "HID device requested virtual cable unplug");
        break;

    default:
        ESP_LOGD(TAG, "HID host event: %d", event);
        break;
    }
}

static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        if (!scan_running || connection_pending || device_connected || !is_peripheral(param)) {
            break;
        }
        if (memcmp(param->disc_res.bda, TARGET_MOUSE_ADDRESS, sizeof(TARGET_MOUSE_ADDRESS)) != 0) {
            break;
        }
        memcpy(active_address, param->disc_res.bda, sizeof(active_address));
        connection_pending = true;
        scan_requested = false;
        connect_after_discovery = true;
        ESP_LOGI(TAG, "Found target mouse; stopping inquiry before connecting");
        log_address(active_address);
        esp_err_t result = esp_bt_gap_cancel_discovery();
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Could not stop Bluetooth inquiry: %s", esp_err_to_name(result));
            connect_after_discovery = false;
            connection_pending = false;
            scan_requested = true;
        }
        break;

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            scan_running = false;
            if (connect_after_discovery) {
                connect_after_discovery = false;
                ESP_LOGI(TAG, "Inquiry stopped; starting HID connection");
                esp_err_t result = esp_bt_hid_host_connect(active_address);
                if (result != ESP_OK) {
                    ESP_LOGW(TAG, "Could not start HID connection: %s", esp_err_to_name(result));
                    connection_pending = false;
                    scan_requested = true;
                }
            } else if (!connection_pending && !device_connected) {
                scan_requested = true;
            }
        }
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGW(TAG, "Pairing confirmation requested (numeric code %06" PRIu32 "); accepting",
                 param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code = {'0', '0', '0', '0'};
        ESP_LOGW(TAG, "Legacy pairing PIN requested; replying with 0000");
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        break;
    }

    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "Bluetooth authentication %s",
                 param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "succeeded" : "failed");
        break;

    default:
        break;
    }
}

static void initialize_bluetooth(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);

    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    controller_config.mode = ESP_BT_MODE_CLASSIC_BT;
    ESP_ERROR_CHECK(esp_bt_controller_init(&controller_config));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_callback));
    ESP_ERROR_CHECK(esp_bt_hid_host_register_callback(hid_host_callback));
    ESP_ERROR_CHECK(esp_bt_hid_host_init());
}

static led_strip_handle_t create_led_strip(int gpio_num)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = gpio_num,
        .max_leds = STRIP_PIXEL_COUNT,
        .led_model = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRBW,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    led_strip_handle_t strip = NULL;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip));
    ESP_ERROR_CHECK(led_strip_clear(strip));
    return strip;
}

static void set_strip_color(led_strip_handle_t strip, bool yellow)
{
    const uint32_t red = yellow ? 128 : 0;
    const uint32_t green = yellow ? 128 : 0;
    for (uint32_t pixel = 0; pixel < STRIP_PIXEL_COUNT; ++pixel) {
        ESP_ERROR_CHECK(led_strip_set_pixel_rgbw(strip, pixel, red, green, 0, 0));
    }
    ESP_ERROR_CHECK(led_strip_refresh(strip));
}

static void update_blink_patterns(void)
{
    static EventBits_t rendered_mask = 0;
    static bool rendered_lit;
    static bool phase_lit;
    static int64_t next_toggle_us;

    const EventBits_t active_mask = xEventGroupGetBits(blink_event_group) &
                                    (LEFT_BLINK_BIT | RIGHT_BLINK_BIT);
    const int64_t now_us = esp_timer_get_time();

    if (active_mask == 0) {
        phase_lit = false;
        next_toggle_us = 0;
    } else if (active_mask != rendered_mask) {
        phase_lit = true;
        next_toggle_us = now_us + BLINK_INTERVAL_US;
    } else if (next_toggle_us == 0) {
        phase_lit = true;
        next_toggle_us = now_us + BLINK_INTERVAL_US;
    } else if (now_us >= next_toggle_us) {
        phase_lit = !phase_lit;
        next_toggle_us = now_us + BLINK_INTERVAL_US;
    }

    if (active_mask == rendered_mask && phase_lit == rendered_lit) {
        return;
    }

    set_strip_color(left_strip, (active_mask & LEFT_BLINK_BIT) != 0 && phase_lit);
    set_strip_color(right_strip, (active_mask & RIGHT_BLINK_BIT) != 0 && phase_lit);
    rendered_mask = active_mask;
    rendered_lit = phase_lit;
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-IDF Bluetooth Classic HID host");
    blink_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(blink_event_group == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    left_strip = create_led_strip(LEFT_STRIP_GPIO);
    right_strip = create_led_strip(RIGHT_STRIP_GPIO);
    initialize_bluetooth();

    while (true) {
        if (scan_requested && !scan_running && !connection_pending && !device_connected) {
            scan_requested = false;
            scan_running = true;
            ESP_LOGI(TAG, "Starting 10-second Bluetooth inquiry");
            esp_err_t result = esp_bt_gap_start_discovery(
                ESP_BT_INQ_MODE_GENERAL_INQUIRY, INQUIRY_LENGTH, 0);
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Inquiry could not start: %s", esp_err_to_name(result));
                scan_running = false;
                scan_requested = true;
            }
        }
        update_blink_patterns();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
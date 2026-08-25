// MIT License

// Copyright (c) 2017 Vadim Grigoruk @nesbox // grigoruk@gmail.com

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// USB HID host. A keyboard types into the editors, a mouse drives the pointer
// TIC-80 needs for half of them, and a gamepad plays.
//
// Devices do not agree on a report layout, so rather than guess by report
// length this reads the report descriptor and looks the controls up by usage.
// That parsing lives in the badgeteam/hid-host component; what is here is the
// USB host plumbing around it and the mapping onto tic_key and tic80_gamepad.
//
// Reports arrive on the HID driver's own task. They are decoded there and the
// result is posted to a queue the main loop drains in tanmatsu_usb_hid_poll(),
// which keeps every field TIC-80 reads owned by one task.

#include "usb_hid.h"

#include <string.h>

#include "bsp/power.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hid_gamepad.h"
#include "hid_layout.h"
#include "usb/hid.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"
#include "usb/usb_host.h"

static char const TAG[] = "tic80-usb";

// A keyboard, a mouse and a pad at once is already more than the port can use,
// and every slot costs a report descriptor's worth of parsed layout.
#define MAX_DEVICES 4

#define EVENT_QUEUE_LENGTH 16

// Mice count in their own units and a modern one counts fast. TIC-80's screen
// is 256 pixels wide, so at 1:1 an inch of desk crosses it several times over.
#define MOUSE_DIVISOR 2

typedef enum {
    DEVICE_NONE = 0,
    DEVICE_KEYBOARD,      // Boot protocol, which is all TIC-80 needs of one
    DEVICE_MOUSE,         // Report protocol, read through the parsed layout
    DEVICE_MOUSE_BOOT,    // A mouse whose descriptor said nothing usable
    DEVICE_GAMEPAD,
} device_kind_t;

typedef struct {
    bool                     in_use;
    device_kind_t            kind;
    hid_host_device_handle_t handle;
    hid_layout_t             layout;   // Mice
    hid_gamepad_t            gamepad;  // Gamepads
} device_t;

typedef enum {
    EVENT_KEYBOARD = 0,
    EVENT_MOUSE,
    EVENT_GAMEPAD,
    EVENT_DEVICES,  // A device came or went; carries what is plugged in now
} event_type_t;

typedef struct {
    event_type_t type;
    uint8_t      slot;  // Which device it came from, for the per-device state
    union {
        struct {
            uint8_t modifier;
            uint8_t keys[HID_KEYBOARD_KEY_MAX];
        } keyboard;
        struct {
            int16_t dx;
            int16_t dy;
            int8_t  wheel;
            uint8_t buttons;
        } mouse;
        struct {
            uint8_t buttons;  // tic80_gamepad bits
        } gamepad;
        struct {
            bool    keyboard;
            bool    mouse;
            bool    gamepad;
            uint8_t live;  // Bit per slot still plugged in
        } devices;
    };
} event_t;

// Written by the HID driver task, read by the main loop through the queue.
static device_t      devices[MAX_DEVICES];
static QueueHandle_t event_queue  = NULL;
static QueueHandle_t driver_queue = NULL;
static bool          started      = false;

// Main loop side: what the last reports added up to. The keyboard state is
// kept per device rather than merged as it arrives: a keyboard that presents
// two interfaces reports the same key on one and nothing on the other, and
// shared state would see that as the key being pressed over and over.
static bool key_state[MAX_DEVICES][tic_keys_count];
static int32_t pending_dx     = 0;
static int32_t pending_dy     = 0;
static int32_t pending_scroll = 0;
static int32_t leftover_dx    = 0;  // The half pixels MOUSE_DIVISOR leaves behind
static int32_t leftover_dy    = 0;
static bool    mouse_left     = false;
static bool    mouse_middle   = false;
static bool    mouse_right    = false;
static uint8_t gamepad_state  = 0;
static bool    have_keyboard  = false;
static bool    have_mouse     = false;
static bool    have_gamepad   = false;

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

static tic_key usage_to_tic_key(uint8_t usage) {
    if (usage >= HID_KEY_A && usage <= HID_KEY_Z) {
        return (tic_key)(tic_key_a + (usage - HID_KEY_A));
    }
    if (usage >= HID_KEY_1 && usage <= HID_KEY_9) {
        return (tic_key)(tic_key_1 + (usage - HID_KEY_1));
    }
    if (usage >= HID_KEY_F1 && usage <= HID_KEY_F12) {
        return (tic_key)(tic_key_f1 + (usage - HID_KEY_F1));
    }
    if (usage >= HID_KEY_KEYPAD_1 && usage <= HID_KEY_KEYPAD_9) {
        return (tic_key)(tic_key_numpad1 + (usage - HID_KEY_KEYPAD_1));
    }

    switch (usage) {
        case HID_KEY_0: return tic_key_0;
        case HID_KEY_ENTER: return tic_key_return;
        case HID_KEY_ESC: return tic_key_escape;
        case HID_KEY_DEL: return tic_key_backspace;
        case HID_KEY_TAB: return tic_key_tab;
        case HID_KEY_SPACE: return tic_key_space;
        case HID_KEY_MINUS: return tic_key_minus;
        case HID_KEY_EQUAL: return tic_key_equals;
        case HID_KEY_OPEN_BRACKET: return tic_key_leftbracket;
        case HID_KEY_CLOSE_BRACKET: return tic_key_rightbracket;
        case HID_KEY_BACK_SLASH:
        case HID_KEY_SHARP: return tic_key_backslash;
        case HID_KEY_COLON: return tic_key_semicolon;
        case HID_KEY_QUOTE: return tic_key_apostrophe;
        case HID_KEY_TILDE: return tic_key_grave;
        case HID_KEY_LESS: return tic_key_comma;
        case HID_KEY_GREATER: return tic_key_period;
        case HID_KEY_SLASH: return tic_key_slash;
        case HID_KEY_CAPS_LOCK: return tic_key_capslock;
        case HID_KEY_INSERT: return tic_key_insert;
        case HID_KEY_HOME: return tic_key_home;
        case HID_KEY_PAGEUP: return tic_key_pageup;
        case HID_KEY_DELETE: return tic_key_delete;
        case HID_KEY_END: return tic_key_end;
        case HID_KEY_PAGEDOWN: return tic_key_pagedown;
        case HID_KEY_RIGHT: return tic_key_right;
        case HID_KEY_LEFT: return tic_key_left;
        case HID_KEY_DOWN: return tic_key_down;
        case HID_KEY_UP: return tic_key_up;
        case HID_KEY_KEYPAD_DIV: return tic_key_numpaddivide;
        case HID_KEY_KEYPAD_MUL: return tic_key_numpadmultiply;
        case HID_KEY_KEYPAD_SUB: return tic_key_numpadminus;
        case HID_KEY_KEYPAD_ADD: return tic_key_numpadplus;
        case HID_KEY_KEYPAD_ENTER: return tic_key_numpadenter;
        case HID_KEY_KEYPAD_0: return tic_key_numpad0;
        case HID_KEY_KEYPAD_DELETE: return tic_key_numpadperiod;
        default: return tic_key_unknown;
    }
}

// A boot report carries every key that is down, so the state is rebuilt from
// it outright rather than tracked press by press.
//
// The characters themselves are left to the studio, which works them out from
// the keys it is handed and repeats them at its own rate. Handing it text as
// well would be a second source of the same keystroke.
static void apply_keyboard(uint8_t slot, const uint8_t modifier, const uint8_t* keys) {
    bool* state = key_state[slot];

    memset(state, 0, sizeof(key_state[slot]));

    state[tic_key_shift] = (modifier & (HID_LEFT_SHIFT | HID_RIGHT_SHIFT)) != 0;
    state[tic_key_ctrl]  = (modifier & (HID_LEFT_CONTROL | HID_RIGHT_CONTROL)) != 0;
    state[tic_key_alt]   = (modifier & (HID_LEFT_ALT | HID_RIGHT_ALT)) != 0;

    for (size_t i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        uint8_t usage = keys[i];
        if (usage <= HID_KEY_ERROR_UNDEFINED) {
            continue;
        }

        tic_key key = usage_to_tic_key(usage);
        if (key != tic_key_unknown) {
            state[key] = true;
        }
    }

}

// ---------------------------------------------------------------------------
// Report decoding, on the HID driver's task
// ---------------------------------------------------------------------------

static void post(const event_t* event) {
    if (event_queue != NULL) {
        xQueueSend(event_queue, event, 0);
    }
}

static void post_device_list(void) {
    event_t event = {.type = EVENT_DEVICES};
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (!devices[i].in_use) {
            continue;
        }
        event.devices.live |= (uint8_t)(1u << i);
        switch (devices[i].kind) {
            case DEVICE_KEYBOARD: event.devices.keyboard = true; break;
            case DEVICE_MOUSE:
            case DEVICE_MOUSE_BOOT: event.devices.mouse = true; break;
            case DEVICE_GAMEPAD: event.devices.gamepad = true; break;
            default: break;
        }
    }
    post(&event);
}

static void decode_keyboard(uint8_t slot, const uint8_t* data, size_t length) {
    if (length < sizeof(hid_keyboard_input_report_boot_t)) {
        return;
    }

    const hid_keyboard_input_report_boot_t* report = (const hid_keyboard_input_report_boot_t*)data;

    event_t event = {.type = EVENT_KEYBOARD, .slot = slot};
    event.keyboard.modifier = report->modifier.val;
    memcpy(event.keyboard.keys, report->key, HID_KEYBOARD_KEY_MAX);
    post(&event);
}

static void decode_mouse_boot(const uint8_t* data, size_t length) {
    if (length < sizeof(hid_mouse_input_report_boot_t)) {
        return;
    }

    const hid_mouse_input_report_boot_t* report = (const hid_mouse_input_report_boot_t*)data;

    event_t event = {.type = EVENT_MOUSE};
    event.mouse.dx      = report->x_displacement;
    event.mouse.dy      = report->y_displacement;
    event.mouse.buttons = report->buttons.val & 0x07;
    post(&event);
}

static void decode_mouse(const device_t* device, const uint8_t* data, int length) {
    const uint8_t* body = data;
    int            left = length;
    if (!hid_layout_strip_report_id(&device->layout, &body, &left)) {
        return;  // A report for some other part of the device, a battery level say
    }

    event_t event = {.type = EVENT_MOUSE};
    event.mouse.dx    = (int16_t)hid_layout_read(body, left, &device->layout.x);
    event.mouse.dy    = (int16_t)hid_layout_read(body, left, &device->layout.y);
    event.mouse.wheel = (int8_t)hid_layout_read(body, left, &device->layout.wheel);

    for (uint16_t button = 0; button < device->layout.button_count && button < 3; button++) {
        if (hid_layout_read_button(body, left, &device->layout, button)) {
            event.mouse.buttons |= (uint8_t)(1u << button);
        }
    }

    post(&event);
}

static void decode_gamepad(const device_t* device, const uint8_t* data, int length) {
    hid_gamepad_state_t state;
    if (!hid_gamepad_decode(&device->gamepad, data, length, &state)) {
        return;
    }

    // TIC-80 has four buttons, and a pad names its own however it likes. The
    // first four the descriptor numbers are the closest thing to a convention
    // there is; a pad that disagrees still steers, it just labels oddly.
    uint8_t buttons = 0;
    if (state.up) {
        buttons |= 1u << 0;
    }
    if (state.down) {
        buttons |= 1u << 1;
    }
    if (state.left) {
        buttons |= 1u << 2;
    }
    if (state.right) {
        buttons |= 1u << 3;
    }

    uint32_t named = state.usage_buttons != 0 ? state.usage_buttons : state.buttons;
    for (uint8_t i = 0; i < 4; i++) {
        if (named & (1u << i)) {
            buttons |= (uint8_t)(1u << (4 + i));
        }
    }

    event_t event = {.type = EVENT_GAMEPAD};
    event.gamepad.buttons = buttons;
    post(&event);
}

// ---------------------------------------------------------------------------
// USB host plumbing
// ---------------------------------------------------------------------------

static void interface_callback(hid_host_device_handle_t handle, const hid_host_interface_event_t event, void* arg) {
    device_t* device = (device_t*)arg;

    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
            uint8_t data[64];
            size_t  length = 0;
            if (hid_host_device_get_raw_input_report_data(handle, data, sizeof(data), &length) != ESP_OK) {
                return;
            }

            uint8_t slot = (uint8_t)(device - devices);

            switch (device->kind) {
                case DEVICE_KEYBOARD: decode_keyboard(slot, data, length); break;
                case DEVICE_MOUSE: decode_mouse(device, data, (int)length); break;
                case DEVICE_MOUSE_BOOT: decode_mouse_boot(data, length); break;
                case DEVICE_GAMEPAD: decode_gamepad(device, data, (int)length); break;
                default: break;
            }
            break;
        }

        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Device disconnected");
            hid_host_device_close(handle);
            device->in_use = false;
            device->kind   = DEVICE_NONE;
            post_device_list();
            break;

        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
            ESP_LOGW(TAG, "Transfer error");
            break;

        default:
            break;
    }
}

static device_t* take_slot(void) {
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (!devices[i].in_use) {
            memset(&devices[i], 0, sizeof(devices[i]));
            devices[i].in_use = true;
            return &devices[i];
        }
    }
    return NULL;
}

// Some gamepads enumerate, hand out a report descriptor and then say nothing
// at all until they are asked to start reporting. The hid-host component knows
// which ones and what to send; sending it is up to whoever holds the handle.
static void nudge_gamepad(hid_host_device_handle_t handle, const hid_gamepad_t* gamepad) {
    const hid_gamepad_quirk_t* quirk = gamepad->quirk;
    if (quirk == NULL || quirk->enable_report == NULL) {
        return;
    }

    uint8_t report[8] = {0};
    size_t  length    = quirk->enable_report_length;
    if (length > sizeof(report)) {
        length = sizeof(report);
    }
    memcpy(report, quirk->enable_report, length);

    ESP_LOGI(TAG, "%s needs a nudge before it reports, sending it", quirk->name);
    hid_class_request_set_report(handle, HID_REPORT_TYPE_FEATURE, quirk->enable_report_id, report, length);
}

static void device_connected(hid_host_device_handle_t handle) {
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(handle, &params) != ESP_OK) {
        return;
    }

    device_t* device = take_slot();
    if (device == NULL) {
        ESP_LOGW(TAG, "No room for another device");
        return;
    }
    device->handle = handle;

    const hid_host_device_config_t config = {.callback = interface_callback, .callback_arg = device};
    if (hid_host_device_open(handle, &config) != ESP_OK) {
        device->in_use = false;
        return;
    }

    bool boot = params.sub_class == HID_SUBCLASS_BOOT_INTERFACE;

    if (boot && params.proto == HID_PROTOCOL_KEYBOARD) {
        // The boot report is a fixed eight bytes and says everything TIC-80
        // asks of a keyboard, so there is nothing to gain from parsing one.
        hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT);
        hid_class_request_set_idle(handle, 0, 0);
        device->kind = DEVICE_KEYBOARD;
    } else {
        if (boot) {
            // Ask a boot mouse for its full reports: that is where the wheel
            // and the fourth and fifth buttons live.
            hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_REPORT);
        }

        uint16_t vid = 0, pid = 0;
        hid_host_dev_info_t info;
        if (hid_host_get_device_info(handle, &info) == ESP_OK) {
            vid = info.VID;
            pid = info.PID;
        }

        size_t         length     = 0;
        const uint8_t* descriptor = hid_host_get_report_descriptor(handle, &length);

        if (descriptor != NULL && hid_gamepad_open(&device->gamepad, descriptor, length, vid, pid)) {
            device->kind = DEVICE_GAMEPAD;
            nudge_gamepad(handle, &device->gamepad);
        } else if (descriptor != NULL && hid_layout_parse(descriptor, length, &device->layout) &&
                   ((device->layout.x.present && device->layout.x.relative) ||
                    (device->layout.y.present && device->layout.y.relative))) {
            // What separates a mouse from the rest is that it reports a change
            // rather than a position. Without that test the second interface
            // of a keyboard, whose keys parse as a pile of buttons, would be
            // taken for a mouse and click on every keystroke.
            device->kind = DEVICE_MOUSE;
        } else if (params.proto == HID_PROTOCOL_MOUSE) {
            device->kind = DEVICE_MOUSE_BOOT;
        } else {
            ESP_LOGI(TAG, "Nothing usable in the report descriptor, ignoring the device");
            hid_host_device_close(handle);
            device->in_use = false;
            return;
        }
    }

    static const char* const kind_name[] = {"none", "keyboard", "mouse", "boot mouse", "gamepad"};
    ESP_LOGI(TAG, "Device connected, using it as a %s", kind_name[device->kind]);

    hid_host_device_start(handle);
    post_device_list();
}

// The driver calls this from its own task, where opening a device would
// deadlock, so it only hands the event on.
static void driver_callback(hid_host_device_handle_t handle, const hid_host_driver_event_t event, void* arg) {
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED || driver_queue == NULL) {
        return;
    }
    xQueueSend(driver_queue, &handle, 0);
}

static void driver_task(void* arg) {
    hid_host_device_handle_t handle;
    while (xQueueReceive(driver_queue, &handle, portMAX_DELAY)) {
        device_connected(handle);
    }
    vTaskDelete(NULL);
}

static void usb_lib_task(void* arg) {
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };

    if (usb_host_install(&config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install the USB host library");
        xTaskNotifyGive((TaskHandle_t)arg);
        vTaskDelete(NULL);
        return;
    }

    started = true;
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

esp_err_t tanmatsu_usb_hid_init(void) {
    memset(devices, 0, sizeof(devices));
    memset(key_state, 0, sizeof(key_state));

    // Nothing enumerates without power on the port, and the port has none
    // until the coprocessor turns the boost converter on.
    esp_err_t res = bsp_power_set_usb_host_boost_enabled(true);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "Failed to power the USB port: %s", esp_err_to_name(res));
        return res;
    }

    event_queue  = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(event_t));
    driver_queue = xQueueCreate(MAX_DEVICES, sizeof(hid_host_device_handle_t));
    if (event_queue == NULL || driver_queue == NULL) {
        ESP_LOGE(TAG, "Out of memory");
        tanmatsu_usb_hid_deinit();
        return ESP_ERR_NO_MEM;
    }

    // Core 0: core 1 runs the audio task, and the main loop wants what is left
    // of it. USB is not busy enough to mind sharing with the rest.
    if (xTaskCreatePinnedToCore(usb_lib_task, "tic80-usb-lib", 4096, xTaskGetCurrentTaskHandle(), 2, NULL, 0) !=
        pdPASS) {
        tanmatsu_usb_hid_deinit();
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

    if (!started) {
        tanmatsu_usb_hid_deinit();
        return ESP_FAIL;
    }

    const hid_host_driver_config_t driver_config = {
        .create_background_task = true,
        .task_priority          = 5,
        .stack_size             = 4096,
        .core_id                = 0,
        .callback               = driver_callback,
        .callback_arg           = NULL,
    };

    res = hid_host_install(&driver_config);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install the HID driver: %s", esp_err_to_name(res));
        tanmatsu_usb_hid_deinit();
        return res;
    }

    if (xTaskCreatePinnedToCore(driver_task, "tic80-usb-hid", 4096, NULL, 4, NULL, 0) != pdPASS) {
        tanmatsu_usb_hid_deinit();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB HID host started");
    return ESP_OK;
}

void tanmatsu_usb_hid_deinit(void) {
    // Leaving the boost converter on would keep draining the battery after
    // TIC-80 has handed the device back to the launcher.
    bsp_power_set_usb_host_boost_enabled(false);
}

// ---------------------------------------------------------------------------
// Main loop side
// ---------------------------------------------------------------------------

void tanmatsu_usb_hid_poll(tanmatsu_usb_hid_state_t* out) {
    if (out == NULL) {
        return;
    }

    event_t event;
    while (event_queue != NULL && xQueueReceive(event_queue, &event, 0) == pdTRUE) {
        switch (event.type) {
            case EVENT_KEYBOARD:
                if (event.slot < MAX_DEVICES) {
                    apply_keyboard(event.slot, event.keyboard.modifier, event.keyboard.keys);
                }
                break;

            case EVENT_MOUSE:
                pending_dx     += event.mouse.dx;
                pending_dy     += event.mouse.dy;
                pending_scroll += event.mouse.wheel;
                mouse_left      = (event.mouse.buttons & (1u << 0)) != 0;
                mouse_right     = (event.mouse.buttons & (1u << 1)) != 0;
                mouse_middle    = (event.mouse.buttons & (1u << 2)) != 0;
                break;

            case EVENT_GAMEPAD:
                gamepad_state = event.gamepad.buttons;
                break;

            case EVENT_DEVICES:
                have_keyboard = event.devices.keyboard;
                have_mouse    = event.devices.mouse;
                have_gamepad  = event.devices.gamepad;
                for (size_t i = 0; i < MAX_DEVICES; i++) {
                    if ((event.devices.live & (1u << i)) == 0) {
                        memset(key_state[i], 0, sizeof(key_state[i]));
                    }
                }
                if (!have_mouse) {
                    mouse_left = mouse_middle = mouse_right = false;
                }
                if (!have_gamepad) {
                    gamepad_state = 0;
                }
                break;
        }
    }

    memset(out->keys, 0, sizeof(out->keys));
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        for (tic_key key = tic_key_unknown + 1; key < tic_keys_count; key++) {
            if (key_state[i][key]) {
                out->keys[key] = true;
            }
        }
    }

    out->keyboard = have_keyboard;
    out->mouse    = have_mouse;
    out->gamepad  = have_gamepad;

    // Divide with the remainder carried over, so slow movement still gets
    // there rather than being rounded away one report at a time.
    int32_t dx  = pending_dx + leftover_dx;
    int32_t dy  = pending_dy + leftover_dy;
    out->mouse_dx = dx / MOUSE_DIVISOR;
    out->mouse_dy = dy / MOUSE_DIVISOR;
    leftover_dx   = dx - out->mouse_dx * MOUSE_DIVISOR;
    leftover_dy   = dy - out->mouse_dy * MOUSE_DIVISOR;
    pending_dx    = 0;
    pending_dy    = 0;

    out->mouse_scroll = pending_scroll;
    pending_scroll    = 0;

    out->mouse_left   = mouse_left;
    out->mouse_middle = mouse_middle;
    out->mouse_right  = mouse_right;

    out->gamepad_buttons = gamepad_state;
}

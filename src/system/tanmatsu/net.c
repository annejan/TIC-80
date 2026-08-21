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

// HTTP for the Tanmatsu, which surf and the version check use.
//
// The radio is an ESP32-C6 reached over ESP-Hosted, and the networks are the
// ones the launcher already stored in NVS, so nothing is asked of the user
// here. Bringing WiFi up takes seconds, so it happens on its own task and
// requests made before it is ready simply fail, exactly as they would with no
// network.
//
// tic_net_get queues work and returns at once. A worker task performs the
// requests, and tic_net_end hands the results to their callbacks on the main
// task, which is where the studio expects to be called back.

#include "net.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bsp/power.h"
#include "wifi_connection.h"
#include "wifi_remote.h"

#define URL_SIZE       2048
#define MAX_REQUESTS   8
#define RESPONSE_LIMIT (4 * 1024 * 1024)

static char const TAG[] = "tic80-net";

typedef struct {
    char url[URL_SIZE];

    net_get_callback callback;
    void*            calldata;

    // Written by the worker, read by the main task once done is set. done is
    // the handover: everything else is stable by the time it becomes true.
    volatile bool done;
    s32           status;
    u8*           data;
    s32           size;
} HttpGet;

struct tic_net {
    char host[URL_SIZE];

    HttpGet*          requests[MAX_REQUESTS];
    SemaphoreHandle_t lock;
    QueueHandle_t     work;
    TaskHandle_t      worker;
};

// Set once the stack is up, whether or not a network was joined. Until then a
// request has to wait rather than fail, because associating takes seconds and
// the console is usable long before that.
static volatile bool wifi_settled = false;

// Only true once the connection manager is initialised. Its other entry points
// dereference an event group that init_stack creates, so calling them before
// that is a crash rather than a failure.
static volatile bool wifi_stack_up = false;

static void wifi_task(void* arg) {
    (void)arg;

    // The order matters and is the launcher's: power the radio into
    // application mode, hand the remote stack its transport, and only then
    // bring up the connection manager. Calling the connection manager cold
    // leaves its event group uncreated, and the first connect attempt then
    // asserts inside FreeRTOS.
    esp_err_t res = bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_APPLICATION);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "Could not power the radio: %s", esp_err_to_name(res));
        wifi_settled = true;
        vTaskDelete(NULL);
        return;
    }

    res = wifi_remote_initialize();
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "Could not reach the radio: %s", esp_err_to_name(res));
        wifi_settled = true;
        vTaskDelete(NULL);
        return;
    }

    res = wifi_connection_init_stack();
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "Could not start the network stack: %s", esp_err_to_name(res));
        wifi_settled = true;
        vTaskDelete(NULL);
        return;
    }

    wifi_stack_up = true;

    if (wifi_connect_try_all() != ESP_OK) {
        ESP_LOGW(TAG, "No network joined, TIC-80 stays offline");
    } else {
        ESP_LOGI(TAG, "Network %s", wifi_connection_is_connected() ? "up" : "unavailable");
    }

    wifi_settled = true;
    vTaskDelete(NULL);
}

// Waits for the radio, but only as long as a person would tolerate before
// deciding the thing is broken. Bringing the coprocessor up, associating and
// getting a lease took about 35 seconds from cold on the first try, so this has
// to cover more than the association alone.
#define WIFI_WAIT_MS 45000

// Starts the radio on first use. Only the worker task calls this, so there is
// one caller and no need to guard against two at once.
static void wifi_start_once(void) {
    static bool started = false;

    if (!started) {
        started = true;
        xTaskCreatePinnedToCore(wifi_task, "tic80-wifi", 8192, NULL, 4, NULL, 1);
    }
}

static bool network_available(void) {
    wifi_start_once();

    int waited = 0;

    // Wait for the connection manager to exist before asking it anything: its
    // entry points dereference an event group that init_stack creates.
    while (!wifi_settled && waited < WIFI_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(250));
        waited += 250;
    }

    if (!wifi_stack_up) {
        return false;
    }

    // wifi_connect_try_all() returns once it has asked to associate, not once
    // there is a working link, so waiting on it alone is not enough: the first
    // request used to fail seconds before the lease arrived. Wait for the
    // address, which is what actually makes a request possible.
    while (waited < WIFI_WAIT_MS) {
        if (wifi_connection_is_connected()) {
            return true;
        }

        wifi_connection_await(1000);
        waited += 1000;
    }

    return wifi_connection_is_connected();
}

static void perform(HttpGet* get) {
    esp_http_client_config_t config = {
        .url               = get->url,
        .timeout_ms        = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent        = "TIC-80",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        get->status = -1;
        return;
    }

    if (esp_http_client_open(client, 0) != ESP_OK) {
        get->status = -1;
        esp_http_client_cleanup(client);
        return;
    }

    s64 length = esp_http_client_fetch_headers(client);
    get->status = esp_http_client_get_status_code(client);

    if (get->status == 200 && length <= RESPONSE_LIMIT) {
        // A chunked response reports a length of zero, so grow as it arrives
        // rather than trusting the header.
        s32 capacity = length > 0 ? (s32)length : 16384;
        u8* buffer   = malloc(capacity);
        s32 size     = 0;

        while (buffer != NULL) {
            if (size == capacity) {
                if (capacity >= RESPONSE_LIMIT) {
                    free(buffer);
                    buffer = NULL;
                    break;
                }

                capacity *= 2;
                u8* grown = realloc(buffer, capacity);
                if (grown == NULL) {
                    free(buffer);
                    buffer = NULL;
                    break;
                }
                buffer = grown;
            }

            int read = esp_http_client_read(client, (char*)buffer + size, capacity - size);
            if (read < 0) {
                free(buffer);
                buffer = NULL;
                break;
            }
            if (read == 0) {
                break;
            }

            size += read;
        }

        if (buffer == NULL) {
            get->status = -1;
        } else {
            get->data = buffer;
            get->size = size;
        }
    } else if (get->status == 200) {
        get->status = -1;  // Too big to hold.
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static void worker_task(void* arg) {
    tic_net* net = arg;

    while (true) {
        HttpGet* get = NULL;
        if (xQueueReceive(net->work, &get, portMAX_DELAY) != pdTRUE || get == NULL) {
            continue;
        }

        if (network_available()) {
            perform(get);
        } else {
            ESP_LOGW(TAG, "No network, failing request for %s", get->url);
            get->status = -1;
        }

        get->done = true;
    }
}

tic_net* tic_net_create(const char* host) {
    tic_net* net = calloc(1, sizeof(tic_net));
    if (net == NULL) {
        return NULL;
    }

    strncpy(net->host, host, sizeof(net->host) - 1);

    net->lock = xSemaphoreCreateMutex();
    net->work = xQueueCreate(MAX_REQUESTS, sizeof(HttpGet*));

    if (net->lock == NULL || net->work == NULL ||
        xTaskCreatePinnedToCore(worker_task, "tic80-net", 8192, net, 4, &net->worker, 1) != pdPASS) {
        ESP_LOGW(TAG, "Could not start the network worker, TIC-80 stays offline");
    }

    // The radio is deliberately left alone until something asks for it. Bringing
    // it up resets the ESP32-C6 and takes seconds, and most of what people do
    // with TIC-80 never touches the network.

    return net;
}

void tic_net_get(tic_net* net, const char* url, net_get_callback callback, void* calldata) {
    if (net == NULL || net->work == NULL) {
        return;
    }

    HttpGet* get = calloc(1, sizeof(HttpGet));
    if (get == NULL) {
        return;
    }

    snprintf(get->url, sizeof(get->url), "%s%s", net->host, url);
    get->callback = callback;
    get->calldata = calldata;

    xSemaphoreTake(net->lock, portMAX_DELAY);

    s32 slot = -1;
    for (s32 i = 0; i < MAX_REQUESTS; i++) {
        if (net->requests[i] == NULL) {
            slot = i;
            break;
        }
    }

    if (slot >= 0) {
        net->requests[slot] = get;
    }

    xSemaphoreGive(net->lock);

    if (slot < 0 || xQueueSend(net->work, &get, 0) != pdTRUE) {
        // Nowhere to put it, so report the failure rather than dropping it and
        // leaving the caller waiting for a callback that never comes.
        if (slot >= 0) {
            xSemaphoreTake(net->lock, portMAX_DELAY);
            net->requests[slot] = NULL;
            xSemaphoreGive(net->lock);
        }

        net_get_data data = {
            .type       = net_get_error,
            .calldata   = calldata,
            .url        = get->url,
            .error.code = -1,
        };
        callback(&data);
        free(get);
    }
}

void tic_net_start(tic_net* net) {}

void tic_net_end(tic_net* net) {
    if (net == NULL) {
        return;
    }

    for (s32 i = 0; i < MAX_REQUESTS; i++) {
        xSemaphoreTake(net->lock, portMAX_DELAY);
        HttpGet* get = net->requests[i];
        bool     ready = get != NULL && get->done;
        if (ready) {
            net->requests[i] = NULL;
        }
        xSemaphoreGive(net->lock);

        if (!ready) {
            continue;
        }

        net_get_data data = {
            .calldata = get->calldata,
            .url      = get->url,
        };

        if (get->status == 200 && get->data != NULL) {
            data.type      = net_get_done;
            data.done.data = get->data;
            data.done.size = get->size;
        } else {
            data.type       = net_get_error;
            data.error.code = get->status;
        }

        get->callback(&data);

        free(get->data);
        free(get);
    }
}

void tic_net_close(tic_net* net) {
    if (net == NULL) {
        return;
    }

    if (net->worker) {
        vTaskDelete(net->worker);
    }

    for (s32 i = 0; i < MAX_REQUESTS; i++) {
        if (net->requests[i]) {
            free(net->requests[i]->data);
            free(net->requests[i]);
        }
    }

    if (net->work) {
        vQueueDelete(net->work);
    }
    if (net->lock) {
        vSemaphoreDelete(net->lock);
    }

    free(net);
}

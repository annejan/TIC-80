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

#include "tanmatsu.h"

#include <string.h>

#include "bsp/display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static char const TAG[] = "tic80-display";

// The panel is wider than TIC-80's 256x144 output, so the picture is scaled by
// a whole number of pixels and centred, with the rest of the panel left black.
// Nearest-neighbour on purpose: TIC-80 is a pixel machine and interpolation
// would only smear it.
static uint16_t* framebuffer  = NULL;
static size_t    panel_width  = 0;
static size_t    panel_height = 0;
static size_t    scale        = 1;
static size_t    scaled_width = 0;
static size_t    scaled_height = 0;
static size_t    offset_x     = 0;
static size_t    offset_y     = 0;
static bool      swap_bytes   = false;

// Height of the scratch band used to paint the letterbox once at startup.
#define CLEAR_BAND_HEIGHT 40

static inline uint16_t to_rgb565(uint32_t pixel) {
    // TIC80_PIXEL_COLOR_RGBA8888 lays the channels out as R,G,B,A in memory,
    // which on this little-endian core reads back as 0xAABBGGRR.
    uint32_t r = (pixel >> 0) & 0xFF;
    uint32_t g = (pixel >> 8) & 0xFF;
    uint32_t b = (pixel >> 16) & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void clear_panel(void) {
    size_t    band_pixels = panel_width * CLEAR_BAND_HEIGHT;
    uint16_t* band        = heap_caps_calloc(band_pixels, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (band == NULL) {
        ESP_LOGW(TAG, "No memory for the letterbox scratch buffer, skipping the clear");
        return;
    }

    for (size_t y = 0; y < panel_height; y += CLEAR_BAND_HEIGHT) {
        size_t rows = panel_height - y;
        if (rows > CLEAR_BAND_HEIGHT) {
            rows = CLEAR_BAND_HEIGHT;
        }
        bsp_display_blit(0, y, panel_width, rows, band);
    }

    heap_caps_free(band);
}

esp_err_t tanmatsu_display_init(void) {
    bsp_display_color_format_t color_format = 0;
    bsp_display_endianness_t   endianness   = BSP_DISPLAY_ENDIAN_LITTLE;

    esp_err_t res = bsp_display_get_parameters(&panel_width, &panel_height, &color_format, &endianness);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read the display parameters: %s", esp_err_to_name(res));
        return res;
    }

    if (color_format != BSP_DISPLAY_COLOR_FORMAT_16_565RGB) {
        ESP_LOGE(TAG, "Unsupported display colour format %d, this port needs RGB565", (int)color_format);
        return ESP_ERR_NOT_SUPPORTED;
    }

    swap_bytes = (endianness == BSP_DISPLAY_ENDIAN_BIG);

    size_t scale_x = panel_width / TIC80_FULLWIDTH;
    size_t scale_y = panel_height / TIC80_FULLHEIGHT;
    scale          = scale_x < scale_y ? scale_x : scale_y;
    if (scale < 1) {
        ESP_LOGE(TAG, "Display %ux%u is smaller than TIC-80's %ux%u output", (unsigned)panel_width,
                 (unsigned)panel_height, TIC80_FULLWIDTH, TIC80_FULLHEIGHT);
        return ESP_ERR_NOT_SUPPORTED;
    }

    scaled_width  = TIC80_FULLWIDTH * scale;
    scaled_height = TIC80_FULLHEIGHT * scale;
    offset_x      = (panel_width - scaled_width) / 2;
    offset_y      = (panel_height - scaled_height) / 2;

    framebuffer = heap_caps_aligned_calloc(64, scaled_width * scaled_height, sizeof(uint16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (framebuffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate the %ux%u framebuffer", (unsigned)scaled_width, (unsigned)scaled_height);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Panel %ux%u, drawing TIC-80 at %ux scale (%ux%u) offset %u,%u", (unsigned)panel_width,
             (unsigned)panel_height, (unsigned)scale, (unsigned)scaled_width, (unsigned)scaled_height,
             (unsigned)offset_x, (unsigned)offset_y);

    clear_panel();

    return ESP_OK;
}

void tanmatsu_display_present(const uint32_t* screen) {
    if (framebuffer == NULL || screen == NULL) {
        return;
    }

    for (size_t y = 0; y < TIC80_FULLHEIGHT; y++) {
        const uint32_t* src = screen + y * TIC80_FULLWIDTH;
        uint16_t*       row = framebuffer + (y * scale) * scaled_width;

        // Expand one source row horizontally...
        for (size_t x = 0; x < TIC80_FULLWIDTH; x++) {
            uint16_t colour = to_rgb565(src[x]);
            if (swap_bytes) {
                colour = (uint16_t)((colour >> 8) | (colour << 8));
            }
            uint16_t* out = row + x * scale;
            for (size_t i = 0; i < scale; i++) {
                out[i] = colour;
            }
        }

        // ...then copy it down, which is much cheaper than converting again.
        for (size_t i = 1; i < scale; i++) {
            memcpy(row + i * scaled_width, row, scaled_width * sizeof(uint16_t));
        }
    }

    bsp_display_blit(offset_x, offset_y, scaled_width, scaled_height, framebuffer);
}

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

// The Tanmatsu panel is a 480x800 portrait ST7701 that the BSP reports with a
// default rotation of 270 degrees, so the landscape picture people see is
// turned a quarter turn in panel memory. TIC-80 draws 256x144 pixels including
// the border; that is scaled by a whole number of pixels, rotated to match the
// panel, and centred, with the rest of the panel left black.
//
// Nearest-neighbour on purpose: TIC-80 is a pixel machine and interpolation
// would only smear it.
static uint16_t* framebuffer = NULL;

static size_t panel_width  = 0;
static size_t panel_height = 0;
static size_t scale        = 1;

// Size and position of the drawn area, in panel coordinates.
static size_t blit_width  = 0;
static size_t blit_height = 0;
static size_t offset_x    = 0;
static size_t offset_y    = 0;

// How a TIC-80 pixel lands in panel memory. One TIC-80 row or column always
// becomes one panel row, which is what makes the scaling a memcpy.
static bool transpose  = false;
static bool flip_outer = false;
static bool flip_inner = false;

static bool swap_bytes = false;

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
        bsp_display_blit(0, y, panel_width, y + rows, band);
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

    switch (bsp_display_get_default_rotation()) {
        case BSP_DISPLAY_ROTATION_90:  // Counter-clockwise
            transpose  = true;
            flip_outer = true;
            flip_inner = false;
            break;
        case BSP_DISPLAY_ROTATION_180:
            transpose  = false;
            flip_outer = true;
            flip_inner = true;
            break;
        case BSP_DISPLAY_ROTATION_270:  // Clockwise, which is what the Tanmatsu uses
            transpose  = true;
            flip_outer = false;
            flip_inner = true;
            break;
        case BSP_DISPLAY_ROTATION_0:
        default:
            transpose  = false;
            flip_outer = false;
            flip_inner = false;
            break;
    }

    // Rotating by a quarter turn swaps which TIC-80 axis has to fit which
    // panel axis.
    size_t needed_width  = transpose ? TIC80_FULLHEIGHT : TIC80_FULLWIDTH;
    size_t needed_height = transpose ? TIC80_FULLWIDTH : TIC80_FULLHEIGHT;

    size_t scale_x = panel_width / needed_width;
    size_t scale_y = panel_height / needed_height;
    scale          = scale_x < scale_y ? scale_x : scale_y;
    if (scale < 1) {
        ESP_LOGE(TAG, "Display %ux%u is smaller than TIC-80's %ux%u output", (unsigned)panel_width,
                 (unsigned)panel_height, (unsigned)needed_width, (unsigned)needed_height);
        return ESP_ERR_NOT_SUPPORTED;
    }

    blit_width  = needed_width * scale;
    blit_height = needed_height * scale;
    offset_x    = (panel_width - blit_width) / 2;
    offset_y    = (panel_height - blit_height) / 2;

    framebuffer = heap_caps_aligned_calloc(64, blit_width * blit_height, sizeof(uint16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (framebuffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate the %ux%u framebuffer", (unsigned)blit_width, (unsigned)blit_height);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Panel %ux%u, TIC-80 at %ux scale as %ux%u at %u,%u%s", (unsigned)panel_width,
             (unsigned)panel_height, (unsigned)scale, (unsigned)blit_width, (unsigned)blit_height, (unsigned)offset_x,
             (unsigned)offset_y, transpose ? ", rotated a quarter turn" : "");

    clear_panel();

    return ESP_OK;
}

void tanmatsu_display_present(const uint32_t* screen) {
    if (framebuffer == NULL || screen == NULL) {
        return;
    }

    // outer walks whichever TIC-80 axis maps to panel rows, inner walks the
    // one that maps along a row.
    const size_t outer_count = transpose ? TIC80_FULLWIDTH : TIC80_FULLHEIGHT;
    const size_t inner_count = transpose ? TIC80_FULLHEIGHT : TIC80_FULLWIDTH;

    for (size_t outer = 0; outer < outer_count; outer++) {
        size_t    row_index = flip_outer ? (outer_count - 1 - outer) : outer;
        uint16_t* row       = framebuffer + (row_index * scale) * blit_width;

        for (size_t inner = 0; inner < inner_count; inner++) {
            uint32_t pixel = transpose ? screen[inner * TIC80_FULLWIDTH + outer]
                                       : screen[outer * TIC80_FULLWIDTH + inner];

            uint16_t colour = to_rgb565(pixel);
            if (swap_bytes) {
                colour = (uint16_t)((colour >> 8) | (colour << 8));
            }

            size_t    column = flip_inner ? (inner_count - 1 - inner) : inner;
            uint16_t* out    = row + column * scale;
            for (size_t i = 0; i < scale; i++) {
                out[i] = colour;
            }
        }

        // Copying the finished row down beats converting it again.
        for (size_t i = 1; i < scale; i++) {
            memcpy(row + i * blit_width, row, blit_width * sizeof(uint16_t));
        }
    }

    // bsp_display_blit takes end coordinates, not a width and a height.
    bsp_display_blit(offset_x, offset_y, offset_x + blit_width, offset_y + blit_height, framebuffer);
}

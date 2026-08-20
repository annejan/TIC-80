# TIC-80 for Tanmatsu

TIC-80 running natively on the [Tanmatsu](https://nicolaielectronics.nl/tanmatsu/),
an ESP32-P4 palmtop with a QWERTY keyboard. The full studio is included:
console, code editor, sprite, map, SFX and music editors, and the surf browser.

## Hardware it uses

| Part | Driven through |
|------|----------------|
| 800x480 MIPI DSI panel | `bsp_display_blit()`, RGB565, 3x nearest-neighbour scale |
| Keyboard (TCA8418) | `bsp_input_get_queue()`, scancode and navigation events |
| ES8156 audio codec | `bsp_audio_*` plus a direct I2S write, 44100 Hz 16 bit stereo |
| SD card and internal FAT | ESP-IDF VFS, mounted at `/sd` and `/int` |

TIC-80 renders 256x144 pixels including the border. That is scaled 3x to
768x432 and centred on the panel, with the rest painted black once at startup.

## Building

Needs ESP-IDF v6.0.2 and the ESP32-P4 toolchain.

```sh
cd src/system/tanmatsu/project
make prepare        # only once: clones ESP-IDF into ./esp-idf
make build
make flashmonitor   # or: make install && make run, to go through badgelink
```

If ESP-IDF is already installed elsewhere, point at it instead of running
`make prepare`:

```sh
echo /path/to/esp-idf > .IDF_PATH
echo $HOME/.espressif > .IDF_TOOLS_PATH
```

The build produces `build/tanmatsu/tic80.bin`, around 1 MB, which fits both the
2 MB OTA slots and the 8 MB AppFS partition.

## Controls

The keyboard works as you would expect in the editors. Since the Tanmatsu has
no pointing device and half of TIC-80 is mouse driven, the **Fn** key turns the
arrow cluster into a pointer:

| Keys | What happens |
|------|--------------|
| Fn + arrows | Move the pointer; it accelerates while held |
| Fn + left space | Left click |
| Fn + right space | Right click |
| Fn + shift + up/down | Scroll wheel |
| Fn + middle space | Latch pointer mode, so Fn need not be held |
| Fn + Esc | Leave TIC-80 and go back to the launcher |
| Volume up/down | Codec volume, separate from TIC-80's own volume setting |

Gamepad input follows TIC-80's usual keyboard mapping: arrows for the d-pad,
`Z` `X` `A` `S` for A, B, X and Y.

## Where files live

Carts and configuration go in `/sd/tic80` when an SD card is mounted, and in
`/int/tic80` on internal flash when there is none. Both are FAT, so a card can
be filled with `.tic` files from a desktop.

## What is not here

- **Lua only.** The other TIC-80 languages are left out to keep the binary and
  the memory footprint down. MoonScript and Fennel run on the same Lua VM and
  would be cheap to add.
- **No network.** `tic_net` is compiled as the stub that fails every request,
  so surf browses local files but cannot reach tic80.com. Wiring it to
  `esp_http_client` is the obvious next step.
- **No FFT.** There is no audio input path, so the `fft()` API is compiled out
  the same way it is on the other embedded ports.
- **No CRT shader.** There is no GPU path; the software renderer draws
  straight into the framebuffer.

## How the port is put together

| File | Job |
|------|-----|
| `tic80_tanmatsu.c` | Entry point, main loop, and the `tic_sys_*` hooks the studio needs |
| `display.c` | Framebuffer allocation, RGBA8888 to RGB565, scaling, blitting |
| `audio.c` | I2S output; the blocking write is what paces the loop at 60 Hz |
| `keymap.c` | Scancodes to `tic_key`, typed text, and the emulated pointer |
| `storage.c` | Mounts internal flash and the SD card, picks the cart directory |
| `CMakeLists.txt` | ESP-IDF component; source lists come from `cmake/tanmatsu.cmake` |
| `linker.lf` | Puts TIC-80's 812 KB of static buffers in PSRAM |
| `project/` | The ESP-IDF project that builds the component into an app |

TIC-80's own CMake is not used: ESP-IDF drives its own build, so
`cmake/tanmatsu.cmake` holds the source lists and both builds read the same
file.

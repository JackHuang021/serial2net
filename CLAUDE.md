# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash

```bash
# ESP-IDF v6.0.2, installed at ~/.espressif/v6.0.2/esp-idf/
source ~/.espressif/v6.0.2/esp-idf/export.sh

idf.py set-target esp32s3    # or esp32c3, esp32
idf.py menuconfig            # configure WiFi SSID/password, UART pins, LED pin
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

On a fresh machine, run `~/.espressif/v6.0.2/esp-idf/install.sh` first.

## Architecture

Single-file embedded application (`main/serial2net.c`) running on bare-metal FreeRTOS. 5 concurrent tasks:

| Task | Priority | Role |
|------|----------|------|
| `led` | 5 | 20 Hz LED state machine (color patterns) |
| `tcp_accept` | 9 | Block on `accept()`, kick old client on new connection |
| `uart2tcp` | 10 | `uart_read_bytes()` → `send()` to client socket |
| `tcp2uart` | 10 | `recv()` from client socket → `uart_write_bytes()` |
| main (app_main) | — | Init, then 30s heartbeat logging |

**Data flow**: UART2 ↔ two independent FreeRTOS tasks ↔ single TCP client socket. Full-duplex transparent bridge, no protocol framing.

**WiFi**: STA+AP fallback by default — tries STA for 15s, falls back to AP `serial2net`. Controlled by `CONFIG_SERIAL2NET_WIFI_MODE_*` preprocessor blocks (not runtime config).

**TCP_NODELAY** is set on both listen and accepted sockets to disable Nagle's algorithm — critical for low-latency keystroke forwarding.

## ESP-IDF v6.0.2 specifics

- Component `driver` was split: UART is now `esp_driver_uart`, GPIO is `esp_driver_gpio`. Include paths remain `driver/uart.h`.
- `mdns` and `led_strip` are **managed components** (ESP Registry), not built-in. Declared in `main/idf_component.yml`, resolved by the component manager during build.
- Project was converted from `MINIMAL_BUILD` hello_world — MINIMAL_BUILD was removed to enable full lwIP/WiFi stacks.
- `sdkconfig` is gitignored (generated); `sdkconfig.ci` is committed for CI reference.

## Portability

Code uses only standard ESP-IDF APIs. Switching chip target:

```bash
idf.py set-target esp32c3
# Then adjust pin defaults in main/Kconfig.projbuild if needed
```

Kconfig pin defaults are conditional on `IDF_TARGET` (`default 17 if IDF_TARGET_ESP32S3`, etc.).

## Client-side usage

```bash
# Recommended: low latency, Ctrl+] to exit
socat -,raw,echo=0,escape=0x1d TCP:serial2net.local:8880,nodelay

# Minimal: lowest latency, Ctrl+C to exit (but no tab completion)
nc serial2net.local 8880
```

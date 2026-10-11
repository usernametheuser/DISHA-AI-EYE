# DISHA-CAM firmware

ESP32-S3-N16R8 + GC2145 camera → JPEG frames over the native USB port to the DISHA GLASS OS app. Wi-Fi and Bluetooth stay off. Setup, wiring and tests: see the main `README.md`. Wire format: `docs/PROTOCOL.md`.

Flash with exact settings: `powershell -ExecutionPolicy Bypass -File tools\flash_firmware.ps1 -Port COMx` (from the project root).

## Files

| File | Purpose |
|---|---|
| `DISHA-CAM.ino` | Boot sequence, required Tools settings, status lights, periodic summary |
| `src/config/disha_config.h` | Board pins (from your working S3_CAM.ino) and tunable defaults |
| `src/camera/camera_service.*` | Camera task: init + sensor check, capture, RGB565 → JPEG, streaming, resolution change, recovery |
| `src/usb/usb_link.*` | Native USB: TinyUSB vendor interface "DISHA-CAM Link" (primary) or USB Serial/JTAG CDC (fallback) |
| `src/protocol/disha_protocol.h` | Protocol constants shared with the app |
| `src/protocol/protocol_io.*` | Message framing, CRCs, frame chunking, phone→device parser, golden-vector self-test |
| `src/protocol/crc.*` | CRC-16/CCITT-FALSE and CRC-32 with self-test |
| `src/command/command_handler.*` | Link task: sessions, command dispatch, keep-alive timeout, log forwarding |
| `src/status/status_report.*` | HELLO_ACK and STATUS JSON, reset reason |
| `src/diagnostics/log.*` | Serial log, forwarded to the app during a session |
| `src/diagnostics/serial_console.*` | Serial commands |
| `src/diagnostics/status_led.*` | RGB LED and IO2 privacy LED |

| `src/storage/sd_storage.*` | microSD: mount (never formats), space, numbered photos, recording with auto-split, file list |
| `src/storage/avi_writer.*` | MJPEG AVI writer with periodic header updates (power-cut tolerant) |

## microSD

- The card must be FAT32. A 32 GB card normally is. The firmware **never formats** a card; if it can't read one, the status says why.
- Files: `/DISHA/IMG_nnnnn.jpg` and `/DISHA/VID_nnnnn.avi`. Numbering continues after the highest existing number and never overwrites.
- Recording continues in a new file when the resolution changes or a file reaches 1 GB. It stops by itself below 20 MB free.
- **BOOT button:** short press = photo, hold 1.5 s = start/stop video.

## Serial console (USB-UART port, 115200 baud, Newline)

| Command | Result |
|---|---|
| `help` | list commands |
| `status` | status JSON |
| `snap` | capture + encode one frame, print size, timing, CRC |
| `snapb64` | one frame as base64 (used by `tools/serial_snap.ps1`) |
| `fps` | capture+encode speed over 20 frames (no USB) |
| `res 320` / `res 640` / `res 800` | change resolution |
| `flip` | toggle vertical flip |
| `sd` | microSD status JSON |
| `photo` | photo to the microSD card |
| `rec` | start/stop microSD video |
| `ls` | list files on the card |
| `reboot` | restart |

## Lights

| RGB LED "48" | Meaning |
|---|---|
| yellow | starting / camera initialising |
| green | camera ready, no phone session |
| cyan | phone app connected |
| blue | streaming |
| purple | recording to the microSD card |
| red | camera error (see log) |

LED **IO2** is on while camera frames leave the camera (to the phone or the card).

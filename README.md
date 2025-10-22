# Konnichiwa Sekai

A kind of, sort of, hello world (こんにちは世界 !) to display Japanese text on a tiny LED matrix, wrapped in a PlatformIO project.

## Overview
This project runs on an ESP32 and displays scrolling Japanese text (UTF-8) on a 32x8 LED matrix using the U8g2 library. It fetches quotes from a backend API and supports WiFi configuration via a captive portal.

## Hardware
- ESP32 development board (tested with `esp32dev`)
- 32x8 LED matrix (MAX7219-based)
- Connections:
  - CLK_PIN: GPIO 14
  - CS_PIN: GPIO 12
  - DATA_PIN: GPIO 13

## Features
- Scrolling Japanese text using a custom font (`misakimincho.h`)
- Fetches quotes from a remote API and displays them
- WiFi credentials and API server settings are stored in flash (Preferences)
- If configuration is missing or WiFi fails, device enters AP mode with a captive portal for setup
- Web-based configuration page for WiFi and API settings

## Backend API
Quotes are fetched from a backend service compatible with [muskeg/quote-api](https://github.com/muskeg/quote-api). The device expects a JSON response like:

```json
{
  "quote": "Some 'inspirational' message or whatever."
}
```

## Setup & Build
1. Install [PlatformIO](https://platformio.org/) (VS Code recommended)
2. Clone this repository
3. Connect your ESP32 and LED matrix as described above
4. Build and upload:
   ```
   pio run -t upload
   ```
5. Open the serial monitor at 115200 baud:
   ```
   pio device monitor -b 115200
   ```

## Configuration
- On first boot (or if WiFi fails), the device starts in AP mode (`KonnichiwaSetup`).
- Connect to this WiFi network and open any browser (captive portal will redirect you).
- Enter your WiFi SSID, password, API server host, port, and path.
- Save and the device will reboot and attempt to connect.

## Serial Commands
- Send a line over serial to change the displayed text
- `/invert on|off|toggle` to invert display colors

## Libraries Used
- [U8g2](https://github.com/olikraus/u8g2) for LED matrix graphics
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) for JSON parsing
- [WiFiManager](https://github.com/tzapu/WiFiManager) (listed, but not used in current code)

## Custom Font
- `src/misakimincho.h` contains a Japanese font for rendering on the matrix


---
For questions or improvements, open an issue or PR.
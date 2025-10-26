# Konnichiwa Sekai

A kind of, sort of, hello world (こんにちは世界 !) to display Japanese text on a tiny LED matrix, wrapped in a PlatformIO project.

## Overview
This project runs on an ESP32 and displays scrolling Japanese text (UTF-8) on a 32x8 LED matrix using the U8g2 library. It fetches quotes from a backend API and supports WiFi configuration via a captive portal.
A lot of credits goes to M-Factory Osaka and their [ESPTimeCast project](https://github.com/mfactory-osaka/ESPTimeCast) for inspiration and case models.

<img width="1749" height="1317" alt="image" src="https://github.com/user-attachments/assets/142a3d3c-3202-4724-b8e6-288ff5a37e2c" />


## Hardware
- ESP32 development board (tested with `esp32dev`)
- 32x8 LED matrix (MAX7219-based)
- 3D-printed case, based on [ESPTimeCast's models (purchase links on their repo)](https://github.com/mfactory-osaka/ESPTimeCast?tab=readme-ov-file#-3d-printable-case)

### Connections
Adjust pins according to your device pinouts. 

Code is currently using:
  - CLK_PIN: GPIO 14
  - CS_PIN: GPIO 12
  - DATA_PIN: GPIO 13

<img width="542" height="659" alt="image" src="https://github.com/user-attachments/assets/d9169f3b-b596-444f-99ca-7192aa88182e" />

_VIN and GND pins respectively used for 5V and Ground_
    

## Features
- Scrolling Japanese text using a custom font (`misakimincho.h`)
- Fetches quotes from a remote API and displays them
- WiFi credentials and API server settings are stored in flash (Preferences)
- If configuration is missing or WiFi fails, device enters AP mode with a captive portal for setup
- Web-based configuration page for WiFi and API settings
- Automatic cookie handling for API authentication and session management

## Backend API
Quotes are fetched from a backend service compatible with [muskeg/quote-api](https://github.com/muskeg/quote-api). The device expects a JSON response like:

```json
{
  "quote": "Some 'inspirational' message or whatever."
}
```

## Setup & Build
1. Install [PlatformIO](https://platformio.org/)
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
- On first boot (or if WiFi fails), the device starts in AP mode.
- Connect to the `KonnichiwaSetup` WiFi network with password: `konnichiwa`
- open any browser (captive portal will redirect you).
- Enter your WiFi SSID, password, API server host, and adjust the settings to your liking.
- Save and the device will reboot and attempt to connect.

## Serial Commands
- Send a line over serial to change the displayed text
- `/invert on|off|toggle` to invert display colors
- `/config` will put the device in AP mode to reconfigure settings

## Libraries Used
- [U8g2](https://github.com/olikraus/u8g2) for LED matrix graphics
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) for JSON parsing

## Custom Font
- `src/misakimincho.h` contains a Japanese font for rendering on the matrix


---

For questions or improvements, open an issue or PR.


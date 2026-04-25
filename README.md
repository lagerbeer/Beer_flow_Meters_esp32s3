# Beer Flow Monitor — ESP32-S3

A 6-tap kegerator / keezer flow monitor built on the **ESP32-S3**. Tracks every pour in real time, serves a live web dashboard, publishes to MQTT, and sends Slack/Discord alerts — all from a single embedded device.

---

## Features

### Flow Monitoring
- **6 independent flow meter inputs** via GPIO interrupt (ISR) with software debounce
- Pulse-based pour detection with a configurable noise gate (`POUR_MIN_PULSES`)
- Per-tap **calibration** — pour a known volume and the device calculates pulses/oz automatically
- Live pour display: current pour size updates every 2 s while the tap is open
- Pour history (last 50 pours per tap) stored in RAM, exportable as CSV

### Web Interface
| URL | Description |
|-----|-------------|
| `http://<ip>/` | **Dashboard** — live tap cards, calibration, keg management |
| `http://<ip>/menu` | **Tap Room Menu** — Raspberrypints-style public display board |

#### Dashboard (`/`)
- Live keg level bars and pour-in-progress indicator per tap
- Edit **beer name**, **ABV %**, **IBU**, and **beer color** (hex color picker)
- **Calibrate** a tap: guided prompt — pour a measured amount, enter oz
- **Reset Keg** to full
- Sensor health / diagnostics panel
- Pour history table with CSV download
- Notification settings (Slack, Discord, generic webhook)

#### Tap Room Menu (`/menu`)
- Raspberrypints-inspired full-screen display — ideal for a TV or tablet
- Columns: **Tap #**, **Beer Color** (pint glass, user-selected color), **Beer Name**, **ABV %** (pint glass fill), **Bitterness** (IBU bar gauge), **Keg Level** (SVG keg graphic), **Status**
- SVG keg graphic with color-coded fill: green → yellow → orange → red → empty
- Green hop cone decorations in header corners
- Auto-refreshes every 30 seconds
- Links back to the dashboard

### Persistence (NVS)
All configuration survives reboots — stored in ESP32 Non-Volatile Storage:
- Beer name, ABV, IBU, beer color per tap
- Tap name, keg size, calibration (pulses/oz)
- Current keg level per tap
- Notification URLs and thresholds

### MQTT
- Publishes tap status every 30 s to `beermonitor/stats`
- Per-pour events on `beermonitor/tap/<N>/pour`
- Availability topic: `beermonitor/availability` (`online` / `offline`)
- Auto-reconnects if the broker drops

### Notifications
- **Slack** incoming webhook
- **Discord** webhook
- **Generic HTTP webhook** (JSON POST)
- Events: boot, pour completed, low keg alert, keg empty, keg reset, sensor error

### CSV Logging
- Pour events logged to LittleFS (internal flash) as CSV
- Downloadable directly from the dashboard
- Keg snapshot on reset

### TFT Display (optional)
- 480 × 320 ST7789 via SPI (LovyanGFX)
- Idle screen: 6 tap panels with keg-level arc gauges
- Pour animation when a tap opens
- Pour-complete summary with ounces and duration
- Low-keg alert overlay

### OTA Updates
- ArduinoOTA on port 3232
- Flash new firmware over Wi-Fi without a USB cable (`pio run -e ota --target upload`)

---

## Hardware

### Bill of Materials

| Qty | Component |
|-----|-----------|
| 1 | ESP32-S3-DevKitC-1 (8 MB flash) |
| 6 | YF-S201 or similar Hall-effect flow sensors |
| 1 | ST7789 480×320 TFT display (optional) |
| 1 | 5 V power supply (≥ 1 A) |
| — | Pull-up resistors if sensors need them (many boards have internal pull-ups) |

### Wiring

#### Flow Sensors → ESP32-S3

| Tap | GPIO |
|-----|------|
| 1 | 5 |
| 2 | 1 |
| 3 | 2 |
| 4 | 15 |
| 5 | 16 |
| 6 | 17 |

Flow sensor signal wires connect to these GPIOs. Each pin is configured as `INPUT_PULLUP`. Power the sensors from 5 V; signal lines are 3.3 V compatible on the S3.

#### TFT Display → ESP32-S3 (SPI)

| TFT Pin | ESP32-S3 GPIO |
|---------|---------------|
| MOSI | 11 |
| SCLK | 12 |
| CS | 10 |
| DC | 8 |
| RST | 9 |
| BL (backlight) | 45 |

---

## Software Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- ESP32 Arduino core (installed automatically by PlatformIO)

### Library Dependencies (auto-installed by PlatformIO)

| Library | Purpose |
|---------|---------|
| `knolleary/PubSubClient` | MQTT client |
| `bblanchon/ArduinoJson` | JSON serialisation |
| `adafruit/Adafruit GFX Library` | Graphics primitives |
| `adafruit/Adafruit BusIO` | SPI/I²C abstraction |
| `lovyan03/LovyanGFX` | TFT display driver |

---

## Installation

### 1. Clone the repository

```bash
git clone https://github.com/<your-username>/Beer_flow_Meters_esp32s3.git
cd Beer_flow_Meters_esp32s3
```

### 2. Create your credentials file

Copy the example and fill in your details:

```bash
cp src/credentials.h.example src/credentials.h
```

Edit `src/credentials.h`:

```cpp
#define WIFI_SSID       "your_network"
#define WIFI_PASSWORD   "your_password"
#define WIFI_HOSTNAME   "beer-flow-monitor"
#define WIFI_USE_DHCP   false          // true = DHCP, false = static IP
#define STATIC_IP       "192.168.0.250"
#define GATEWAY         "192.168.0.1"
#define SUBNET          "255.255.255.0"

#define OTA_PASSWORD    "your_ota_password"

#define MQTT_SERVER     "192.168.0.191"
#define MQTT_PORT       1883
#define MQTT_USER       "your_mqtt_user"
#define MQTT_PASSWORD   "your_mqtt_password"
#define MQTT_CLIENT_ID  "beerflow_monitor"

#define DEFAULT_WEBHOOK_URL ""   // leave empty to disable
```

> **`credentials.h` is listed in `.gitignore` and will never be committed.**

### 3. Adjust configuration (optional)

`src/config.h` — tunable constants:

| Define | Default | Description |
|--------|---------|-------------|
| `NUM_TAPS` | `6` | Number of flow meter inputs |
| `KEG_SIZE_OZ` | `640` | Default keg capacity (oz) — ~5 US gallons |
| `LOW_KEG_ALERT_OZ` | `100` | Threshold for low-keg alert |
| `PULSE_DEBOUNCE_MS` | `50` | ISR debounce window (ms) |
| `POUR_TIMEOUT_MS` | `5000` | Silence after which a pour is finalised (ms) |
| `POUR_MIN_PULSES` | `3` | Pulses before a pour is declared (noise gate) |
| `TAP_PINS` | `{5,1,2,15,16,17}` | GPIO pins for each tap |
| `LOG_LEVEL` | `DEBUG` | Set to `INFO` or `WARN` for production |

### 4. Build and flash

**USB (first flash):**
```bash
pio run -e esp32-s3-devkitc-1 --target upload
```

**OTA (subsequent updates):**
```bash
pio run -e ota --target upload
```

**Serial monitor:**
```bash
pio device monitor
```

---

## Usage

### First Boot
1. Device connects to Wi-Fi and prints its IP address on the serial monitor.
2. Navigate to `http://<ip>/` in a browser.
3. Each tap card shows **Tap N** with default beer name "Unknown".

### Setting Up a Tap
1. Enter a **beer name** in the text field on the tap card.
2. Set **ABV %** (e.g. `5.2`), **IBU** (e.g. `35`), and pick a **beer color** using the color picker.
3. Click **Save**.

### Calibrating a Tap
1. Click **Calibrate** on the tap card.
2. When the dialog appears — **pour a precisely measured amount** (e.g. exactly 8 oz / 236 ml) — then click OK.
3. Enter the exact number of ounces you poured and click OK.
4. The device calculates and saves the pulses/oz ratio.

> Tip: use a measuring cup and pour slowly for the most accurate result.

### Resetting a Keg
Click **Reset Keg** to mark the keg as full again. This also logs a snapshot to the CSV log.

### Notifications
Under **Settings** on the dashboard, enter Slack and/or Discord webhook URLs and enable them. Test buttons send a sample message immediately.

---

## API Reference

All endpoints are on port 80. POST bodies use `application/x-www-form-urlencoded`.

### GET endpoints

| Endpoint | Response |
|----------|---------|
| `GET /` | Dashboard HTML |
| `GET /menu` | Tap Room Menu HTML |
| `GET /api/status` | Full tap status JSON |
| `GET /api/last_pour?index=<N>` | Last pour event for tap N |
| `GET /api/history?index=<N\|all>` | Pour history JSON |
| `GET /api/diagnostics` | Sensor health JSON |

#### `/api/status` response shape (per tap)

```json
{
  "taps": [{
    "index": 0,
    "name": "Tap 1",
    "beer": "Heady Topper",
    "abv": 8.0,
    "ibu": 75,
    "beerColor": "#e8a020",
    "level": 480.5,
    "capacity": 640,
    "pulsesPerOz": 425.0,
    "isPouring": false,
    "currentPour": 0.0,
    "pourCount": 12,
    "lowKegAlert": false,
    "sensorHealth": "Good",
    "pulseQuality": 92,
    "daysLeft": 8.5
  }]
}
```

### POST endpoints

| Endpoint | Parameters | Description |
|----------|-----------|-------------|
| `POST /api/beer/update` | `index`, `name`, `abv`, `ibu`, `color` | Update beer details |
| `POST /api/reset` | `index` | Reset keg to full |
| `POST /api/calibrate/start` | `index` | Begin calibration |
| `POST /api/calibrate/end` | `index`, `ounces` | Finish calibration |
| `POST /api/alert/clear` | `index` | Clear low-keg alert |
| `POST /api/settings/alert` | `threshold` | Set low-keg threshold (oz) |
| `POST /api/settings/webhook` | `url` | Set generic webhook URL |
| `POST /api/settings/profile` | `tap`, `profile` | Apply keg size profile |
| `POST /api/notifications/config` | `slackUrl`, `slackEnabled`, `discordUrl`, `discordEnabled` | Save notification settings |

---

## MQTT Topics

| Topic | Direction | Payload |
|-------|-----------|---------|
| `beermonitor/availability` | publish | `online` / `offline` |
| `beermonitor/stats` | publish | Full status JSON (every 30 s) |
| `beermonitor/tap/<N>/pour` | publish | Pour event JSON |
| `beermonitor/tap/<N>/command` | subscribe | `RESET` |

---

## Serial Debug Commands

With the serial monitor open at 115200 baud, send single-character commands:

| Key | Action |
|-----|--------|
| `s` | Print pin states |
| `t` | Print tap status summary |

---

## Project Structure

```
Beer_flow_Meters_esp32s3/
├── src/
│   ├── main.cpp                  # Web server, routing, OTA, app loop
│   ├── TapManager.h/.cpp         # Flow counting, calibration, pour state machine
│   ├── DiagnosticsManager.h/.cpp # Sensor health scoring
│   ├── DisplayManager.h/.cpp     # TFT display (LovyanGFX)
│   ├── NotificationManager.h/.cpp# Slack / Discord / webhook alerts
│   ├── SDManager.h/.cpp          # CSV logging to LittleFS
│   ├── config.h                  # Tunable constants
│   ├── pins.h                    # TFT pin definitions
│   ├── credentials.h             # ⚠ NOT committed — copy from .example
│   └── credentials.h.example     # Template — fill in and rename
├── platformio.ini                # Build environments (USB + OTA)
└── README.md
```

---

## Calibration Notes

The YF-S201 sensor is rated at ~450 pulses/litre (≈ 450 pulses/34 oz ≈ **13 pulses/oz**) at low flow, but varies significantly between units and at different flow rates. Always calibrate with your actual hardware.

A good target is a consistent reading within ±2 % of the measured volume across 3 pours after calibration.

---

## License

MIT — do whatever you like, brew responsibly.

# Bline — Tri-Screen Motorcycle Navigation Dashboard

Operating on the **Waveshare ESP32-S3-Touch-LCD-1.28** (GC9A01 240×240 circular LCD, CST816S capacitive touch, and ESP32-S3R2 with 8MB Octal-SPI PSRAM), the system provides a smartwatch-style tri-screen dashboard:

1. **Screen 1 (Mini-Map HUD)**: Royal Enfield style heading-up rotated raster mini-map (MapTiler Dark) streamed asynchronously from the companion Android app, vehicle marker anchored at $(x=120, y=160)$, overlaid with dark-scrim HUD indicators for street name marquee, maneuver glyph, countdown distance, and live GPS speed.
2. **Screen 2 (Turn-by-Turn Vector Ribbon)**: Beeline Moto II style high-contrast monochrome vector road ribbon, dynamic bend geometry, distance countdown typography, and 228px circular progress arc for direct sunlight legibility.
3. **Screen 3 (Classic Retro Chrono Gauge & Cockpit Telemetry)**: Classic circular analog-style speedometer dial (0–120 km/h) with sweeping red digital needle, prominent digital center speed, cardinal compass heading ring (N/NE/E...), accumulated trip odometer, ride clock, and an upcoming turn notification banner when an active turn is $< 150\text{ m}$.

---

## 1. Hardware Pinouts & Wiring (ESP32-S3-Touch-LCD-1.28)

### GC9A01 Circular LCD (SPI)
- **LCD_DC**: GPIO 8
- **LCD_CS**: GPIO 9
- **LCD_CLK**: GPIO 10
- **LCD_MOSI**: GPIO 11
- **LCD_RST**: GPIO 12
- **LCD_BL**: GPIO 40 (Backlight PWM)

### CST816S Capacitive Touch (I2C)
- **TOUCH_SDA**: GPIO 6
- **TOUCH_SCL**: GPIO 7
- **TOUCH_RST**: GPIO 13 (Hardware reset pulse: LOW 20ms, HIGH 50ms on boot)
- **TOUCH_INT**: GPIO 5

---

## 2. Firmware Build Configuration (ESP32-S3)

### Arduino IDE Settings
- **Board**: `ESP32S3 Dev Module`
- **USB CDC On Boot**: `Enabled`
- **CPU Frequency**: `240MHz (WiFi)`
- **Flash Size**: `8MB (64Mb)`
- **Flash Mode**: `QIO 80MHz`
- **Partition Scheme**: `8M with spiffs (3MB APP/1.5MB SPIFFS)` or `default_8MB`
- **PSRAM**: **`OPI PSRAM`** *(Mandatory — uses 8MB Octal SPI at 80 MHz)*
- **Arduino Runs On**: `Core 1`
- **Events Run On**: `Core 1`

### Arduino CLI FQBN
```bash
esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,LoopCore=1
```

### Required Libraries
Install the following libraries via the Arduino Library Manager:
- **TFT_eSPI** (v2.5+): Configure `User_Setup_Select.h` to include `Setup302_Waveshare_ESP32S3_GC9A01.h`
- **lvgl** (v8.3.11): Standard `lv_conf.h` with `LV_FONT_MONTSERRAT_14`, `LV_FONT_MONTSERRAT_28`, and `LV_FONT_MONTSERRAT_48` enabled
- **TJpg_Decoder** (v1.1+ by Bodmer): Decodes JPEG tiles into PSRAM double buffers
- **ArduinoJson** (v7.x): Deserializes JSON telemetry
- **Wire & WiFi**: Standard ESP32 core libraries

### Local Configuration
Copy `secrets.h.example` to `secrets.h` and configure hotspot credentials:
```cpp
#define WIFI_SSID "YOUR_PHONE_HOTSPOT"
#define WIFI_PASS "YOUR_PASSWORD"
#define NAV_SERVER_HOST ""   // Leave empty to auto-discover phone gateway IP; or laptop IP for stub
#define NAV_SERVER_PORT 8080
```

---

## 3. FreeRTOS Dual-Core Architecture

- **Core 0 (`TaskNetwork`)**: Priority 3, Stack 12 KB. Exclusively owns Wi-Fi connectivity, HTTP polling (`/map` and `/nav`), and TJpgDec decoding into external PSRAM double buffers (`mapBufPing`, `mapBufPong`). Performs explicit L1 cache writebacks (`esp_cache_msync()`) before posting descriptors to Core 1 via FreeRTOS overwrite queues (`frameQueue`, `navQueue`). Core 0 automatically suspends `/map` requests when Screen 2 or 3 is active to preserve phone battery and radio bandwidth.
- **Core 1 (`TaskUI`)**: Priority 1. Dedicated entirely to LVGL rendering, CST816S touch gesture polling at 20 Hz, and SPI DMA display flushes. **Core 1 performs ZERO blocking network calls.**

---

## 4. Touch Gestures & Tri-Screen Carousel

- **Swipe Left (`0x04`)**: Advance screen forward: `Screen 1 (Map) -> Screen 2 (Ribbon) -> Screen 3 (Chrono)`.
- **Swipe Right (`0x03`)**: Retreat screen backward: `Screen 3 (Chrono) -> Screen 2 (Ribbon) -> Screen 1 (Map)`.
- **Failover Hysteresis**: If `/map` fetches fail continuously for $> 6\text{ seconds}$ while on Screen 1, the UI automatically transitions to Screen 2. Core 0 continues background low-frequency probes every 5 seconds. Upon 2 consecutive successful frames, Screen 1 is restored automatically.

---

## 5. Laptop Simulation Stub (`tools/nav_stub.py`)

Run the simulation server on your laptop to test display hardware without needing the phone:

```bash
# Normal demo mode (cycles maneuvers, live speed, Doppler bearing, and rotating map)
python3 tools/nav_stub.py --demo

# Test 6-second failover and automatic recovery
python3 tools/nav_stub.py --demo --fail-map
```

### Runtime Failover Testing:
While `nav_stub.py` is running:
- Trigger failover to Screen 2: `curl http://localhost:8080/fail-map?on=1`
- Restore map stream to Screen 1: `curl http://localhost:8080/fail-map?on=0`

---

## 6. Android Companion Application

### CLI Build (No Android Studio required)
Requires JDK 17+ and Android SDK command-line tools:
```bash
cd android
# Configure MapTiler API Key in android/secrets.properties
echo "MAPTILER_API_KEY=YOUR_MAPTILER_API_KEY" > secrets.properties

# Build debug APK
./gradlew assembleDebug

# Install on USB-connected phone
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### Phone Permissions & Setup
1. Open **Bline Nav** on Android.
2. Grant requested permissions:
   - **Location Permission**: `ACCESS_FINE_LOCATION` (GPS Doppler hardware telemetry)
   - **Notification Access**: Scrapes Google Maps navigation maneuvers
   - **Battery Optimization**: Set to *Unrestricted*
3. Turn on **Mobile Hotspot** with matching SSID/password from `secrets.h`.
4. Tap **Start service**. The embedded HTTP server serves `:8080/nav` and `:8080/map`.
5. Start navigation in Google Maps.

---

## 7. Network Wire Protocols

### `GET /nav` (JSON Telemetry)
```json
{
  "nav": true,
  "maneuver": "turn-right",
  "dist_text": "250 m",
  "street": "MG Road",
  "eta": "12:34",
  "remain": "4.2 km",
  "seq": 187,
  "speed_kmh": 68.5,
  "bearing": 182.4,
  "trip_dist_km": 14.2,
  "elapsed_s": 1540,
  "map_available": true
}
```

### `GET /map` (Binary JPEG with Telemetry Headers)
- **Status**: `200 OK` (or `503 Service Unavailable` if tiles offline)
- **Content-Type**: `image/jpeg`
- **Connection**: `close`
- **Headers**:
  - `X-Nav-Dist`: `250 m`
  - `X-Nav-Maneuver`: `turn-right`
  - `X-Nav-Speed`: `68.5`
  - `X-Nav-Street`: `MG Road`
  - `X-Nav-Eta`: `12:34`
- **Body**: $240 \times 240$ JPEG (Quality 75) heading-up rotated raster tile

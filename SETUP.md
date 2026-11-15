# Beeline — portable turn-by-turn display

Waveshare ESP32-S3-Touch-LCD-1.28 shows the next turn. Your Android phone
runs Google Maps, scrapes the navigation notification, and serves JSON on
port 8080. The ESP32 joins the phone hotspot and polls that endpoint.

## 1. Arduino IDE

Board: **ESP32S3 Dev Module**

- USB CDC On Boot: Enabled
- PSRAM: **OPI PSRAM**
- Flash Size: 8MB (or match the module)
- TFT_eSPI: `User_Setup_Select.h` includes `Setup302_Waveshare_ESP32S3_GC9A01.h`

Copy `secrets.h.example` to `secrets.h` if you do not already have one, then
put the phone hotspot SSID and password in `secrets.h`.

## 2. Prove the display (laptop stub, optional)

1. Connect the laptop to the same hotspot the ESP32 will join, or put the
   laptop on a network the board can reach.
2. Run:

   ```
   python3 tools/nav_stub.py --demo
   ```

3. In `secrets.h` set `NAV_SERVER_HOST` to the laptop IP (leave empty later
   so the board uses the phone gateway).
4. Flash and confirm the ribbon, distance, and maneuvers cycle.

Leave `NAV_SERVER_HOST` empty for the real phone setup.

## 3. Android app

### Option A — no Android Studio (recommended if you only need an APK)

See [android/BUILD_CLI.md](android/BUILD_CLI.md): install JDK 17 + Android command-line tools, then `./gradlew assembleDebug`.

### Option B — Android Studio

1. Open the `android/` folder in Android Studio and let Gradle sync.
2. Plug in the phone, enable USB debugging, Run the app.
3. In the app:
   - **Grant notification access** — enable Beeline Nav
   - **Disable battery optimization** — allow unrestricted
   - **Start service**
4. Phone locale should be **English** (the parser matches English Maps text).
5. Turn on **Mobile Hotspot**. SSID/password must match `secrets.h`.
6. Start navigation in Google Maps. The app live view should show JSON like:

   ```json
   {"nav":true,"maneuver":"turn-left","dist_text":"240 m","street":"MG Road","eta":"12:34","remain":"4.2 km","seq":12}
   ```

7. Power the ESP32. It joins the hotspot, polls `http://<phone-gateway>:8080/nav`,
   and draws the turn.

Typical hotspot gateway IPs: `192.168.43.1` or `192.168.137.1`. The board
discovers this with `WiFi.gatewayIP()` — you do not set it.

## 4. Road test

- Keep Maps in the foreground or as an ongoing notification.
- If the screen says **Waiting for phone**, the HTTP server is not reachable
  (service stopped, or hotspot not actually on).
- **No signal** means a poll succeeded earlier then timed out for 3 seconds.
- **Connecting...** means WiFi is not associated with the hotspot.
- Some Androids idle-timeout the hotspot with no clients; the board reconnects
  once the hotspot is back.

## Wire format

`GET /nav`

```json
{"nav":true,"maneuver":"turn-right","dist_text":"250 m","street":"MG Road","eta":"12:34","remain":"4.2 km","seq":187}
```

Maneuvers: `straight`, `slight-left`, `slight-right`, `turn-left`, `turn-right`,
`sharp-left`, `sharp-right`, `uturn`, `roundabout`, `arrive`.

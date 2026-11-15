/*
  Bline — Waveshare ESP32-S3-Touch-LCD-1.28 (GC9A01 240x240)

  Turn-by-turn renderer. The phone owns routing; this board polls
  GET http://<gateway>:8080/nav and draws a Bline-style ribbon.

  Arduino IDE (required for this board):
    Board: ESP32S3 Dev Module
    USB CDC On Boot: Enabled
    PSRAM: OPI PSRAM
    Flash Size: 8MB (or match your module)

  TFT_eSPI: User_Setup_Select.h -> Setup302_Waveshare_ESP32S3_GC9A01.h

  Fill in secrets.h with the phone hotspot SSID/password before flashing.
*/

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <ctype.h>
#include <strings.h>
#include "secrets.h"

extern const lv_img_dsc_t arrow_img;

// ---------- Display / LVGL ----------
TFT_eSPI tft = TFT_eSPI();
static const uint16_t SCREEN_W = 240;
static const uint16_t SCREEN_H = 240;

static const size_t DRAW_BUF_PIXELS = SCREEN_W * 40;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *draw_buf1 = nullptr;

static lv_obj_t *roadOutline = nullptr;
static lv_obj_t *roadLine = nullptr;
static lv_obj_t *posMarker = nullptr;
static lv_obj_t *turnGlyph = nullptr;
static lv_obj_t *streetLabel = nullptr;
static lv_obj_t *distNumLabel = nullptr;
static lv_obj_t *distUnitLabel = nullptr;
static lv_obj_t *statusLabel = nullptr;
static lv_obj_t *progressArc = nullptr;

// ---------- Nav snapshot (filled by HTTP poll) ----------
enum ConnState {
  CONN_CONNECTING,
  CONN_WAITING_PHONE,
  CONN_NOT_NAVIGATING,
  CONN_NO_SIGNAL,
  CONN_NAVIGATING
};

static ConnState connState = CONN_CONNECTING;
static bool navActive = false;
static char navManeuver[24] = "straight";
static char navDistText[24] = "";
static char navStreet[64] = "";
static char navEta[16] = "";
static char navRemain[24] = "";
static uint32_t navSeq = 0;
static uint32_t lastPollOkMs = 0;
static const uint32_t SIGNAL_TIMEOUT_MS = 3000;
static const uint32_t POLL_INTERVAL_MS = 500;
static const int PROGRESS_FULL_M = 500;

// ---------- Debug ----------
RTC_DATA_ATTR static uint32_t bootCount = 0;

static void bootLog(const char *msg) {
  Serial.println(msg);
  Serial.flush();
}

static void logHeap(const char *tag) {
  Serial.printf("%s: heap=%u psram=%u\n", tag,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getFreePsram());
  Serial.flush();
}

// ================= Ribbon geometries (static — lv_line stores the pointer) =================
// Screen origin is top-left. Stem runs up the centre; the last points form the bend.
static const lv_point_t RIBBON_STRAIGHT[] = {{120, 175}, {120, 42}};
static const lv_point_t RIBBON_SLIGHT_R[] = {{120, 175}, {120, 90}, {168, 42}};
static const lv_point_t RIBBON_SLIGHT_L[] = {{120, 175}, {120, 90}, {72, 42}};
static const lv_point_t RIBBON_TURN_R[]   = {{120, 175}, {120, 78}, {188, 78}};
static const lv_point_t RIBBON_TURN_L[]   = {{120, 175}, {120, 78}, {52, 78}};
static const lv_point_t RIBBON_SHARP_R[]  = {{120, 175}, {120, 88}, {178, 88}, {178, 150}};
static const lv_point_t RIBBON_SHARP_L[]  = {{120, 175}, {120, 88}, {62, 88}, {62, 150}};
static const lv_point_t RIBBON_UTURN[]    = {{120, 175}, {120, 52}, {86, 52}, {86, 110}};

// Out-params instead of a custom return type: Arduino auto-prototypes
// functions before this struct would be visible, which breaks compile.
static void geomFor(const char *m, const lv_point_t **pts, uint16_t *n, int16_t *glyphAngle) {
  if (!m) m = "straight";
  if (!strcmp(m, "slight-right")) { *pts = RIBBON_SLIGHT_R; *n = 3; *glyphAngle = 450; return; }
  if (!strcmp(m, "slight-left"))  { *pts = RIBBON_SLIGHT_L; *n = 3; *glyphAngle = -450; return; }
  if (!strcmp(m, "turn-right"))   { *pts = RIBBON_TURN_R;   *n = 3; *glyphAngle = 900; return; }
  if (!strcmp(m, "turn-left"))    { *pts = RIBBON_TURN_L;   *n = 3; *glyphAngle = -900; return; }
  if (!strcmp(m, "sharp-right"))  { *pts = RIBBON_SHARP_R;  *n = 4; *glyphAngle = 1350; return; }
  if (!strcmp(m, "sharp-left"))   { *pts = RIBBON_SHARP_L;  *n = 4; *glyphAngle = -1350; return; }
  if (!strcmp(m, "uturn"))        { *pts = RIBBON_UTURN;    *n = 4; *glyphAngle = 1800; return; }
  if (!strcmp(m, "roundabout"))   { *pts = RIBBON_TURN_R;   *n = 3; *glyphAngle = 900; return; }
  *pts = RIBBON_STRAIGHT;
  *n = 2;
  *glyphAngle = 0;
}

// ================= LVGL flush =================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

static void setLabelIfChanged(lv_obj_t *label, const char *text, char *cache, size_t cacheLen) {
  if (!label || !text) return;
  if (strncmp(cache, text, cacheLen) == 0) return;
  lv_label_set_text(label, text);
  strncpy(cache, text, cacheLen - 1);
  cache[cacheLen - 1] = '\0';
}

static void setHidden(lv_obj_t *obj, bool hidden) {
  if (!obj) return;
  if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static int parseMeters(const char *distText) {
  if (!distText || !distText[0]) return -1;
  char *end = nullptr;
  float v = strtof(distText, &end);
  if (end == distText) return -1;
  while (*end == ' ') end++;
  if (!strncasecmp(end, "km", 2) || !strncasecmp(end, "kilom", 5)) return (int)(v * 1000.0f);
  if (!strncasecmp(end, "mi", 2) || !strncasecmp(end, "mile", 4)) return (int)(v * 1609.34f);
  if (!strncasecmp(end, "ft", 2) || !strncasecmp(end, "feet", 4)) return (int)(v * 0.3048f);
  return (int)v;
}

static void splitDistText(const char *text, char *num, size_t nlen, char *unit, size_t ulen) {
  num[0] = 0;
  unit[0] = 0;
  if (!text) return;
  while (*text == ' ') text++;
  size_t i = 0;
  while (text[i] && (isdigit((unsigned char)text[i]) || text[i] == '.' || text[i] == ',') && i + 1 < nlen) {
    num[i] = text[i] == ',' ? '.' : text[i];
    i++;
  }
  num[i] = 0;
  const char *u = text + i;
  while (*u == ' ') u++;
  strncpy(unit, u, ulen - 1);
  unit[ulen - 1] = 0;
}

static void placeMarkerOnStem(const lv_point_t *pts, float t) {
  if (!posMarker || !pts) return;
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  lv_coord_t x = (lv_coord_t)(pts[0].x + (pts[1].x - pts[0].x) * t);
  lv_coord_t y = (lv_coord_t)(pts[0].y + (pts[1].y - pts[0].y) * t);
  lv_obj_set_pos(posMarker, x - 32, y - 32);
}

static void applyRibbon(const char *maneuver, int meters) {
  const lv_point_t *pts = nullptr;
  uint16_t n = 0;
  int16_t glyphAngle = 0;
  geomFor(maneuver, &pts, &n, &glyphAngle);
  lv_line_set_points(roadOutline, pts, n);
  lv_line_set_points(roadLine, pts, n);

  static int16_t lastGlyph = 9999;
  if (glyphAngle != lastGlyph) {
    lv_img_set_angle(turnGlyph, glyphAngle);
    lastGlyph = glyphAngle;
  }

  float t = 0.15f;
  if (meters >= 0) {
    t = 1.0f - ((float)meters / (float)PROGRESS_FULL_M);
    if (t < 0.08f) t = 0.08f;
    if (t > 0.92f) t = 0.92f;
  }
  placeMarkerOnStem(pts, t);

  int pct = 0;
  if (meters < 0) pct = 100;
  else if (meters >= PROGRESS_FULL_M) pct = 100;
  else pct = (meters * 100) / PROGRESS_FULL_M;
  lv_arc_set_value(progressArc, pct);
}

void buildUI() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  progressArc = lv_arc_create(scr);
  lv_obj_set_size(progressArc, 228, 228);
  lv_obj_center(progressArc);
  lv_arc_set_rotation(progressArc, 270);
  lv_arc_set_bg_angles(progressArc, 0, 360);
  lv_arc_set_range(progressArc, 0, 100);
  lv_arc_set_value(progressArc, 0);
  lv_obj_remove_style(progressArc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(progressArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(progressArc, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(progressArc, lv_color_white(), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(progressArc, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(progressArc, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_color(progressArc, lv_color_make(40, 40, 40), LV_PART_MAIN);

  // Dark outline under the white ribbon so the white arrow stays readable
  roadOutline = lv_line_create(scr);
  lv_obj_set_style_line_width(roadOutline, 22, LV_PART_MAIN);
  lv_obj_set_style_line_color(roadOutline, lv_color_make(28, 28, 28), LV_PART_MAIN);
  lv_obj_set_style_line_rounded(roadOutline, true, LV_PART_MAIN);
  lv_line_set_points(roadOutline, RIBBON_STRAIGHT, 2);

  roadLine = lv_line_create(scr);
  lv_obj_set_style_line_width(roadLine, 14, LV_PART_MAIN);
  lv_obj_set_style_line_color(roadLine, lv_color_make(210, 210, 210), LV_PART_MAIN);
  lv_obj_set_style_line_rounded(roadLine, true, LV_PART_MAIN);
  lv_line_set_points(roadLine, RIBBON_STRAIGHT, 2);

  posMarker = lv_img_create(scr);
  lv_img_set_src(posMarker, &arrow_img);
  lv_img_set_pivot(posMarker, 32, 32);
  lv_img_set_zoom(posMarker, 96);
  lv_img_set_antialias(posMarker, true);
  lv_img_set_angle(posMarker, 0);
  // Pure-white asset disappears on a light road — recolor the rider marker black
  lv_obj_set_style_img_recolor(posMarker, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_img_recolor_opa(posMarker, LV_OPA_COVER, LV_PART_MAIN);
  placeMarkerOnStem(RIBBON_STRAIGHT, 0.2f);

  streetLabel = lv_label_create(scr);
  lv_obj_set_width(streetLabel, 180);
  lv_obj_set_style_text_color(streetLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(streetLabel, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_align(streetLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(streetLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_label_set_text(streetLabel, "");
  lv_obj_align(streetLabel, LV_ALIGN_TOP_MID, 0, 18);

  statusLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(statusLabel, lv_color_make(160, 160, 160), LV_PART_MAIN);
  lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(statusLabel, "Connecting...");
  lv_obj_align(statusLabel, LV_ALIGN_TOP_MID, 0, 18);

  turnGlyph = lv_img_create(scr);
  lv_img_set_src(turnGlyph, &arrow_img);
  lv_img_set_pivot(turnGlyph, 32, 32);
  lv_img_set_zoom(turnGlyph, 140);
  lv_img_set_antialias(turnGlyph, true);
  lv_obj_align(turnGlyph, LV_ALIGN_BOTTOM_LEFT, 18, -28);

  distNumLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(distNumLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(distNumLabel, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_label_set_text(distNumLabel, "--");
  lv_obj_align(distNumLabel, LV_ALIGN_BOTTOM_RIGHT, -28, -36);

  distUnitLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(distUnitLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(distUnitLabel, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(distUnitLabel, "");
  lv_obj_align_to(distUnitLabel, distNumLabel, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -6);

  setHidden(roadOutline, true);
  setHidden(roadLine, true);
  setHidden(posMarker, true);
  setHidden(turnGlyph, true);
  setHidden(progressArc, true);
  setHidden(streetLabel, true);
  setHidden(distNumLabel, true);
  setHidden(distUnitLabel, true);
}

static void showIdle(const char *status) {
  setHidden(roadOutline, true);
  setHidden(roadLine, true);
  setHidden(posMarker, true);
  setHidden(turnGlyph, true);
  setHidden(progressArc, true);
  setHidden(streetLabel, true);
  setHidden(distNumLabel, true);
  setHidden(distUnitLabel, true);
  setHidden(statusLabel, false);
  static char statusCache[40] = "";
  setLabelIfChanged(statusLabel, status, statusCache, sizeof(statusCache));
}

static void showNav() {
  const bool arrive = !strcmp(navManeuver, "arrive");
  setHidden(statusLabel, true);
  setHidden(roadOutline, arrive);
  setHidden(roadLine, arrive);
  setHidden(posMarker, arrive);
  setHidden(turnGlyph, arrive);
  setHidden(progressArc, arrive);
  setHidden(streetLabel, false);
  setHidden(distNumLabel, false);
  setHidden(distUnitLabel, !arrive);

  static char streetCache[64] = "";
  static char numCache[16] = "";
  static char unitCache[12] = "";
  static char lastMan[24] = "";
  static int lastMeters = -99999;

  const char *street = navStreet[0] ? navStreet : " ";
  setLabelIfChanged(streetLabel, street, streetCache, sizeof(streetCache));

  if (arrive) {
    setLabelIfChanged(distNumLabel, "Arrived", numCache, sizeof(numCache));
    setLabelIfChanged(distUnitLabel, "", unitCache, sizeof(unitCache));
    return;
  }

  char num[16], unit[12];
  splitDistText(navDistText, num, sizeof(num), unit, sizeof(unit));
  if (!num[0]) strncpy(num, "--", sizeof(num) - 1);
  setLabelIfChanged(distNumLabel, num, numCache, sizeof(numCache));
  setLabelIfChanged(distUnitLabel, unit, unitCache, sizeof(unitCache));
  lv_obj_align(distNumLabel, LV_ALIGN_BOTTOM_RIGHT, -28, -36);
  lv_obj_align_to(distUnitLabel, distNumLabel, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -6);

  int meters = parseMeters(navDistText);
  if (strcmp(lastMan, navManeuver) != 0 || meters != lastMeters) {
    applyRibbon(navManeuver, meters);
    strncpy(lastMan, navManeuver, sizeof(lastMan) - 1);
    lastMan[sizeof(lastMan) - 1] = 0;
    lastMeters = meters;
  }
}

void updateNavUI() {
  if (!statusLabel) return;

  switch (connState) {
    case CONN_CONNECTING:     showIdle("Connecting..."); break;
    case CONN_WAITING_PHONE:  showIdle("Waiting for phone"); break;
    case CONN_NOT_NAVIGATING: showIdle("Not navigating"); break;
    case CONN_NO_SIGNAL:      showIdle("No signal"); break;
    case CONN_NAVIGATING:     showNav(); break;
  }
}

// ================= HTTP poll =================
static String navUrl() {
  if (strlen(NAV_SERVER_HOST) > 0) {
    return String("http://") + NAV_SERVER_HOST + ":" + String(NAV_SERVER_PORT) + "/nav";
  }
  IPAddress gw = WiFi.gatewayIP();
  return String("http://") + gw.toString() + ":" + String(NAV_SERVER_PORT) + "/nav";
}

static void applyNavJson(const char *payload) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return;

  navActive = doc["nav"] | false;
  navSeq = doc["seq"] | navSeq;

  const char *m = doc["maneuver"] | "straight";
  strncpy(navManeuver, m, sizeof(navManeuver) - 1);
  navManeuver[sizeof(navManeuver) - 1] = 0;

  const char *d = doc["dist_text"] | "";
  strncpy(navDistText, d, sizeof(navDistText) - 1);
  navDistText[sizeof(navDistText) - 1] = 0;

  const char *s = doc["street"] | "";
  strncpy(navStreet, s, sizeof(navStreet) - 1);
  navStreet[sizeof(navStreet) - 1] = 0;

  const char *e = doc["eta"] | "";
  strncpy(navEta, e, sizeof(navEta) - 1);
  navEta[sizeof(navEta) - 1] = 0;

  const char *r = doc["remain"] | "";
  strncpy(navRemain, r, sizeof(navRemain) - 1);
  navRemain[sizeof(navRemain) - 1] = 0;

  lastPollOkMs = millis();
  connState = navActive ? CONN_NAVIGATING : CONN_NOT_NAVIGATING;
  Serial.printf("nav seq=%lu active=%d %s %s '%s' remain=%s eta=%s\n",
                (unsigned long)navSeq, navActive ? 1 : 0, navManeuver, navDistText,
                navStreet, navRemain, navEta);
}

static void pollNav() {
  if (WiFi.status() != WL_CONNECTED) {
    connState = CONN_CONNECTING;
    return;
  }

  HTTPClient http;
  WiFiClient client;
  http.setTimeout(400);
  if (!http.begin(client, navUrl())) {
    if (millis() - lastPollOkMs > SIGNAL_TIMEOUT_MS) {
      connState = (lastPollOkMs == 0) ? CONN_WAITING_PHONE : CONN_NO_SIGNAL;
    }
    return;
  }

  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    applyNavJson(body.c_str());
  } else {
    if (lastPollOkMs == 0) connState = CONN_WAITING_PHONE;
    else if (millis() - lastPollOkMs > SIGNAL_TIMEOUT_MS) connState = CONN_NO_SIGNAL;
  }
  http.end();
}

static void maintainWifi() {
  static uint32_t lastTry = 0;
  if (WiFi.status() == WL_CONNECTED) return;
  connState = CONN_CONNECTING;
  uint32_t now = millis();
  if (now - lastTry < 4000) return;
  lastTry = now;
  bootLog("WiFi reconnect");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ================= Init =================
static bool initDrawBuffer() {
  size_t bytes = DRAW_BUF_PIXELS * sizeof(lv_color_t);

  draw_buf1 = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!draw_buf1) {
    bootLog("WARN: PSRAM alloc failed, using internal RAM for LVGL buffer");
    draw_buf1 = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return draw_buf1 != nullptr;
}

static bool initDisplayAndLvgl() {
#ifdef TFT_BL
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
#endif

  bootLog("step: tft.begin()");
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  bootLog("step: tft OK");

  if (!initDrawBuffer()) {
    bootLog("FATAL: LVGL draw buffer alloc failed");
    return false;
  }

  bootLog("step: lv_init()");
  lv_init();
  lv_disp_draw_buf_init(&draw_buf, draw_buf1, NULL, DRAW_BUF_PIXELS);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_W;
  disp_drv.ver_res = SCREEN_H;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  bootLog("step: buildUI()");
  buildUI();
  return true;
}

static void initWiFi() {
  bootLog("step: WiFi.begin()");
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void setup() {
  Serial.begin(115200);
  for (uint32_t t0 = millis(); !Serial && (millis() - t0) < 3000;) {
    delay(10);
  }

  bootCount++;
  Serial.printf("Bline boot #%lu\n", (unsigned long)bootCount);
  logHeap("boot");

#ifndef BOARD_HAS_PSRAM
  bootLog("WARN: PSRAM not enabled — set Tools -> PSRAM -> OPI PSRAM");
#endif

  initWiFi();

  if (!initDisplayAndLvgl()) {
    bootLog("HALTED: display init failed");
    return;
  }

  logHeap("after LVGL");
  bootLog("display OK");
}

void loop() {
  static uint32_t last_tick_ms = 0;
  static uint32_t boot_ms = 0;
  uint32_t now = millis();
  if (boot_ms == 0) boot_ms = now;

  if (last_tick_ms == 0) last_tick_ms = now;
  uint32_t dt = now - last_tick_ms;
  if (dt > 0) {
    lv_tick_inc(dt);
    last_tick_ms = now;
  }

  maintainWifi();

  static uint32_t last_poll_ms = 0;
  if ((now - boot_ms) > 1500 && (now - last_poll_ms) >= POLL_INTERVAL_MS) {
    last_poll_ms = now;
    pollNav();
  }

  lv_timer_handler();

  static uint32_t last_ui_ms = 0;
  uint32_t uiInterval = (connState == CONN_NAVIGATING) ? 100 : 400;
  if (now - last_ui_ms >= uiInterval) {
    last_ui_ms = now;
    updateNavUI();
  }

  esp_task_wdt_reset();
  delay(5);
}

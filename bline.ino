/*
  Bline — Waveshare ESP32-S3-Touch-LCD-1.28 (GC9A01 240x240 circular LCD + CST816S Touch)
  Tri-Screen Motorcycle Navigation Dashboard:
    Screen 1: Royal Enfield style rotated raster mini-map HUD (MapTiler Dark)
    Screen 2: Beeline Moto II style high-contrast monochrome vector road ribbon
    Screen 3: Classic Retro Chrono Gauge & Cockpit Telemetry

  Dual-Core FreeRTOS Architecture:
    Core 0 (TaskNetwork): Wi-Fi, HTTP /map & /nav polling, TJpgDec PSRAM decoding, cache sync.
    Core 1 (TaskUI): LVGL rendering, CST816S touch polling, SPI DMA flushes. ZERO network calls.
*/

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include <ctype.h>
#include <strings.h>
#include <math.h>
#include "secrets.h"
#include "cst816s.h"

#if __has_include(<esp_cache.h>)
#include <esp_cache.h>
#define esp_cache_msync_compat(addr, size) esp_cache_msync(addr, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M)
#else
extern "C" int Cache_WriteBack_Addr(uint32_t addr, uint32_t size);
#define esp_cache_msync_compat(addr, size) Cache_WriteBack_Addr((uint32_t)(addr), (uint32_t)(size))
#endif

extern const lv_img_dsc_t arrow_img;

// ================= Display & Hardware Constants =================
static const uint16_t SCREEN_W = 240;
static const uint16_t SCREEN_H = 240;
static const size_t DRAW_BUF_PIXELS = SCREEN_W * 40; // 19.2 KB for DMA line buffers
static const int PROGRESS_FULL_M = 500;

TFT_eSPI tft = TFT_eSPI();
CST816S touch(CST816S_PIN_SDA, CST816S_PIN_SCL, CST816S_PIN_RST, CST816S_PIN_INT);

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *draw_buf1 = nullptr;
static lv_color_t *draw_buf2 = nullptr;

// ================= Inter-Core IPC Structures =================
struct FrameDesc {
  uint16_t *buf;
  char distText[24];
  char maneuver[24];
  float speedKmh;
  char street[64];
  char eta[16];
  uint32_t timestampMs;
  bool valid;
};

struct NavSnapshot {
  bool nav;
  char maneuver[24];
  char distText[24];
  char street[64];
  char eta[16];
  char remain[24];
  uint32_t seq;
  float speedKmh;
  float bearing;
  float tripDistKm;
  uint32_t elapsedS;
  bool mapAvailable;
  uint32_t timestampMs;
};

static QueueHandle_t frameQueue = NULL;
static QueueHandle_t navQueue = NULL;
static TaskHandle_t taskNetworkHandle = NULL;

// PSRAM Buffers for Map Frames
static uint16_t *mapBufPing = nullptr;
static uint16_t *mapBufPong = nullptr;
static uint8_t *jpegDownloadBuf = nullptr;
static uint16_t *decodeTargetBuf = nullptr;

// Display Image Descriptor for LVGL Screen 1
static lv_img_dsc_t mapImgDsc = {
  .header = {
    .cf = LV_IMG_CF_TRUE_COLOR,
    .always_zero = 0,
    .reserved = 0,
    .w = SCREEN_W,
    .h = SCREEN_H,
  },
  .data_size = (uint32_t)(SCREEN_W * SCREEN_H * sizeof(lv_color_t)),
  .data = NULL
};

// ================= State Machine & Shared Concurrency =================
enum ConnState {
  CONN_CONNECTING,
  CONN_WAITING_PHONE,
  CONN_NOT_NAVIGATING,
  CONN_NO_SIGNAL,
  CONN_NAVIGATING
};

static ConnState connState = CONN_CONNECTING;
static volatile uint8_t activeScreenIndex = 0; // 0=Map, 1=Ribbon, 2=Chrono
static volatile bool fallbackMode = false;
static volatile bool autoFailoverActive = false;
static volatile bool suppressFailover = false;
static uint8_t currentScreen = 0;

static bool navActive = false;
static char navManeuver[24] = "straight";
static char navDistText[24] = "";
static char navStreet[64] = "";
static char navEta[16] = "";
static char navRemain[24] = "";
static uint32_t navSeq = 0;
static float navSpeedKmh = 0.0f;
static float navBearing = 0.0f;
static float navTripDistKm = 0.0f;
static uint32_t navElapsedS = 0;
static bool navMapAvailable = false;
static uint32_t lastNavUpdateMs = 0;
static uint32_t lastMapFrameMs = 0;

// ================= LVGL Screen Objects =================
static lv_obj_t *screens[3] = {nullptr, nullptr, nullptr};

// Screen 1: Mini-Map HUD
static lv_obj_t *screenMap = nullptr;
static lv_obj_t *mapImgObj = nullptr;
static lv_obj_t *posMarkerMap = nullptr;
static lv_obj_t *scrimTopMap = nullptr;
static lv_obj_t *streetLabelMap = nullptr;
static lv_obj_t *scrimBotMap = nullptr;
static lv_obj_t *glyphMap = nullptr;
static lv_obj_t *distLabelMap = nullptr;
static lv_obj_t *speedPillMap = nullptr;
static lv_obj_t *speedLabelMap = nullptr;
static lv_obj_t *offlinePillMap = nullptr;
static lv_obj_t *offlineLabelMap = nullptr;

// Screen 2: Vector Ribbon
static lv_obj_t *screenRibbon = nullptr;
static lv_obj_t *roadOutline = nullptr;
static lv_obj_t *roadLine = nullptr;
static lv_obj_t *posMarker = nullptr;
static lv_obj_t *turnGlyph = nullptr;
static lv_obj_t *streetLabel = nullptr;
static lv_obj_t *distNumLabel = nullptr;
static lv_obj_t *distUnitLabel = nullptr;
static lv_obj_t *statusLabel = nullptr;
static lv_obj_t *progressArc = nullptr;

// Screen 3: Retro Chrono Gauge
static lv_obj_t *screenChrono = nullptr;
static lv_obj_t *chronoMeter = nullptr;
static lv_obj_t *chronoKmH = nullptr;
static lv_obj_t *chronoNeedle = nullptr;
static lv_obj_t *chronoHub = nullptr;
static lv_obj_t *chronoRivet = nullptr;
static lv_obj_t *chronoTurnBanner = nullptr;
static lv_obj_t *chronoTurnGlyph = nullptr;
static lv_obj_t *chronoTurnDist = nullptr;
static lv_point_t chronoNeedlePts[2] = {{120, 120}, {120, 120}};

// Forward declarations
static void switchScreen(uint8_t targetIdx, lv_scr_load_anim_t anim);
static void updateActiveScreenUI();
static void applyNavSnapshot(const NavSnapshot *s);
static void applyFrameToMapScreen(const FrameDesc *f);
static void TaskNetworkCode(void *pvParameters);

// ================= Ribbon Geometries =================
static const lv_point_t RIBBON_STRAIGHT[] = {{120, 175}, {120, 42}};
static const lv_point_t RIBBON_SLIGHT_R[] = {{120, 175}, {120, 90}, {168, 42}};
static const lv_point_t RIBBON_SLIGHT_L[] = {{120, 175}, {120, 90}, {72, 42}};
static const lv_point_t RIBBON_TURN_R[]   = {{120, 175}, {120, 78}, {188, 78}};
static const lv_point_t RIBBON_TURN_L[]   = {{120, 175}, {120, 78}, {52, 78}};
static const lv_point_t RIBBON_SHARP_R[]  = {{120, 175}, {120, 88}, {178, 88}, {178, 150}};
static const lv_point_t RIBBON_SHARP_L[]  = {{120, 175}, {120, 88}, {62, 88}, {62, 150}};
static const lv_point_t RIBBON_UTURN[]    = {{120, 175}, {120, 52}, {86, 52}, {86, 110}};

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

// ================= Utility Helpers =================
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
  char buf[32];
  size_t i = 0;
  for (; distText[i] != '\0' && i < sizeof(buf) - 1; i++) {
    buf[i] = (distText[i] == ',') ? '.' : distText[i];
  }
  buf[i] = '\0';

  char *end = nullptr;
  float v = strtof(buf, &end);
  if (end == buf) return -1;
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

static const char *getCardinal(float bearing) {
  static const char *cardinals[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  while (bearing < 0.0f) bearing += 360.0f;
  while (bearing >= 360.0f) bearing -= 360.0f;
  int idx = (int)((bearing + 22.5f) / 45.0f) % 8;
  return cardinals[idx];
}

static void placeMarkerOnStem(const lv_point_t *pts, float t) {
  if (!posMarker || !pts) return;
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  lv_coord_t x = (lv_coord_t)(pts[0].x + (pts[1].x - pts[0].x) * t);
  lv_coord_t y = (lv_coord_t)(pts[0].y + (pts[1].y - pts[0].y) * t);
  lv_obj_set_pos(posMarker, x - 32, y - 32);
}

static void updateChronoNeedle(float speedKmh) {
  if (!chronoNeedle) return;
  if (speedKmh < 0.0f) speedKmh = 0.0f;
  if (speedKmh > 140.0f) speedKmh = 140.0f;

  // Vintage dial: 0 to 140 km/h over 270 deg sweep (starts at 135 deg / 7:30 bottom-left)
  float angleDeg = 135.0f + (speedKmh / 140.0f) * 270.0f;
  float rad = angleDeg * (3.14159265f / 180.0f);
  int16_t rFront = 84;
  int16_t rTail = 18;

  chronoNeedlePts[0].x = 120 - (int16_t)(rTail * cosf(rad));
  chronoNeedlePts[0].y = 120 - (int16_t)(rTail * sinf(rad));
  chronoNeedlePts[1].x = 120 + (int16_t)(rFront * cosf(rad));
  chronoNeedlePts[1].y = 120 + (int16_t)(rFront * sinf(rad));
  lv_line_set_points(chronoNeedle, chronoNeedlePts, 2);
}

// ================= LVGL Display & Input Drivers =================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  uint8_t gesture = GESTURE_NONE;
  uint16_t tx = 0, ty = 0;
  bool isPressed = false;
  static uint32_t lastSwipeMs = 0;
  static bool gestureHandled = false;

  uint32_t now = millis();

  if (touch.readTouch(gesture, tx, ty, isPressed)) {
    if (!isPressed && gesture == GESTURE_NONE) {
      gestureHandled = false;
    } else if (now - lastSwipeMs > 600) {
      gestureHandled = false;
    }

    if (!gestureHandled && (now - lastSwipeMs >= 350)) {
      if (gesture == GESTURE_SWIPE_LEFT) {
        if (currentScreen < 2) {
          gestureHandled = true;
          lastSwipeMs = now;
          autoFailoverActive = false;
          switchScreen(currentScreen + 1, LV_SCR_LOAD_ANIM_MOVE_LEFT);
        }
      } else if (gesture == GESTURE_SWIPE_RIGHT) {
        if (currentScreen > 0) {
          gestureHandled = true;
          lastSwipeMs = now;
          autoFailoverActive = false;
          if (currentScreen - 1 == 0 && fallbackMode) {
            suppressFailover = true;
          }
          switchScreen(currentScreen - 1, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
        }
      }
    }

    if (isPressed) {
      data->state = LV_INDEV_STATE_PR;
      data->point.x = tx;
      data->point.y = ty;
    } else {
      data->state = LV_INDEV_STATE_REL;
    }
  } else {
    data->state = LV_INDEV_STATE_REL;
    gestureHandled = false;
  }
}

// ================= Screen Builder Functions =================
static void buildScreenMap() {
  screenMap = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screenMap, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(screenMap, LV_OPA_COVER, 0);

  mapImgObj = lv_img_create(screenMap);
  lv_obj_set_pos(mapImgObj, 0, 0);
  lv_obj_set_size(mapImgObj, SCREEN_W, SCREEN_H);

  posMarkerMap = lv_img_create(screenMap);
  lv_img_set_src(posMarkerMap, &arrow_img);
  lv_img_set_pivot(posMarkerMap, 32, 32);
  lv_img_set_zoom(posMarkerMap, 96);
  lv_img_set_angle(posMarkerMap, 0);
  lv_obj_set_style_img_recolor(posMarkerMap, lv_color_make(0, 220, 255), LV_PART_MAIN);
  lv_obj_set_style_img_recolor_opa(posMarkerMap, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_pos(posMarkerMap, 120 - 32, 160 - 32);

  scrimTopMap = lv_obj_create(screenMap);
  lv_obj_set_size(scrimTopMap, 180, 26);
  lv_obj_align(scrimTopMap, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_set_style_bg_color(scrimTopMap, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scrimTopMap, LV_OPA_80, 0);
  lv_obj_set_style_radius(scrimTopMap, 8, 0);
  lv_obj_set_style_border_width(scrimTopMap, 0, 0);
  lv_obj_clear_flag(scrimTopMap, LV_OBJ_FLAG_SCROLLABLE);

  streetLabelMap = lv_label_create(scrimTopMap);
  lv_label_set_long_mode(streetLabelMap, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(streetLabelMap, 160);
  lv_obj_center(streetLabelMap);
  lv_obj_set_style_text_color(streetLabelMap, lv_color_white(), 0);
  lv_obj_set_style_text_font(streetLabelMap, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(streetLabelMap, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(streetLabelMap, "Bline Map HUD");

  scrimBotMap = lv_obj_create(screenMap);
  lv_obj_set_size(scrimBotMap, 170, 36);
  lv_obj_align(scrimBotMap, LV_ALIGN_TOP_MID, 0, 168);
  lv_obj_set_style_bg_color(scrimBotMap, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scrimBotMap, LV_OPA_80, 0);
  lv_obj_set_style_radius(scrimBotMap, 8, 0);
  lv_obj_set_style_border_width(scrimBotMap, 0, 0);
  lv_obj_clear_flag(scrimBotMap, LV_OBJ_FLAG_SCROLLABLE);

  glyphMap = lv_img_create(scrimBotMap);
  lv_img_set_src(glyphMap, &arrow_img);
  lv_img_set_pivot(glyphMap, 32, 32);
  lv_img_set_zoom(glyphMap, 80);
  lv_img_set_antialias(glyphMap, true);
  lv_obj_set_style_img_recolor(glyphMap, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_img_recolor_opa(glyphMap, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(glyphMap, LV_ALIGN_LEFT_MID, 4, 0);

  distLabelMap = lv_label_create(scrimBotMap);
  lv_obj_set_style_text_color(distLabelMap, lv_color_white(), 0);
  lv_obj_set_style_text_font(distLabelMap, &lv_font_montserrat_28, 0);
  lv_label_set_text(distLabelMap, "-- m");
  lv_obj_align(distLabelMap, LV_ALIGN_RIGHT_MID, -8, 0);

  speedPillMap = lv_obj_create(screenMap);
  lv_obj_set_size(speedPillMap, 84, 20);
  lv_obj_align(speedPillMap, LV_ALIGN_TOP_MID, 0, 208);
  lv_obj_set_style_bg_color(speedPillMap, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(speedPillMap, LV_OPA_80, 0);
  lv_obj_set_style_radius(speedPillMap, 6, 0);
  lv_obj_set_style_border_width(speedPillMap, 0, 0);
  lv_obj_clear_flag(speedPillMap, LV_OBJ_FLAG_SCROLLABLE);

  speedLabelMap = lv_label_create(speedPillMap);
  lv_obj_set_style_text_color(speedLabelMap, lv_color_make(0, 220, 255), 0);
  lv_obj_set_style_text_font(speedLabelMap, &lv_font_montserrat_14, 0);
  lv_label_set_text(speedLabelMap, "0 km/h");
  lv_obj_center(speedLabelMap);

  offlinePillMap = lv_obj_create(screenMap);
  lv_obj_set_size(offlinePillMap, 140, 24);
  lv_obj_center(offlinePillMap);
  lv_obj_set_style_bg_color(offlinePillMap, lv_color_make(180, 30, 30), 0);
  lv_obj_set_style_bg_opa(offlinePillMap, LV_OPA_90, 0);
  lv_obj_set_style_radius(offlinePillMap, 6, 0);
  lv_obj_set_style_border_width(offlinePillMap, 0, 0);
  lv_obj_clear_flag(offlinePillMap, LV_OBJ_FLAG_SCROLLABLE);

  offlineLabelMap = lv_label_create(offlinePillMap);
  lv_obj_set_style_text_color(offlineLabelMap, lv_color_white(), 0);
  lv_obj_set_style_text_font(offlineLabelMap, &lv_font_montserrat_14, 0);
  lv_label_set_text(offlineLabelMap, "CONNECTING...");
  lv_obj_center(offlineLabelMap);
}

static void buildScreenRibbon() {
  screenRibbon = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screenRibbon, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(screenRibbon, LV_OPA_COVER, 0);

  progressArc = lv_arc_create(screenRibbon);
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

  roadOutline = lv_line_create(screenRibbon);
  lv_obj_set_style_line_width(roadOutline, 22, LV_PART_MAIN);
  lv_obj_set_style_line_color(roadOutline, lv_color_make(28, 28, 28), LV_PART_MAIN);
  lv_obj_set_style_line_rounded(roadOutline, true, LV_PART_MAIN);
  lv_line_set_points(roadOutline, RIBBON_STRAIGHT, 2);

  roadLine = lv_line_create(screenRibbon);
  lv_obj_set_style_line_width(roadLine, 14, LV_PART_MAIN);
  lv_obj_set_style_line_color(roadLine, lv_color_make(210, 210, 210), LV_PART_MAIN);
  lv_obj_set_style_line_rounded(roadLine, true, LV_PART_MAIN);
  lv_line_set_points(roadLine, RIBBON_STRAIGHT, 2);

  posMarker = lv_img_create(screenRibbon);
  lv_img_set_src(posMarker, &arrow_img);
  lv_img_set_pivot(posMarker, 32, 32);
  lv_img_set_zoom(posMarker, 96);
  lv_img_set_antialias(posMarker, true);
  lv_img_set_angle(posMarker, 0);
  lv_obj_set_style_img_recolor(posMarker, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_img_recolor_opa(posMarker, LV_OPA_COVER, LV_PART_MAIN);
  placeMarkerOnStem(RIBBON_STRAIGHT, 0.2f);

  streetLabel = lv_label_create(screenRibbon);
  lv_obj_set_width(streetLabel, 180);
  lv_obj_set_style_text_color(streetLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(streetLabel, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_align(streetLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(streetLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_label_set_text(streetLabel, "");
  lv_obj_align(streetLabel, LV_ALIGN_TOP_MID, 0, 18);

  statusLabel = lv_label_create(screenRibbon);
  lv_obj_set_style_text_color(statusLabel, lv_color_make(160, 160, 160), LV_PART_MAIN);
  lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(statusLabel, "Connecting...");
  lv_obj_align(statusLabel, LV_ALIGN_TOP_MID, 0, 18);

  turnGlyph = lv_img_create(screenRibbon);
  lv_img_set_src(turnGlyph, &arrow_img);
  lv_img_set_pivot(turnGlyph, 32, 32);
  lv_img_set_zoom(turnGlyph, 140);
  lv_img_set_antialias(turnGlyph, true);
  lv_obj_align(turnGlyph, LV_ALIGN_BOTTOM_LEFT, 18, -28);

  distNumLabel = lv_label_create(screenRibbon);
  lv_obj_set_style_text_color(distNumLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(distNumLabel, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_label_set_text(distNumLabel, "--");
  lv_obj_align(distNumLabel, LV_ALIGN_BOTTOM_RIGHT, -28, -36);

  distUnitLabel = lv_label_create(screenRibbon);
  lv_obj_set_style_text_color(distUnitLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(distUnitLabel, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(distUnitLabel, "");
  lv_obj_align_to(distUnitLabel, distNumLabel, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -6);
}

static void buildScreenChrono() {
  screenChrono = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screenChrono, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(screenChrono, LV_OPA_COVER, 0);

  // 1. Vintage White Dial Face (238x238 with black outer bezel)
  chronoMeter = lv_meter_create(screenChrono);
  lv_obj_set_size(chronoMeter, 238, 238);
  lv_obj_center(chronoMeter);
  lv_obj_set_style_bg_color(chronoMeter, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(chronoMeter, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(chronoMeter, lv_color_black(), 0);
  lv_obj_set_style_border_width(chronoMeter, 5, 0);
  lv_obj_set_style_radius(chronoMeter, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_all(chronoMeter, 6, 0);

  // 2. Analog Speed Scale: 0 to 140 km/h over 270 deg sweep (matching Image 2)
  lv_meter_scale_t *scale = lv_meter_add_scale(chronoMeter);
  lv_meter_set_scale_range(chronoMeter, scale, 0, 140, 270, 135);
  // 29 ticks total (5 km/h steps): 2px wide, 8px long, dark charcoal
  lv_meter_set_scale_ticks(chronoMeter, scale, 29, 2, 8, lv_color_make(60, 60, 60));
  // Every 4th tick is major (0, 20, 40, 60, 80, 100, 120, 140): 3px wide, 14px long, black
  lv_meter_set_scale_major_ticks(chronoMeter, scale, 4, 3, 14, lv_color_black(), 10);
  lv_obj_set_style_text_color(chronoMeter, lv_color_black(), LV_PART_TICKS);
  lv_obj_set_style_text_font(chronoMeter, &lv_font_montserrat_14, LV_PART_TICKS);

  // 3. Vintage "km/h" Dial Label
  chronoKmH = lv_label_create(screenChrono);
  lv_obj_set_style_text_font(chronoKmH, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(chronoKmH, lv_color_black(), 0);
  lv_label_set_text(chronoKmH, "km/h");
  lv_obj_align(chronoKmH, LV_ALIGN_CENTER, 0, -30);

  // 4. Vintage Pilot Indicator Jewels (Centered in the middle white blank section)
  const lv_color_t jewelColors[4] = {
    lv_color_make(240, 180, 15), // Amber (Turn)
    lv_color_make(30, 115, 235), // Blue (High Beam)
    lv_color_make(35, 190, 55),  // Green (Neutral)
    lv_color_make(225, 35, 35)   // Red (Warning)
  };
  const lv_coord_t jewelX[4] = {84, 104, 124, 144};
  const lv_coord_t jewelY[4] = {172, 172, 172, 172};
  for (int i = 0; i < 4; i++) {
    lv_obj_t *jewel = lv_obj_create(screenChrono);
    lv_obj_set_size(jewel, 12, 12);
    lv_obj_set_style_radius(jewel, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(jewel, jewelColors[i], 0);
    lv_obj_set_style_border_color(jewel, lv_color_make(50, 50, 50), 0);
    lv_obj_set_style_border_width(jewel, 1, 0);
    lv_obj_set_pos(jewel, jewelX[i], jewelY[i]);
    lv_obj_clear_flag(jewel, LV_OBJ_FLAG_SCROLLABLE);
  }

  // 5. Classic Solid Black Needle (Tapered front pointer + counterweight tail)
  chronoNeedle = lv_line_create(screenChrono);
  lv_obj_set_style_line_width(chronoNeedle, 4, LV_PART_MAIN);
  lv_obj_set_style_line_color(chronoNeedle, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_line_rounded(chronoNeedle, true, LV_PART_MAIN);
  updateChronoNeedle(0.0f);

  // 6. Black Center Hub with Silver Center Rivet (Matching classic motorcycle gauge)
  chronoHub = lv_obj_create(screenChrono);
  lv_obj_set_size(chronoHub, 22, 22);
  lv_obj_center(chronoHub);
  lv_obj_set_style_radius(chronoHub, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(chronoHub, lv_color_black(), 0);
  lv_obj_set_style_border_width(chronoHub, 0, 0);
  lv_obj_clear_flag(chronoHub, LV_OBJ_FLAG_SCROLLABLE);

  chronoRivet = lv_obj_create(chronoHub);
  lv_obj_set_size(chronoRivet, 8, 8);
  lv_obj_center(chronoRivet);
  lv_obj_set_style_radius(chronoRivet, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(chronoRivet, lv_color_make(180, 185, 190), 0);
  lv_obj_set_style_border_width(chronoRivet, 0, 0);
  lv_obj_clear_flag(chronoRivet, LV_OBJ_FLAG_SCROLLABLE);

  // 7. Compact Turn Mini-Banner (hidden by default, only shown if < 150m)
  chronoTurnBanner = lv_obj_create(screenChrono);
  lv_obj_set_size(chronoTurnBanner, 170, 32);
  lv_obj_align(chronoTurnBanner, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_set_style_bg_color(chronoTurnBanner, lv_color_make(20, 20, 20), 0);
  lv_obj_set_style_bg_opa(chronoTurnBanner, LV_OPA_90, 0);
  lv_obj_set_style_radius(chronoTurnBanner, 6, 0);
  lv_obj_set_style_border_color(chronoTurnBanner, lv_color_make(80, 80, 80), 0);
  lv_obj_set_style_border_width(chronoTurnBanner, 1, 0);
  lv_obj_clear_flag(chronoTurnBanner, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(chronoTurnBanner, LV_OBJ_FLAG_HIDDEN);

  chronoTurnGlyph = lv_img_create(chronoTurnBanner);
  lv_img_set_src(chronoTurnGlyph, &arrow_img);
  lv_img_set_pivot(chronoTurnGlyph, 32, 32);
  lv_img_set_zoom(chronoTurnGlyph, 70);
  lv_img_set_antialias(chronoTurnGlyph, true);
  lv_obj_set_style_img_recolor(chronoTurnGlyph, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_img_recolor_opa(chronoTurnGlyph, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(chronoTurnGlyph, LV_ALIGN_LEFT_MID, 4, 0);

  chronoTurnDist = lv_label_create(chronoTurnBanner);
  lv_obj_set_style_text_font(chronoTurnDist, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(chronoTurnDist, lv_color_white(), 0);
  lv_label_set_text(chronoTurnDist, "");
  lv_obj_align(chronoTurnDist, LV_ALIGN_LEFT_MID, 30, 0);
}

static void buildUI() {
  buildScreenMap();
  buildScreenRibbon();
  buildScreenChrono();

  screens[0] = screenMap;
  screens[1] = screenRibbon;
  screens[2] = screenChrono;

  lv_scr_load(screenMap);
  currentScreen = 0;
  activeScreenIndex = 0;
}

static void switchScreen(uint8_t targetIdx, lv_scr_load_anim_t anim) {
  if (targetIdx > 2 || targetIdx == currentScreen) return;
  currentScreen = targetIdx;
  activeScreenIndex = targetIdx;
  lv_scr_load_anim(screens[targetIdx], anim, 250, 0, false);
}

// ================= UI Updates =================
static void applyFrameToMapScreen(const FrameDesc *f) {
  if (!f || !f->valid || !f->buf) return;
  mapImgDsc.data = (const uint8_t *)f->buf;
  lv_img_set_src(mapImgObj, &mapImgDsc);
  lv_obj_invalidate(mapImgObj);

  static char streetCache[64] = "";
  static char distCache[24] = "";
  static char spdCache[24] = "";
  static int16_t lastGlyphAngle = 9999;

  setLabelIfChanged(streetLabelMap, f->street[0] ? f->street : "Bline Navigation", streetCache, sizeof(streetCache));
  setLabelIfChanged(distLabelMap, f->distText[0] ? f->distText : "--", distCache, sizeof(distCache));

  char spdBuf[24];
  snprintf(spdBuf, sizeof(spdBuf), "%.0f km/h", f->speedKmh);
  setLabelIfChanged(speedLabelMap, spdBuf, spdCache, sizeof(spdBuf));

  const lv_point_t *pts = nullptr;
  uint16_t n = 0;
  int16_t glyphAngle = 0;
  geomFor(f->maneuver, &pts, &n, &glyphAngle);
  if (glyphAngle != lastGlyphAngle) {
    lv_img_set_angle(glyphMap, glyphAngle);
    lastGlyphAngle = glyphAngle;
  }

  // Hide top scrim when within 100m of turn to reduce clutter
  int meters = parseMeters(f->distText);
  if (meters >= 0 && meters < 100) {
    setHidden(scrimTopMap, true);
  } else {
    setHidden(scrimTopMap, false);
  }

  setHidden(offlinePillMap, true);
  lastMapFrameMs = millis();
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

static void applyNavSnapshot(const NavSnapshot *s) {
  if (!s) return;
  navActive = s->nav;
  strncpy(navManeuver, s->maneuver, sizeof(navManeuver) - 1);
  navManeuver[sizeof(navManeuver) - 1] = 0;
  strncpy(navDistText, s->distText, sizeof(navDistText) - 1);
  navDistText[sizeof(navDistText) - 1] = 0;
  strncpy(navStreet, s->street, sizeof(navStreet) - 1);
  navStreet[sizeof(navStreet) - 1] = 0;
  strncpy(navEta, s->eta, sizeof(navEta) - 1);
  navEta[sizeof(navEta) - 1] = 0;
  strncpy(navRemain, s->remain, sizeof(navRemain) - 1);
  navRemain[sizeof(navRemain) - 1] = 0;

  navSeq = s->seq;
  navSpeedKmh = s->speedKmh;
  navBearing = s->bearing;
  navTripDistKm = s->tripDistKm;
  navElapsedS = s->elapsedS;
  navMapAvailable = s->mapAvailable;
  lastNavUpdateMs = millis();
}

static void updateRibbonScreen() {
  const bool arrive = !strcmp(navManeuver, "arrive");
  if (!navActive) {
    setHidden(statusLabel, false);
    setHidden(roadOutline, true);
    setHidden(roadLine, true);
    setHidden(posMarker, true);
    setHidden(turnGlyph, true);
    setHidden(progressArc, true);
    setHidden(streetLabel, true);
    setHidden(distNumLabel, true);
    setHidden(distUnitLabel, true);
    static char statusCache[40] = "";
    setLabelIfChanged(statusLabel, "Waiting for nav", statusCache, sizeof(statusCache));
    return;
  }

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

static void updateChronoScreen() {
  updateChronoNeedle(navSpeedKmh);

  // Turn mini-banner overlay only when maneuver is within 150m
  int meters = parseMeters(navDistText);
  if (navActive && meters >= 0 && meters <= 150) {
    setHidden(chronoTurnBanner, false);
    const lv_point_t *pts = nullptr;
    uint16_t n = 0;
    int16_t glyphAngle = 0;
    geomFor(navManeuver, &pts, &n, &glyphAngle);
    lv_img_set_angle(chronoTurnGlyph, glyphAngle);

    char bnrText[32];
    snprintf(bnrText, sizeof(bnrText), "%s  %s", navManeuver, navDistText);
    lv_label_set_text(chronoTurnDist, bnrText);
  } else {
    setHidden(chronoTurnBanner, true);
  }
}

static void updateActiveScreenUI() {
  if (currentScreen == 0) {
    // If on map screen and no frame received for > 2500ms, show grace-period indicator
    if (millis() - lastMapFrameMs > 2500) {
      setHidden(offlinePillMap, false);
    }
  } else if (currentScreen == 1) {
    updateRibbonScreen();
  } else if (currentScreen == 2) {
    updateChronoScreen();
  }
}

// ================= TJpgDec Block Callback (Core 0) =================
static bool mapBlockCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (!decodeTargetBuf) return false;
  for (int r = 0; r < h; r++) {
    int curY = y + r;
    if (curY < 0 || curY >= SCREEN_H) continue;
    int curX = x;
    int copyW = w;
    if (curX < 0) { copyW += curX; curX = 0; }
    if (curX + copyW > SCREEN_W) copyW = SCREEN_W - curX;
    if (copyW <= 0) continue;
    memcpy(&decodeTargetBuf[curY * SCREEN_W + curX], &bitmap[r * w], copyW * sizeof(uint16_t));
  }
  return true;
}

// ================= URL Builders =================
static String mapUrl() {
  if (strlen(NAV_SERVER_HOST) > 0) {
    return String("http://") + NAV_SERVER_HOST + ":" + String(NAV_SERVER_PORT) + "/map";
  }
  return String("http://") + WiFi.gatewayIP().toString() + ":" + String(NAV_SERVER_PORT) + "/map";
}

static String navUrl() {
  if (strlen(NAV_SERVER_HOST) > 0) {
    return String("http://") + NAV_SERVER_HOST + ":" + String(NAV_SERVER_PORT) + "/nav";
  }
  return String("http://") + WiFi.gatewayIP().toString() + ":" + String(NAV_SERVER_PORT) + "/nav";
}

// ================= Core 0 Dedicated Network Task =================
static bool probeMapTile(uint16_t *targetBuf) {
  if (!targetBuf || !jpegDownloadBuf) return false;
  HTTPClient http;
  WiFiClient client;
  http.setTimeout(2500);
  const char *headerKeys[] = {"X-Nav-Dist", "X-Nav-Maneuver", "X-Nav-Speed", "X-Nav-Street", "X-Nav-Eta", "Content-Type", "Content-Length"};
  http.collectHeaders(headerKeys, 7);

  bool success = false;
  if (http.begin(client, mapUrl())) {
    int code = http.GET();
    if (code == 200) {
      String distText = http.header("X-Nav-Dist");
      String maneuver = http.header("X-Nav-Maneuver");
      String speedStr = http.header("X-Nav-Speed");
      String street = http.header("X-Nav-Street");
      String eta = http.header("X-Nav-Eta");

      int len = http.getSize();
      WiFiClient *stream = http.getStreamPtr();
      int bytesRead = 0;
      if (stream) {
        while (http.connected() && bytesRead < 32 * 1024) {
          size_t avail = stream->available();
          if (avail) {
            int c = stream->readBytes(jpegDownloadBuf + bytesRead, min((int)avail, 32 * 1024 - bytesRead));
            bytesRead += c;
            if (len > 0 && bytesRead >= len) break;
          } else {
            delay(2);
            if (stream->available() == 0 && (len <= 0 || bytesRead >= len)) break;
          }
        }
      }

      if (bytesRead > 0) {
        decodeTargetBuf = targetBuf;
        TJpgDec.setJpgScale(1);
        TJpgDec.setSwapBytes(false);
        TJpgDec.setCallback(mapBlockCallback);
        JRESULT res = TJpgDec.drawJpg(0, 0, (const uint8_t *)jpegDownloadBuf, bytesRead);
        if (res == JDR_OK) {
          esp_cache_msync_compat(targetBuf, SCREEN_W * SCREEN_H * sizeof(uint16_t));
          FrameDesc frame;
          frame.buf = targetBuf;
          strncpy(frame.distText, distText.c_str(), sizeof(frame.distText) - 1);
          frame.distText[sizeof(frame.distText) - 1] = 0;
          strncpy(frame.maneuver, maneuver.c_str(), sizeof(frame.maneuver) - 1);
          frame.maneuver[sizeof(frame.maneuver) - 1] = 0;
          frame.speedKmh = speedStr.toFloat();
          strncpy(frame.street, street.c_str(), sizeof(frame.street) - 1);
          frame.street[sizeof(frame.street) - 1] = 0;
          strncpy(frame.eta, eta.c_str(), sizeof(frame.eta) - 1);
          frame.eta[sizeof(frame.eta) - 1] = 0;
          frame.timestampMs = millis();
          frame.valid = true;
          xQueueOverwrite(frameQueue, &frame);
          success = true;
        }
      }
    } else {
      static uint32_t lastErrPrint = 0;
      if (millis() - lastErrPrint >= 3000) {
        lastErrPrint = millis();
        Serial.printf("[HTTP] /map failed (code %d) at %s\n", code, mapUrl().c_str());
      }
    }
    http.end();
  }
  return success;
}

static void pollNavJson() {
  HTTPClient http;
  WiFiClient client;
  http.setTimeout(1200);
  if (!http.begin(client, navUrl())) return;

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      NavSnapshot snap;
      snap.nav = doc["nav"] | false;
      const char *m = doc["maneuver"] | "straight";
      strncpy(snap.maneuver, m, sizeof(snap.maneuver) - 1);
      snap.maneuver[sizeof(snap.maneuver) - 1] = 0;

      const char *d = doc["dist_text"] | "";
      strncpy(snap.distText, d, sizeof(snap.distText) - 1);
      snap.distText[sizeof(snap.distText) - 1] = 0;

      const char *s = doc["street"] | "";
      strncpy(snap.street, s, sizeof(snap.street) - 1);
      snap.street[sizeof(snap.street) - 1] = 0;

      const char *e = doc["eta"] | "";
      strncpy(snap.eta, e, sizeof(snap.eta) - 1);
      snap.eta[sizeof(snap.eta) - 1] = 0;

      const char *r = doc["remain"] | "";
      strncpy(snap.remain, r, sizeof(snap.remain) - 1);
      snap.remain[sizeof(snap.remain) - 1] = 0;

      snap.seq = doc["seq"] | 0;
      snap.speedKmh = doc["speed_kmh"] | 0.0f;
      snap.bearing = doc["bearing"] | 0.0f;
      snap.tripDistKm = doc["trip_dist_km"] | 0.0f;
      snap.elapsedS = doc["elapsed_s"] | 0;
      snap.mapAvailable = doc["map_available"] | false;
      snap.timestampMs = millis();

      xQueueOverwrite(navQueue, &snap);
    }
  } else {
    static uint32_t lastNavErrPrint = 0;
    if (millis() - lastNavErrPrint >= 3000) {
      lastNavErrPrint = millis();
      Serial.printf("[HTTP] /nav failed (code %d) at %s\n", code, navUrl().c_str());
    }
  }
  http.end();
}

static void TaskNetworkCode(void *pvParameters) {
  uint32_t lastPollMs = 0;
  uint32_t lastProbeMs = 0;
  uint32_t lastMapSuccessMs = millis();
  int mapConsecutiveFailures = 0;
  int probeConsecutiveSuccesses = 0;
  uint16_t *backBuf = mapBufPing;

  while (true) {
    uint32_t now = millis();

    // WiFi maintenance exclusively on Core 0
    static bool wifiWasConnected = false;
    if (WiFi.status() == WL_CONNECTED) {
      if (!wifiWasConnected) {
        wifiWasConnected = true;
        Serial.printf("[WiFi] Connected! IP: %s, Gateway: %s\n",
                      WiFi.localIP().toString().c_str(),
                      WiFi.gatewayIP().toString().c_str());
        Serial.printf("[Nav] Polling endpoint: %s\n", mapUrl().c_str());
      }
    } else {
      if (wifiWasConnected) {
        wifiWasConnected = false;
        Serial.println("[WiFi] Lost connection! Reconnecting...");
      }
      static uint32_t lastWifiTry = 0;
      if (lastWifiTry == 0) lastWifiTry = now;
      if (now - lastWifiTry >= 8000) {
        lastWifiTry = now;
        Serial.printf("[WiFi] Reconnecting to '%s'...\n", WIFI_SSID);
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    uint8_t screen = activeScreenIndex;
    bool isFallback = fallbackMode;

    if (screen == 0 && !isFallback) {
      // Screen 1 active: fetch map at 1.0 Hz
      if (now - lastPollMs >= 1000) {
        lastPollMs = now;
        bool ok = probeMapTile(backBuf);
        if (ok) {
          backBuf = (backBuf == mapBufPing) ? mapBufPong : mapBufPing;
          mapConsecutiveFailures = 0;
          lastMapSuccessMs = now;
        } else {
          mapConsecutiveFailures++;
          // Failover hysteresis: continuous failure > 6s (or >= 3 failures)
          if (mapConsecutiveFailures >= 3 || (now - lastMapSuccessMs > 6000)) {
            fallbackMode = true;
            probeConsecutiveSuccesses = 0;
          }
        }
      }
    } else {
      // Screen 2, Screen 3, or Fallback: poll JSON /nav at 1.0 Hz
      if (now - lastPollMs >= 800) {
        lastPollMs = now;
        pollNavJson();
      }

      // If in fallback mode, probe /map every 5 seconds
      if (fallbackMode && (now - lastProbeMs >= 5000)) {
        lastProbeMs = now;
        bool probeOk = probeMapTile(backBuf);
        if (probeOk) {
          probeConsecutiveSuccesses++;
          if (probeConsecutiveSuccesses >= 2) {
            fallbackMode = false;
            probeConsecutiveSuccesses = 0;
            mapConsecutiveFailures = 0;
            lastMapSuccessMs = now;
            backBuf = (backBuf == mapBufPing) ? mapBufPong : mapBufPing;
          }
        } else {
          probeConsecutiveSuccesses = 0;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ================= Display & Hardware Init =================
static bool initDrawBuffer() {
  size_t bytes = DRAW_BUF_PIXELS * sizeof(lv_color_t);
  draw_buf1 = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  draw_buf2 = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!draw_buf1) draw_buf1 = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL);
  if (!draw_buf2) draw_buf2 = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL);
  return (draw_buf1 != nullptr);
}

static bool initDisplayAndLvgl() {
#ifdef TFT_BL
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
#endif

  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  if (!initDrawBuffer()) {
    Serial.println("FATAL: LVGL draw buffer allocation failed");
    return false;
  }

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, draw_buf1, draw_buf2, DRAW_BUF_PIXELS);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_W;
  disp_drv.ver_res = SCREEN_H;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read_cb;
  lv_indev_drv_register(&indev_drv);

  buildUI();
  return true;
}

static void initWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ================= Setup & Loop =================
void setup() {
  Serial.begin(115200);
  for (uint32_t t0 = millis(); !Serial && (millis() - t0) < 2000;) {
    delay(10);
  }

  Serial.println("Bline Tri-Screen Dashboard Starting...");
  Serial.printf("Heap: %u, PSRAM: %u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

  // PSRAM Dynamic Allocations for Double Buffering & TJpgDec
  mapBufPing = (uint16_t *)heap_caps_malloc(SCREEN_W * SCREEN_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  mapBufPong = (uint16_t *)heap_caps_malloc(SCREEN_W * SCREEN_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  jpegDownloadBuf = (uint8_t *)heap_caps_malloc(32 * 1024, MALLOC_CAP_SPIRAM);

  if (!mapBufPing || !mapBufPong || !jpegDownloadBuf) {
    Serial.println("ERROR: Failed to allocate PSRAM map framebuffers!");
  } else {
    memset(mapBufPing, 0, SCREEN_W * SCREEN_H * sizeof(uint16_t));
    memset(mapBufPong, 0, SCREEN_W * SCREEN_H * sizeof(uint16_t));
    mapImgDsc.data = (const uint8_t *)mapBufPing;
  }

  frameQueue = xQueueCreate(1, sizeof(FrameDesc));
  navQueue = xQueueCreate(1, sizeof(NavSnapshot));

  // Initialize CST816S Hardware Touch Driver
  if (!touch.begin()) {
    Serial.println("WARN: CST816S touch controller did not acknowledge I2C");
  } else {
    Serial.println("CST816S touch controller OK");
  }

  initWiFi();

  if (!initDisplayAndLvgl()) {
    Serial.println("HALTED: Display/LVGL init failed");
    return;
  }

  // Pin Network & Decoding Task to Core 0 (Priority 3, Stack 12 KB)
  xTaskCreatePinnedToCore(
    TaskNetworkCode,
    "TaskNetwork",
    12 * 1024,
    NULL,
    3,
    &taskNetworkHandle,
    0
  );

  Serial.println("Core 0 Network Task Spawned. Starting Core 1 UI loop.");
}

void loop() {
  uint32_t now = millis();

  // 1. LVGL Tick Management
  static uint32_t last_tick_ms = 0;
  if (last_tick_ms == 0) last_tick_ms = now;
  uint32_t dt = now - last_tick_ms;
  if (dt > 0) {
    lv_tick_inc(dt);
    last_tick_ms = now;
  }

  // Touch is polled exclusively via LVGL indev driver touch_read_cb()
  // to avoid I2C multi-master collision and register read races.

  // 2. Receive Inter-Core Descriptors
  FrameDesc frame;
  if (xQueueReceive(frameQueue, &frame, 0) == pdTRUE) {
    applyFrameToMapScreen(&frame);
  }

  NavSnapshot snap;
  if (xQueueReceive(navQueue, &snap, 0) == pdTRUE) {
    applyNavSnapshot(&snap);
  }

  // 3. Automatic Failover & Recovery State Machine
  if (activeScreenIndex == 0 && fallbackMode && !autoFailoverActive && !suppressFailover) {
    autoFailoverActive = true;
    switchScreen(1, LV_SCR_LOAD_ANIM_FADE_ON);
  } else if (autoFailoverActive && !fallbackMode) {
    autoFailoverActive = false;
    switchScreen(0, LV_SCR_LOAD_ANIM_FADE_ON);
  }
  if (!fallbackMode) {
    suppressFailover = false;
  }

  // 5. Active Screen Refresh
  static uint32_t lastUiMs = 0;
  if (now - lastUiMs >= 80) {
    lastUiMs = now;
    updateActiveScreenUI();
  }

  // 6. LVGL Task Handler
  lv_timer_handler();

  esp_task_wdt_reset();
  delay(5);
}

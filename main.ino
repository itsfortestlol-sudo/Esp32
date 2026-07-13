/***********************************************************************
 * MochiPod - ESP32 Handheld (Mochi Gen2 / Watch / Flappy Bird / OTA)
 * Board  : ESP32 DevKit V1 (ESP-WROOM-32)
 * Display: SH1106 128x64 I2C
 * Touch  : TTP223 (GPIO 4, active HIGH)
 * NO external JSON library needed - uses inline parser
 ***********************************************************************/
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include "secrets.h"

#define SSD1306_WHITE SH110X_WHITE
#define SSD1306_BLACK SH110X_BLACK

/* ============================ CONFIG ================================ */
const long  GMT_OFFSET_SEC  = 21600;
const int   DST_OFFSET_SEC  = 0;
const char* NTP_SERVER      = "asia.pool.ntp.org";

const int   CURRENT_VERSION = 2;
const char* VERSION_URL     = "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/virsion.txt";
const char* BIN_URL         = "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/firmware.bin";

/* ------------------------------ Pins ------------------------------- */
#define PIN_OLED_SDA 21
#define PIN_OLED_SCL 22
#define PIN_TOUCH    4
#define PIN_VBAT     34

/* ------------------------- Battery calibration --------------------- */
const float VBAT_DIVIDER = 2.0f;
const float VBAT_CAL     = 1.00f;
const float VBAT_FULL    = 4.20f;
const float VBAT_EMPTY   = 3.30f;

/* --------------------------- Touch timing -------------------------- */
const unsigned long TOUCH_DEBOUNCE_MS    = 30;
const unsigned long DOUBLE_TAP_WINDOW_MS = 300;

/* ------------------------------ Idle ------------------------------- */
const unsigned long IDLE_TIMEOUT = 20000;

/* ========================= Sleep window ============================ */
const int SLEEP_HOUR_START = 22;
const int SLEEP_HOUR_END   = 8;

/* ========================= AI Timing =============================== */
const unsigned long AI_POLL_INTERVAL = 60000;

/* =========================== App Modes ============================= */
enum AppMode  { MODE_MOCHI, MODE_WATCH, MODE_FLAPPY, MODE_UPDATE };
enum TapEvent { TAP_NONE, TAP_SINGLE, TAP_DOUBLE };

/* ====================== Tiny JSON Helpers =========================
 * No library needed. Extracts string/bool/int values from a flat
 * JSON string. Works perfectly for Gemini's simple response.
 * ================================================================ */
namespace TinyJSON {

  /* Find the value string for a given key in a JSON object string.
   * Returns empty string if not found. */
  String extractString(const String& json, const String& key) {
    String search = "\"" + key + "\"";
    int ki = json.indexOf(search);
    if (ki < 0) return "";
    int ci = json.indexOf(':', ki + search.length());
    if (ci < 0) return "";
    ci++;
    while (ci < (int)json.length() && json[ci] == ' ') ci++;
    if (json[ci] == '"') {
      int start = ci + 1;
      int end   = json.indexOf('"', start);
      if (end < 0) return "";
      return json.substring(start, end);
    }
    // Not a string — return raw token (for bool/number)
    int end = ci;
    while (end < (int)json.length() &&
           json[end] != ',' && json[end] != '}' && json[end] != '\n')
      end++;
    String tok = json.substring(ci, end);
    tok.trim();
    return tok;
  }

  bool extractBool(const String& json, const String& key, bool def = false) {
    String v = extractString(json, key);
    if (v == "true")  return true;
    if (v == "false") return false;
    return def;
  }

  int extractInt(const String& json, const String& key, int def = 0) {
    String v = extractString(json, key);
    if (v.length() == 0) return def;
    return v.toInt();
  }

  /* Find the inner JSON from Gemini's wrapper:
   * candidates[0].content.parts[0].text  */
  String extractGeminiText(const String& resp) {
    // Look for "text": "<content>"
    // The inner JSON may itself contain escaped quotes — handle carefully
    String searchKey = "\"text\":";
    int ki = resp.indexOf(searchKey);
    if (ki < 0) return "";
    int ci = ki + searchKey.length();
    while (ci < (int)resp.length() && resp[ci] == ' ') ci++;
    if (resp[ci] != '"') return "";
    ci++; // skip opening quote
    String result = "";
    while (ci < (int)resp.length()) {
      char c = resp[ci];
      if (c == '\\' && ci + 1 < (int)resp.length()) {
        char nc = resp[ci + 1];
        if (nc == '"')       { result += '"';  ci += 2; continue; }
        if (nc == '\\')      { result += '\\'; ci += 2; continue; }
        if (nc == 'n')       { result += '\n'; ci += 2; continue; }
        if (nc == 'r')       { result += '\r'; ci += 2; continue; }
        if (nc == 't')       { result += '\t'; ci += 2; continue; }
        result += c; ci++; continue;
      }
      if (c == '"') break; // closing quote
      result += c;
      ci++;
    }
    return result;
  }

} // namespace TinyJSON

/* ======================== DisplayManager =========================== */
class DisplayManager {
public:
  Adafruit_SH1106G oled;
  DisplayManager() : oled(128, 64, &Wire, -1) {}

  bool begin() {
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    Wire.setClock(400000);
    if (!oled.begin(0x3C, true)) return false;
    oled.clearDisplay();
    oled.setTextColor(SH110X_WHITE);
    oled.display();
    return true;
  }

  void centerText(const char* s, int16_t y, uint8_t size) {
    oled.setTextSize(size);
    int16_t w = (int16_t)(strlen(s) * 6 * size);
    oled.setCursor((128 - w) / 2, y);
    oled.print(s);
  }
};

/* ========================= TouchManager ============================ */
class TouchManager {
  bool stableState         = false;
  bool lastRead            = false;
  unsigned long lastChangeMs    = 0;
  unsigned long firstTapMs      = 0;
  bool waitingSecondTap         = false;
public:
  void begin() { pinMode(PIN_TOUCH, INPUT); }

  TapEvent update() {
    unsigned long now = millis();
    TapEvent ev = TAP_NONE;
    bool raw = (digitalRead(PIN_TOUCH) == HIGH);

    if (raw != lastRead) { lastRead = raw; lastChangeMs = now; }

    if ((now - lastChangeMs) >= TOUCH_DEBOUNCE_MS && raw != stableState) {
      stableState = raw;
      if (stableState) {
        if (waitingSecondTap && (now - firstTapMs) <= DOUBLE_TAP_WINDOW_MS) {
          waitingSecondTap = false;
          ev = TAP_DOUBLE;
        } else {
          firstTapMs       = now;
          waitingSecondTap = true;
        }
      }
    }
    if (waitingSecondTap && (now - firstTapMs) > DOUBLE_TAP_WINDOW_MS) {
      waitingSecondTap = false;
      ev = TAP_SINGLE;
    }
    return ev;
  }
};

/* ======================== BatteryManager =========================== */
class BatteryManager {
  unsigned long lastReadMs = 0;
  float voltage  = 3.7f;
  int   percent  = 50;
public:
  void begin() {
    analogSetPinAttenuation(PIN_VBAT, ADC_11db);
    sample();
  }
  void update() { if (millis() - lastReadMs >= 5000) sample(); }
  void sample() {
    lastReadMs = millis();
    uint32_t mv = 0;
    for (int i = 0; i < 10; i++) mv += analogReadMilliVolts(PIN_VBAT);
    voltage = (mv / 10) / 1000.0f * VBAT_DIVIDER * VBAT_CAL;
    float p = (voltage - VBAT_EMPTY) / (VBAT_FULL - VBAT_EMPTY) * 100.0f;
    percent = constrain((int)(p + 0.5f), 0, 100);
  }
  int   getPercent() const { return percent; }
  float getVoltage() const { return voltage; }
};

/* ========================== TimeManager ============================ */
class TimeManager {
  enum St { CONNECTING, SYNCING, DONE };
  St   state       = CONNECTING;
  unsigned long startMs      = 0;
  unsigned long lastRetryMs  = 0;
  bool synced      = false;
public:
  void begin()     { startSync(); }

  void startSync() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    state   = CONNECTING;
    startMs = millis();
  }

  void update() {
    unsigned long now = millis();
    if (state == CONNECTING) {
      if (WiFi.status() == WL_CONNECTED) {
        configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
        state = SYNCING; startMs = now;
      } else if (now - startMs > 30000) {
        shutdownWifi(); lastRetryMs = now;
      }
    } else if (state == SYNCING) {
      if (time(nullptr) > 1700000000UL) {
        synced = true; shutdownWifi();
      } else if (now - startMs > 15000) {
        shutdownWifi(); lastRetryMs = now;
      }
    } else if (state == DONE && !synced) {
      if (now - lastRetryMs > 60000) startSync();
    }
  }

  void shutdownWifi() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    state = DONE;
  }

  bool isSynced() const { return synced; }

  void getLocal(struct tm& t) {
    time_t current = time(nullptr);
    localtime_r(&current, &t);
  }

  bool isSleepTime() {
    if (!synced) return false;
    struct tm t; getLocal(t);
    int h = t.tm_hour;
    return (h >= SLEEP_HOUR_START || h < SLEEP_HOUR_END);
  }

  int getHour() {
    struct tm t; getLocal(t); return t.tm_hour;
  }
  int getMinute() {
    struct tm t; getLocal(t); return t.tm_min;
  }
};

/* ============================ MochiApp ============================== */
class MochiApp {
  DisplayManager* dm = nullptr;
  TimeManager*    tm = nullptr;

public:
  enum MochiExpression {
    EXPR_NORMAL, EXPR_HAPPY, EXPR_SAD, EXPR_EXCITED,
    EXPR_SLEEPY, EXPR_SLEEPING, EXPR_ANNOYED, EXPR_BORED,
    EXPR_SHOCKED, EXPR_LOVE, EXPR_THINKING, EXPR_PLAYFUL, EXPR_ANGRY
  };

private:
  struct MochiAction {
    MochiExpression expression = EXPR_NORMAL;
    bool showPhone     = false;
    bool showCoffee    = false;
    bool doLookAround  = false;
    bool doBounce      = true;
    unsigned long durationMs = 8000;
  };

  MochiAction currentAction;

  /* Touch reaction */
  bool touchReacting      = false;
  unsigned long touchReactEndMs = 0;
  MochiExpression touchExpr     = EXPR_HAPPY;

  /* AI poll tracking */
  unsigned long lastAIPollMs  = 0;
  unsigned long actionStartMs = 0;

  /* Animation */
  float bouncePhase  = 0.0f;
  float lookPhase    = 0.0f;
  float sparklePhase = 0.0f;

  /* Blink */
  bool blinking      = false;
  unsigned long blinkStartMs = 0;
  unsigned long nextBlinkMs  = 0;

  /* Zzz */
  struct ZzzParticle {
    float x, y, vy;
    int   size;
    bool  active;
    float life; // 0..1
  };
  static const int ZZZ_MAX = 4;
  ZzzParticle zzz[ZZZ_MAX];
  unsigned long nextZzzMs = 0;

  /* Coffee */
  float coffeeSip  = 0.0f;
  bool  sipping    = false;
  unsigned long sipEndMs = 0;

  /* Phone */
  int  phoneScrollY  = 0;
  unsigned long nextScrollMs = 0;

  unsigned long lastFrameMs = 0;

public:
  void begin(DisplayManager* d, TimeManager* t) {
    dm = d; tm = t;
    initZzz();
    nextBlinkMs   = millis() + random(2000, 4000);
    actionStartMs = millis();
    currentAction.expression = EXPR_NORMAL;
    currentAction.doBounce   = true;
    currentAction.durationMs = 8000;
  }

  void onEnter() {
    touchReacting = false;
    currentAction.expression = EXPR_NORMAL;
    currentAction.doBounce   = true;
    currentAction.durationMs = 8000;
    actionStartMs = millis();
  }

  /* Touch: always code-driven, instant, no AI call */
  void onSingleTap() {
    unsigned long now = millis();
    if (currentAction.expression == EXPR_SLEEPING || touchExpr == EXPR_SLEEPING) {
      touchExpr       = EXPR_ANGRY;
      touchReacting   = true;
      touchReactEndMs = now + 3000;
    } else {
      int r = random(0, 4);
      touchExpr = (r == 0) ? EXPR_HAPPY  :
                  (r == 1) ? EXPR_LOVE   :
                  (r == 2) ? EXPR_PLAYFUL: EXPR_EXCITED;
      touchReacting   = true;
      touchReactEndMs = now + 1500;
    }
  }

  /* Called by AIManager with parsed action */
  void applyAIAction(MochiExpression expr, bool phone, bool coffee,
                     bool lookAround, bool bounce, unsigned long durMs) {
    if (tm->isSleepTime() && expr != EXPR_SLEEPING) return;
    currentAction.expression  = expr;
    currentAction.showPhone   = phone;
    currentAction.showCoffee  = coffee;
    currentAction.doLookAround= lookAround;
    currentAction.doBounce    = bounce;
    currentAction.durationMs  = durMs;
    actionStartMs = millis();
    coffeeSip = 0.0f; sipping = false; sipEndMs = millis() + 1500;
    phoneScrollY = 0;  nextScrollMs = millis() + 600;
    initZzz(); nextZzzMs = millis();
  }

  bool shouldPollAI() {
    if (tm->isSleepTime()) return false;
    return (millis() - lastAIPollMs >= AI_POLL_INTERVAL);
  }
  void markAIPollDone() { lastAIPollMs = millis(); }

  MochiExpression expressionFromString(const String& s) {
    if (s == "happy")    return EXPR_HAPPY;
    if (s == "sad")      return EXPR_SAD;
    if (s == "excited")  return EXPR_EXCITED;
    if (s == "sleepy")   return EXPR_SLEEPY;
    if (s == "sleeping") return EXPR_SLEEPING;
    if (s == "annoyed")  return EXPR_ANNOYED;
    if (s == "bored")    return EXPR_BORED;
    if (s == "shocked")  return EXPR_SHOCKED;
    if (s == "love")     return EXPR_LOVE;
    if (s == "thinking") return EXPR_THINKING;
    if (s == "playful")  return EXPR_PLAYFUL;
    if (s == "angry")    return EXPR_ANGRY;
    return EXPR_NORMAL;
  }

  void update() {
    unsigned long now = millis();
    if (now - lastFrameMs < 33) return;
    lastFrameMs = now;

    /* Sleep override */
    if (tm->isSleepTime()) {
      if (currentAction.expression != EXPR_SLEEPING && !touchReacting) {
        currentAction = MochiAction();
        currentAction.expression = EXPR_SLEEPING;
        currentAction.doBounce   = false;
        actionStartMs = now;
        initZzz(); nextZzzMs = now;
      }
    } else {
      if (currentAction.expression == EXPR_SLEEPING && !touchReacting) {
        currentAction.expression = EXPR_HAPPY;
        currentAction.doBounce   = true;
        currentAction.durationMs = 5000;
        actionStartMs = now;
      }
    }

    /* Touch expiry */
    if (touchReacting && now >= touchReactEndMs) {
      touchReacting = false;
      if (touchExpr == EXPR_ANGRY)
        currentAction.expression = EXPR_SLEEPING;
    }

    /* Blink */
    MochiExpression displayExpr = touchReacting ? touchExpr : currentAction.expression;
    bool canBlink = (displayExpr != EXPR_SLEEPING && displayExpr != EXPR_SHOCKED &&
                     displayExpr != EXPR_ANGRY    && displayExpr != EXPR_LOVE);
    if (canBlink) {
      if (!blinking && now >= nextBlinkMs) { blinking = true; blinkStartMs = now; }
      if (blinking && now - blinkStartMs > 150) {
        blinking = false; nextBlinkMs = now + random(2500, 5000);
      }
    } else { blinking = false; }

    /* Phases */
    if (currentAction.doBounce || touchReacting) {
      bouncePhase += 0.04f;
      if (bouncePhase > TWO_PI) bouncePhase -= TWO_PI;
    }
    if (currentAction.doLookAround) {
      lookPhase += 0.02f;
      if (lookPhase > TWO_PI) lookPhase -= TWO_PI;
    } else { lookPhase = 0.0f; }
    sparklePhase += 0.08f;
    if (sparklePhase > TWO_PI) sparklePhase -= TWO_PI;

    /* Coffee sip */
    if (currentAction.showCoffee && !touchReacting) {
      if (!sipping && now >= sipEndMs) { sipping = true; sipEndMs = now + 700; }
      else if (sipping && now >= sipEndMs) {
        sipping = false; sipEndMs = now + random(1500, 3000); coffeeSip = 0.0f;
      }
      if (sipping) {
        float prog = 1.0f - (float)(sipEndMs - now) / 700.0f;
        coffeeSip = sinf(prog * PI);
      }
    }

    /* Phone scroll */
    if (currentAction.showPhone && !touchReacting && now >= nextScrollMs) {
      phoneScrollY = random(0, 8); nextScrollMs = now + random(500, 1200);
    }

    /* Zzz */
    if (displayExpr == EXPR_SLEEPING) updateZzz(now);

    draw(now);
  }

private:
  void initZzz() {
    for (int i = 0; i < ZZZ_MAX; i++) {
      zzz[i] = {85.0f, 20.0f, -0.35f, 1, false, 1.0f};
    }
  }

  void spawnZzz(unsigned long now) {
    if (now < nextZzzMs) return;
    nextZzzMs = now + 1100;
    for (int i = 0; i < ZZZ_MAX; i++) {
      if (!zzz[i].active) {
        zzz[i].active = true;
        zzz[i].x     = 84.0f + random(-2, 3);
        zzz[i].y     = 20.0f;
        zzz[i].vy    = -0.30f - (i % 3) * 0.04f;
        zzz[i].size  = (i % 3 == 0) ? 2 : 1;
        zzz[i].life  = 1.0f;
        break;
      }
    }
  }

  void updateZzz(unsigned long now) {
    spawnZzz(now);
    for (int i = 0; i < ZZZ_MAX; i++) {
      if (!zzz[i].active) continue;
      zzz[i].y    += zzz[i].vy;
      zzz[i].x    += 0.12f;
      zzz[i].life -= 0.008f;
      if (zzz[i].y < 2 || zzz[i].x > 126 || zzz[i].life <= 0)
        zzz[i].active = false;
    }
  }

  void drawZzz(Adafruit_SH1106G& o) {
    for (int i = 0; i < ZZZ_MAX; i++) {
      if (!zzz[i].active) continue;
      o.setTextSize(zzz[i].size);
      o.setCursor((int)zzz[i].x, (int)zzz[i].y);
      o.print("z");
    }
    o.setTextSize(1);
  }

  void drawPhone(Adafruit_SH1106G& o, int px, int py) {
    o.drawRoundRect(px, py, 14, 20, 2, SSD1306_WHITE);
    o.fillRect(px + 2, py + 2, 10, 14, SSD1306_WHITE);
    int lineY = py + 3 + (phoneScrollY % 4);
    for (int l = 0; l < 3; l++) {
      int ly = lineY + l * 4;
      if (ly >= py + 2 && ly < py + 15)
        o.drawFastHLine(px + 3, ly, 8, SSD1306_BLACK);
    }
    o.drawCircle(px + 7, py + 17, 1, SSD1306_WHITE);
  }

  void drawCoffeeCup(Adafruit_SH1106G& o, int cx, int cy, float sip) {
    int tx = (int)(sip * 4.0f), ty = (int)(sip * 2.0f);
    o.drawLine(cx, cy + 4 - ty, cx + 1, cy + 12, SSD1306_WHITE);
    o.drawLine(cx + 11 - tx, cy + 4 - ty, cx + 9, cy + 12, SSD1306_WHITE);
    o.drawFastHLine(cx + 1, cy + 12, 8, SSD1306_WHITE);
    o.drawFastHLine(cx, cy + 4 - ty, 11 - tx, SSD1306_WHITE);
    o.drawLine(cx + 11 - tx, cy + 6 - ty, cx + 13 - tx, cy + 6 - ty, SSD1306_WHITE);
    o.drawLine(cx + 13 - tx, cy + 6 - ty, cx + 13 - tx, cy + 10, SSD1306_WHITE);
    o.drawLine(cx + 13 - tx, cy + 10, cx + 9, cy + 10, SSD1306_WHITE);
    if (sip < 0.2f) {
      o.drawPixel(cx + 3, cy + 2, SSD1306_WHITE);
      o.drawPixel(cx + 6, cy + 1, SSD1306_WHITE);
      o.drawPixel(cx + 9, cy + 2, SSD1306_WHITE);
    }
  }

  /* Tiny ellipse fill helper */
  void fillEllipseSimple(Adafruit_SH1106G& o, int cx, int cy,
                          int rx, int ry, uint16_t color) {
    for (int dy = -ry; dy <= ry; dy++) {
      if (rx == 0) continue;
      float ratio = 1.0f - (float)(dy * dy) / (float)(ry * ry);
      if (ratio < 0) continue;
      int xw = (int)(rx * sqrtf(ratio));
      o.drawFastHLine(cx - xw, cy + dy, xw * 2 + 1, color);
    }
  }

  enum MouthType { MOUTH_SMALL, MOUTH_BIG, MOUTH_SAD };

  void drawMouth(Adafruit_SH1106G& o, int cx, int cy, MouthType mt) {
    switch (mt) {
      case MOUTH_SMALL:
        // Cute W-dot mouth like the photo
        o.fillRect(cx - 6, cy,     4, 3, SSD1306_WHITE);
        o.fillRect(cx + 2, cy,     4, 3, SSD1306_WHITE);
        o.fillRect(cx - 2, cy + 2, 4, 3, SSD1306_WHITE);
        break;
      case MOUTH_BIG:
        o.drawLine(55, cy,     60, cy + 5, SSD1306_WHITE);
        o.drawLine(60, cy + 5, 64, cy + 2, SSD1306_WHITE);
        o.drawLine(64, cy + 2, 68, cy + 5, SSD1306_WHITE);
        o.drawLine(68, cy + 5, 73, cy,     SSD1306_WHITE);
        o.drawLine(55, cy + 1, 60, cy + 6, SSD1306_WHITE);
        o.drawLine(68, cy + 6, 73, cy + 1, SSD1306_WHITE);
        break;
      case MOUTH_SAD:
        o.drawLine(58, cy + 4, 62, cy + 1, SSD1306_WHITE);
        o.drawLine(62, cy + 1, 66, cy + 1, SSD1306_WHITE);
        o.drawLine(66, cy + 1, 70, cy + 4, SSD1306_WHITE);
        o.drawLine(58, cy + 5, 62, cy + 2, SSD1306_WHITE);
        o.drawLine(66, cy + 2, 70, cy + 5, SSD1306_WHITE);
        break;
    }
  }

  void drawHappyEyeFill(Adafruit_SH1106G& o, int lx, int rx, int ey, int ew) {
    // Solid filled upward-arc eyes (happy ^ ^)
    for (int row = 0; row < 14; row++) {
      float t   = (float)row / 14.0f;
      int   xOff = (int)(ew/2 * sinf(t * PI));
      int   yPos = ey + 14 - row;
      if (yPos >= 0 && yPos < 64) {
        o.drawFastHLine(lx + ew/2 - xOff, yPos, xOff * 2 + 1, SSD1306_WHITE);
        o.drawFastHLine(rx + ew/2 - xOff, yPos, xOff * 2 + 1, SSD1306_WHITE);
      }
    }
  }

  void drawHeartAt(Adafruit_SH1106G& o, int cx, int cy, int r) {
    o.fillCircle(cx - r/2, cy - r/3, r, SSD1306_WHITE);
    o.fillCircle(cx + r/2, cy - r/3, r, SSD1306_WHITE);
    o.fillTriangle(cx - r, cy, cx + r, cy, cx, cy + r + 2, SSD1306_WHITE);
  }

  /* ===========================================================
   * DRAW — two big rounded-rect eyes + expressive mouth
   * Mochi face exactly like the photo
   * =========================================================== */
  void draw(unsigned long now) {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();

    MochiExpression expr = touchReacting ? touchExpr : currentAction.expression;
    int bounce = (currentAction.doBounce || touchReacting)
                 ? (int)(1.5f * sinf(bouncePhase)) : 0;

    /* Eye geometry — same as the photo: two wide rounded rect eyes */
    const int EYE_W   = 26;
    const int EYE_H   = 24;
    const int EYE_GAP = 8;
    const int EYE_Y   = 10 + bounce;
    const int L_EYE_X = 64 - EYE_GAP/2 - EYE_W; // = 30
    const int R_EYE_X = 64 + EYE_GAP/2;           // = 68

    switch (expr) {

      /* ---- NORMAL ---- */
      case EXPR_NORMAL: {
        int h = EYE_H, y = EYE_Y;
        if (blinking) {
          float ph = (float)(now - blinkStartMs) / 150.0f;
          float k  = 1.0f - sinf(ph * PI);
          h = max(3, (int)(EYE_H * k));
          y = EYE_Y + (EYE_H - h) / 2;
        }
        o.fillRoundRect(L_EYE_X, y, EYE_W, h, 6, SSD1306_WHITE);
        o.fillRoundRect(R_EYE_X, y, EYE_W, h, 6, SSD1306_WHITE);
        if (!blinking) {
          // Gleam — small dark block top-left of each eye
          o.fillRect(L_EYE_X + 4, y + 3, 5, 4, SSD1306_BLACK);
          o.fillRect(R_EYE_X + 4, y + 3, 5, 4, SSD1306_BLACK);
        }
        drawMouth(o, 64, 50 + bounce, MOUTH_SMALL);
        break;
      }

      /* ---- HAPPY ---- */
      case EXPR_HAPPY: {
        drawHappyEyeFill(o, L_EYE_X, R_EYE_X, EYE_Y, EYE_W);
        drawMouth(o, 64, 50 + bounce, MOUTH_BIG);
        break;
      }

      /* ---- EXCITED ---- */
      case EXPR_EXCITED: {
        o.fillRoundRect(L_EYE_X - 1, EYE_Y, EYE_W + 2, EYE_H + 3, 6, SSD1306_WHITE);
        o.fillRoundRect(R_EYE_X - 1, EYE_Y, EYE_W + 2, EYE_H + 3, 6, SSD1306_WHITE);
        // Star gleam inside eyes
        int sx = L_EYE_X + EYE_W/2, sy = EYE_Y + EYE_H/2;
        int sr = (int)(4.0f + 1.5f * sinf(sparklePhase));
        o.drawFastHLine(sx - sr, sy, sr*2+1, SSD1306_BLACK);
        o.drawFastVLine(sx, sy - sr, sr*2+1, SSD1306_BLACK);
        o.drawFastHLine(sx - 2, sy, 5, SSD1306_WHITE);
        o.drawFastVLine(sx, sy - 2, 5, SSD1306_WHITE);
        sx = R_EYE_X + EYE_W/2;
        o.drawFastHLine(sx - sr, sy, sr*2+1, SSD1306_BLACK);
        o.drawFastVLine(sx, sy - sr, sr*2+1, SSD1306_BLACK);
        o.drawFastHLine(sx - 2, sy, 5, SSD1306_WHITE);
        o.drawFastVLine(sx, sy - 2, 5, SSD1306_WHITE);
        drawMouth(o, 64, 51 + bounce, MOUTH_BIG);
        break;
      }

      /* ---- SAD ---- */
      case EXPR_SAD: {
        o.fillRoundRect(L_EYE_X, EYE_Y, EYE_W, EYE_H, 6, SSD1306_WHITE);
        o.fillRoundRect(R_EYE_X, EYE_Y, EYE_W, EYE_H, 6, SSD1306_WHITE);
        int lidH = EYE_H * 2 / 5;
        o.fillRect(L_EYE_X, EYE_Y, EYE_W, lidH, SSD1306_BLACK);
        o.fillRect(R_EYE_X, EYE_Y, EYE_W, lidH, SSD1306_BLACK);
        o.drawFastHLine(L_EYE_X, EYE_Y + lidH, EYE_W, SSD1306_WHITE);
        o.drawFastHLine(R_EYE_X, EYE_Y + lidH, EYE_W, SSD1306_WHITE);
        // Tear drops
        o.fillCircle(L_EYE_X + EYE_W - 4, EYE_Y + EYE_H + 3, 2, SSD1306_WHITE);
        o.fillCircle(R_EYE_X + 3,          EYE_Y + EYE_H + 3, 2, SSD1306_WHITE);
        drawMouth(o, 64, 51 + bounce, MOUTH_SAD);
        break;
      }

      /* ---- SLEEPY ---- */
      case EXPR_SLEEPY: {
        o.fillRoundRect(L_EYE_X, EYE_Y, EYE_W, EYE_H, 6, SSD1306_WHITE);
        o.fillRoundRect(R_EYE_X, EYE_Y, EYE_W, EYE_H, 6, SSD1306_WHITE);
        int lidH = EYE_H / 2 + 3;
        o.fillRect(L_EYE_X, EYE_Y, EYE_W, lidH, SSD1306_BLACK);
        o.fillRect(R_EYE_X, EYE_Y, EYE_W, lidH, SSD1306_BLACK);
        o.drawFastHLine(L_EYE_X, EYE_Y + lidH, EYE_W, SSD1306_WHITE);
        o.drawFastHLine(R_EYE_X, EYE_Y + lidH, EYE_W, SSD1306_WHITE);
        // Yawn O
        fillEllipseSimple(o, 64, 51 + bounce, 5, 4, SSD1306_WHITE);
        fillEllipseSimple(o, 64, 51 + bounce, 3, 2, SSD1306_BLACK);
        break;
      }

      /* ---- SLEEPING ---- */
      case EXPR_SLEEPING: {
        int midY = EYE_Y + EYE_H / 2;
        // Closed arc eyes (peaceful)
        for (int t = 0; t < 3; t++) {
          o.drawLine(L_EYE_X,           midY + 2, L_EYE_X + EYE_W/2, midY - 5 + t, SSD1306_WHITE);
          o.drawLine(L_EYE_X + EYE_W/2, midY - 5 + t, L_EYE_X + EYE_W, midY + 2,   SSD1306_WHITE);
          o.drawLine(R_EYE_X,           midY + 2, R_EYE_X + EYE_W/2, midY - 5 + t, SSD1306_WHITE);
          o.drawLine(R_EYE_X + EYE_W/2, midY - 5 + t, R_EYE_X + EYE_W, midY + 2,   SSD1306_WHITE);
        }
        // Serene smile
        o.drawLine(57, 51, 61, 49, SSD1306_WHITE);
        o.drawLine(61, 49, 67, 51, SSD1306_WHITE);
        // Moon + stars
        o.drawCircle(7, 7, 5, SSD1306_WHITE);
        o.fillCircle(10, 5, 4, SSD1306_BLACK);
        o.drawPixel(18, 3,  SSD1306_WHITE);
        o.drawPixel(22, 8,  SSD1306_WHITE);
        o.drawPixel(26, 2,  SSD1306_WHITE);
        drawZzz(o);
        break;
      }

      /* ---- ANNOYED / BORED ---- */
      case EXPR_ANNOYED:
      case EXPR_BORED: {
        o.fillRoundRect(L_EYE_X, EYE_Y, EYE_W, EYE_H, 6, SSD1306_WHITE);
        o.fillRoundRect(R_EYE_X, EYE_Y, EYE_W, EYE_H, 6, SSD1306_WHITE);
        int lidH = EYE_H * 2 / 5;
        o.fillRect(L_EYE_X, EYE_Y, EYE_W, lidH, SSD1306_BLACK);
        o.fillRect(R_EYE_X, EYE_Y, EYE_W, lidH, SSD1306_BLACK);
        o.drawFastHLine(L_EYE_X, EYE_Y + lidH, EYE_W, SSD1306_WHITE);
        o.drawFastHLine(R_EYE_X, EYE_Y + lidH, EYE_W, SSD1306_WHITE);
        // Flat line mouth
        o.fillRect(56, 51 + bounce, 16, 2, SSD1306_WHITE);
        break;
      }

      /* ---- SHOCKED ---- */
      case EXPR_SHOCKED: {
        o.fillCircle(L_EYE_X + EYE_W/2, EYE_Y + EYE_H/2, 13, SSD1306_WHITE);
        o.fillCircle(R_EYE_X + EYE_W/2, EYE_Y + EYE_H/2, 13, SSD1306_WHITE);
        o.fillCircle(L_EYE_X + EYE_W/2, EYE_Y + EYE_H/2,  6, SSD1306_BLACK);
        o.fillCircle(R_EYE_X + EYE_W/2, EYE_Y + EYE_H/2,  6, SSD1306_BLACK);
        o.fillCircle(L_EYE_X + EYE_W/2 - 3, EYE_Y + EYE_H/2 - 3, 2, SSD1306_WHITE);
        o.fillCircle(R_EYE_X + EYE_W/2 - 3, EYE_Y + EYE_H/2 - 3, 2, SSD1306_WHITE);
        fillEllipseSimple(o, 64, 52 + bounce, 7, 5, SSD1306_WHITE);
        fillEllipseSimple(o, 64, 52 + bounce, 4, 3, SSD1306_BLACK);
        for (int i = -2; i <= 2; i++)
          o.drawFastVLine(64 + i * 9, 1, 6, SSD1306_WHITE);
        break;
      }

      /* ---- LOVE ---- */
      case EXPR_LOVE: {
        drawHeartAt(o, L_EYE_X + EYE_W/2, EYE_Y + EYE_H/2 + 1, 7);
        drawHeartAt(o, R_EYE_X + EYE_W/2, EYE_Y + EYE_H/2 + 1, 7);
        // Floating mini hearts
        int hOff = (int)(3.0f * sinf(sparklePhase));
        drawHeartAt(o, L_EYE_X - 2, EYE_Y - 6 - hOff, 3);
        drawHeartAt(o, R_EYE_X + EYE_W + 2, EYE_Y - 6 - hOff, 3);
        drawMouth(o, 64, 51 + bounce, MOUTH_BIG);
        break;
      }

      /* ---- THINKING ---- */
      case EXPR_THINKING: {
        // Left: normal eye
        o.fillRoundRect(L_EYE_X, EYE_Y, EYE_W, EYE_H, 6, SSD1306_WHITE);
        o.fillRect(L_EYE_X + 4, EYE_Y + 3, 5, 4, SSD1306_BLACK);
        // Right: squinted
        o.fillRoundRect(R_EYE_X, EYE_Y, EYE_W, EYE_H, 6, SSD1306_WHITE);
        o.fillRect(R_EYE_X, EYE_Y, EYE_W, EYE_H * 3/5, SSD1306_BLACK);
        o.drawFastHLine(R_EYE_X, EYE_Y + EYE_H * 3/5, EYE_W, SSD1306_WHITE);
        // Thought dots
        o.fillCircle(96, 6,  2, SSD1306_WHITE);
        o.fillCircle(101, 4, 2, SSD1306_WHITE);
        o.fillCircle(106, 6, 2, SSD1306_WHITE);
        drawMouth(o, 64, 51 + bounce, MOUTH_SMALL);
        break;
      }

      /* ---- PLAYFUL (wink + tongue) ---- */
      case EXPR_PLAYFUL: {
        // Left eye: normal (with blink)
        int h = EYE_H, y = EYE_Y;
        if (blinking) {
          float ph = (float)(now - blinkStartMs) / 150.0f;
          float k  = 1.0f - sinf(ph * PI);
          h = max(3, (int)(EYE_H * k));
          y = EYE_Y + (EYE_H - h) / 2;
        }
        o.fillRoundRect(L_EYE_X, y, EYE_W, h, 6, SSD1306_WHITE);
        o.fillRect(L_EYE_X + 4, y + 3, 5, 4, SSD1306_BLACK);
        // Right eye: wink (closed arc)
        int wY = EYE_Y + EYE_H / 2;
        for (int t = 0; t < 3; t++) {
          o.drawLine(R_EYE_X,           wY + 2, R_EYE_X + EYE_W/2, wY - 4 + t, SSD1306_WHITE);
          o.drawLine(R_EYE_X + EYE_W/2, wY - 4 + t, R_EYE_X + EYE_W, wY + 2,   SSD1306_WHITE);
        }
        // Tongue
        o.fillRoundRect(58, 50 + bounce, 12, 8, 3, SSD1306_WHITE);
        o.drawFastHLine(58, 53 + bounce, 12, SSD1306_BLACK);
        o.drawFastHLine(59, 54 + bounce,  5, SSD1306_BLACK);
        break;
      }

      /* ---- ANGRY (woken from sleep) ---- */
      case EXPR_ANGRY: {
        int cx1 = L_EYE_X + EYE_W/2, cx2 = R_EYE_X + EYE_W/2;
        int cy  = EYE_Y + EYE_H/2;
        o.fillCircle(cx1, cy, 12, SSD1306_WHITE);
        o.fillCircle(cx2, cy, 12, SSD1306_WHITE);
        o.fillCircle(cx1, cy,  5, SSD1306_BLACK);
        o.fillCircle(cx2, cy,  5, SSD1306_BLACK);
        // Angry V-brows
        o.drawLine(L_EYE_X,       EYE_Y - 4, L_EYE_X + EYE_W, EYE_Y - 1, SSD1306_WHITE);
        o.drawLine(L_EYE_X,       EYE_Y - 3, L_EYE_X + EYE_W, EYE_Y,     SSD1306_WHITE);
        o.drawLine(R_EYE_X,       EYE_Y - 1, R_EYE_X + EYE_W, EYE_Y - 4, SSD1306_WHITE);
        o.drawLine(R_EYE_X,       EYE_Y,     R_EYE_X + EYE_W, EYE_Y - 3, SSD1306_WHITE);
        // Screaming mouth
        o.fillRoundRect(54, 49 + bounce, 20, 11, 3, SSD1306_WHITE);
        o.fillRoundRect(57, 51 + bounce, 14,  7, 2, SSD1306_BLACK);
        // "!"
        o.setTextSize(2); o.setCursor(110, 1); o.print("!"); o.setTextSize(1);
        break;
      }

      default: {
        // Fallback = NORMAL
        o.fillRoundRect(L_EYE_X, EYE_Y, EYE_W, EYE_H, 6, SSD1306_WHITE);
        o.fillRoundRect(R_EYE_X, EYE_Y, EYE_W, EYE_H, 6, SSD1306_WHITE);
        o.fillRect(L_EYE_X + 4, EYE_Y + 3, 5, 4, SSD1306_BLACK);
        o.fillRect(R_EYE_X + 4, EYE_Y + 3, 5, 4, SSD1306_BLACK);
        drawMouth(o, 64, 50 + bounce, MOUTH_SMALL);
        break;
      }
    }

    /* Props */
    if (!touchReacting && currentAction.showPhone &&
        expr != EXPR_SLEEPING && expr != EXPR_ANGRY) {
      drawPhone(o, 100, 22 + bounce);
    }
    if (!touchReacting && currentAction.showCoffee &&
        expr != EXPR_SLEEPING && expr != EXPR_ANGRY) {
      drawCoffeeCup(o, 101, 32 + bounce, coffeeSip);
    }

    o.display();
  }
};

/* ============================ WatchApp ============================== */
class WatchApp {
  DisplayManager* dm = nullptr;
  TimeManager*    tm = nullptr;
  BatteryManager* bm = nullptr;
  int lastSecond = -1;
public:
  void begin(DisplayManager* d, TimeManager* t, BatteryManager* b) {
    dm = d; tm = t; bm = b;
  }
  void onEnter() { lastSecond = -1; }

  void update() {
    struct tm t;
    tm->getLocal(t);
    if (t.tm_sec == lastSecond) return;
    lastSecond = t.tm_sec;
    draw(t);
  }

  void draw(struct tm& t) {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    char buf[24];

    int pct = bm->getPercent();
    int bx = 106, by = 2;
    o.drawRect(bx, by, 18, 8, SSD1306_WHITE);
    o.fillRect(bx + 18, by + 2, 2, 4, SSD1306_WHITE);
    int fillW = map(pct, 0, 100, 0, 14);
    if (fillW > 0) o.fillRect(bx + 2, by + 2, fillW, 4, SSD1306_WHITE);
    snprintf(buf, sizeof(buf), "%d%%", pct);
    o.setTextSize(1);
    o.setCursor(bx - (int)(6 * strlen(buf)) - 3, by + 1);
    o.print(buf);

    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    dm->centerText(buf, 20, 3);

    snprintf(buf, sizeof(buf), ":%02d", t.tm_sec);
    o.setTextSize(1);
    o.setCursor(110, 38);
    o.print(buf);

    strftime(buf, sizeof(buf), "%d %b %Y", &t);
    dm->centerText(buf, 52, 1);

    o.display();
  }
};

/* ============================ FlappyApp ============================= */
class FlappyApp {
  DisplayManager* dm = nullptr;
  Preferences     prefs;
public:
  enum GState { G_START, G_PLAYING, G_OVER };
  GState gstate = G_START;

private:
  float birdY = 30.0f, birdV = 0.0f;
  static constexpr int   BIRD_X = 22;
  static constexpr int   BIRD_R = 4;
  static constexpr float GRAVITY    = 0.15f;
  static constexpr float FLAP_V     = -2.0f;
  static constexpr float PIPE_SPEED = 1.2f;
  static constexpr int   NPIPES     = 2;
  static constexpr int   PIPE_W     = 12;
  static constexpr int   GAP_H      = 44;
  static constexpr int   PIPE_SPACING = 82;

  float pipeX[NPIPES];
  int   gapY[NPIPES];
  bool  passed[NPIPES];

  int  score = 0, best = 0;
  bool dirty = true;
  unsigned long lastFrameMs  = 0;
  unsigned long gameStartMs  = 0;
  static constexpr unsigned long PIPE_DELAY_MS = 2200;

  float wingPhase = 0.0f;

public:
  void begin(DisplayManager* d) {
    dm = d;
    prefs.begin("flappy", false);
    best = prefs.getInt("best", 0);
  }
  void onEnter() { gstate = G_START; dirty = true; }
  bool isPlaying()      const { return gstate == G_PLAYING; }
  bool isOnStartScreen()const { return gstate == G_START;   }

  void onSingleTap() {
    if      (gstate == G_START)   startGame();
    else if (gstate == G_PLAYING) { birdV = FLAP_V; wingPhase = 0.0f; }
    else if (gstate == G_OVER)    startGame();
  }

  /* Returns true only from start screen — caller uses this to switch mode */
  bool onDoubleTap() { return (gstate == G_START); }

  void startGame() {
    birdY = 30.0f; birdV = 0.0f; score = 0; wingPhase = 0.0f;
    gameStartMs = millis();
    for (int i = 0; i < NPIPES; i++) {
      pipeX[i]  = 128.0f + 60.0f + i * PIPE_SPACING;
      gapY[i]   = random(10, 64 - 10 - GAP_H);
      passed[i] = false;
    }
    gstate = G_PLAYING; dirty = false;
  }

  void update() {
    unsigned long now = millis();
    if (now - lastFrameMs < 33) return;
    lastFrameMs = now;

    if (gstate == G_START) { if (dirty) { drawStart(); dirty = false; } return; }
    if (gstate == G_OVER)  { if (dirty) { drawGameOver(); dirty = false; } return; }

    birdV += GRAVITY;
    birdY += birdV;
    wingPhase += 0.2f;
    if (wingPhase > TWO_PI) wingPhase -= TWO_PI;

    bool pipesActive = (now - gameStartMs > PIPE_DELAY_MS);
    if (pipesActive) {
      for (int i = 0; i < NPIPES; i++) {
        pipeX[i] -= PIPE_SPEED;
        if (pipeX[i] + PIPE_W < 0) {
          float maxX = pipeX[0];
          for (int j = 1; j < NPIPES; j++) if (pipeX[j] > maxX) maxX = pipeX[j];
          pipeX[i]  = maxX + PIPE_SPACING;
          gapY[i]   = random(10, 64 - 10 - GAP_H);
          passed[i] = false;
        }
        if (!passed[i] && pipeX[i] + PIPE_W < BIRD_X - BIRD_R) {
          passed[i] = true; score++;
        }
      }
    }

    bool dead = (birdY - BIRD_R <= 1) || (birdY + BIRD_R >= 61);
    if (pipesActive) {
      for (int i = 0; i < NPIPES && !dead; i++) {
        bool inX  = (BIRD_X + BIRD_R > (int)pipeX[i]) &&
                    (BIRD_X - BIRD_R < (int)(pipeX[i] + PIPE_W));
        bool inGap = (birdY - BIRD_R >= gapY[i]) &&
                     (birdY + BIRD_R <= gapY[i] + GAP_H);
        if (inX && !inGap) dead = true;
      }
    }

    if (dead) {
      if (score > best) { best = score; prefs.putInt("best", best); }
      gstate = G_OVER; dirty = true; return;
    }
    drawGame(now);
  }

private:
  void drawPipe(Adafruit_SH1106G& o, int px, int topH, int botY) {
    if (topH > 4) {
      o.fillRect(px + 1, 0, PIPE_W - 2, topH - 4, SSD1306_WHITE);
      o.drawFastVLine(px + 3, 0, topH - 4, SSD1306_BLACK);
    }
    o.fillRect(px - 1, topH - 4, PIPE_W + 2, 4, SSD1306_WHITE);
    o.fillRect(px - 1, botY, PIPE_W + 2, 4, SSD1306_WHITE);
    int bodyH = 63 - (botY + 4);
    if (bodyH > 0) {
      o.fillRect(px + 1, botY + 4, PIPE_W - 2, bodyH, SSD1306_WHITE);
      o.drawFastVLine(px + 3, botY + 4, bodyH, SSD1306_BLACK);
    }
  }

  void drawBird(Adafruit_SH1106G& o, int bx, int by) {
    o.fillCircle(bx, by, BIRD_R, SSD1306_WHITE);
    int wingOff = (int)(2.0f * sinf(wingPhase));
    o.fillRoundRect(bx - BIRD_R - 2, by - 1 + wingOff, 5, 3, 1, SSD1306_WHITE);
    o.fillTriangle(bx + BIRD_R - 1, by - 1,
                   bx + BIRD_R + 3, by,
                   bx + BIRD_R - 1, by + 2, SSD1306_WHITE);
    o.drawPixel(bx + 2, by - 2, SSD1306_BLACK);
  }

  void drawStart() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    o.fillRoundRect(10, 4, 108, 16, 4, SSD1306_WHITE);
    o.setTextColor(SSD1306_BLACK);
    dm->centerText("FLAPPY MOCHI", 8, 1);
    o.setTextColor(SSD1306_WHITE);
    o.fillRect(0, 0, 5, 18, SSD1306_WHITE);
    o.fillRect(123, 0, 5, 18, SSD1306_WHITE);
    o.fillRect(0, 48, 5, 16, SSD1306_WHITE);
    o.fillRect(123, 48, 5, 16, SSD1306_WHITE);
    drawBird(o, 64, 36);
    o.drawFastHLine(0, 62, 128, SSD1306_WHITE);
    o.drawFastHLine(0, 63, 128, SSD1306_WHITE);
    dm->centerText("Tap  - Start", 44, 1);
    dm->centerText("2xTap- Switch", 53, 1);
    o.display();
  }

  void drawGame(unsigned long now) {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    bool pipesActive = (now - gameStartMs > PIPE_DELAY_MS);
    if (pipesActive) {
      for (int i = 0; i < NPIPES; i++) {
        int px = (int)pipeX[i];
        if (px < 128 && px + PIPE_W > 0)
          drawPipe(o, px, gapY[i], gapY[i] + GAP_H);
      }
    }
    o.drawFastHLine(0, 62, 128, SSD1306_WHITE);
    o.drawFastHLine(0, 63, 128, SSD1306_WHITE);
    for (int x = 0; x < 128; x += 6) o.drawPixel(x, 61, SSD1306_WHITE);
    drawBird(o, BIRD_X, (int)birdY);
    char buf[8]; snprintf(buf, sizeof(buf), "%d", score);
    int sw = (int)(strlen(buf) * 6 + 8);
    o.drawRoundRect((128 - sw) / 2, 1, sw, 10, 2, SSD1306_WHITE);
    dm->centerText(buf, 3, 1);
    if (!pipesActive) dm->centerText("Get Ready!", 26, 1);
    o.display();
  }

  void drawGameOver() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    o.drawRoundRect(4, 2, 120, 60, 5, SSD1306_WHITE);
    o.drawRoundRect(6, 4, 116, 56, 3, SSD1306_WHITE);
    o.fillRoundRect(10, 6, 108, 14, 3, SSD1306_WHITE);
    o.setTextColor(SSD1306_BLACK);
    dm->centerText("GAME OVER", 10, 1);
    o.setTextColor(SSD1306_WHITE);
    char buf[20];
    snprintf(buf, sizeof(buf), "Score: %d", score);
    dm->centerText(buf, 26, 1);
    snprintf(buf, sizeof(buf), "Best:  %d", best);
    dm->centerText(buf, 38, 1);
    o.drawFastHLine(20, 35, 88, SSD1306_WHITE);
    dm->centerText("Tap to retry", 50, 1);
    o.display();
  }
};

/* ============================ UpdateApp ============================= */
class UpdateApp {
  DisplayManager* dm = nullptr;
  enum OtaState {
    OTA_IDLE, OTA_CONNECTING, OTA_CHECKING,
    OTA_DOWNLOADING, OTA_FAIL, OTA_UP_TO_DATE
  };
  OtaState state    = OTA_IDLE;
  unsigned long statusTimer = 0;
  String errorMsg   = "";

public:
  void begin(DisplayManager* d) { dm = d; }

  void onEnter() {
    state       = OTA_CONNECTING;
    statusTimer = millis();
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  void update() {
    Adafruit_SH1106G& o = dm->oled;
    unsigned long now   = millis();
    switch (state) {
      case OTA_CONNECTING:
        o.clearDisplay();
        dm->centerText("OTA UPDATE", 5, 1);
        dm->centerText("Connecting Wi-Fi", 22, 1);
        o.drawRect(14, 38, 100, 6, SSD1306_WHITE);
        { int dots = (now / 400) % 4;
          char dBuf[5] = "    ";
          for (int i = 0; i < dots; i++) dBuf[i] = '.';
          dm->centerText(dBuf, 50, 1); }
        o.display();
        if (WiFi.status() == WL_CONNECTED) state = OTA_CHECKING;
        else if (now - statusTimer > 20000) fail("Wi-Fi Timeout");
        break;

      case OTA_CHECKING:
        o.clearDisplay();
        dm->centerText("OTA UPDATE", 5, 1);
        dm->centerText("Checking GitHub...", 28, 1);
        o.display();
        runCheck();
        break;

      case OTA_DOWNLOADING: break;

      case OTA_FAIL:
        o.clearDisplay();
        dm->centerText("UPDATE FAILED", 5, 1);
        o.drawFastHLine(10, 18, 108, SSD1306_WHITE);
        dm->centerText(errorMsg.c_str(), 26, 1);
        dm->centerText("Dbl Tap to Exit", 50, 1);
        o.display();
        break;

      case OTA_UP_TO_DATE:
        o.clearDisplay();
        dm->centerText("SYSTEM OK", 8, 1);
        o.drawFastHLine(10, 20, 108, SSD1306_WHITE);
        dm->centerText("No New Version", 28, 1);
        dm->centerText("Dbl Tap to Exit", 50, 1);
        o.display();
        break;

      default: break;
    }
  }

private:
  void fail(String msg) {
    errorMsg = msg; state = OTA_FAIL;
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  }

  void runCheck() {
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http;
    if (http.begin(client, VERSION_URL)) {
      int code = http.GET();
      if (code == HTTP_CODE_OK) {
        String payload = http.getString(); payload.trim();
        if (payload.toInt() > CURRENT_VERSION) {
          state = OTA_DOWNLOADING; executeUpdate(client);
        } else {
          state = OTA_UP_TO_DATE;
          WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
        }
      } else { fail("HTTP Err: " + String(code)); }
      http.end();
    } else { fail("Connection Fail"); }
  }

  void executeUpdate(WiFiClientSecure& client) {
    HTTPClient http;
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay(); dm->centerText("DOWNLOADING...", 25, 1); o.display();
    if (http.begin(client, BIN_URL)) {
      int code = http.GET();
      if (code == HTTP_CODE_OK) {
        int len = http.getSize();
        if (Update.begin(len)) {
          WiFiClient* stream = http.getStreamPtr();
          size_t written = 0; uint8_t buf[256];
          while (http.connected() && written < (size_t)len) {
            size_t av = stream->available();
            if (av) {
              int c = stream->readBytes(buf, min(av, sizeof(buf)));
              Update.write(buf, c); written += c;
              o.clearDisplay(); dm->centerText("FLASHING UPDATE", 5, 1);
              o.drawRect(14, 26, 100, 12, SSD1306_WHITE);
              int pw = (int)map((long)written, 0L, (long)len, 0L, 96L);
              if (pw > 0) o.fillRect(16, 28, pw, 8, SSD1306_WHITE);
              char pb[8]; snprintf(pb, sizeof(pb), "%d%%", (int)(written*100/len));
              dm->centerText(pb, 44, 1); o.display();
            }
          }
          if (Update.end() && Update.isFinished()) {
            o.clearDisplay(); dm->centerText("SUCCESS!", 10, 1);
            dm->centerText("Rebooting...", 35, 1); o.display();
            delay(2000); ESP.restart();
          } else { fail("Flash Error"); }
        } else { fail("No Space"); }
      } else { fail("Bin HTTP: " + String(code)); }
      http.end();
    } else { fail("Bin Link Fail"); }
  }
};

/* ========================== AI Manager ============================
 * Polls Gemini 1.5 Flash every 60 s (skip during sleep window).
 * Uses response_mime_type=application/json to get clean JSON.
 * Parses with TinyJSON — zero external library dependency.
 * ================================================================ */
class AIManager {
  enum AIState { AI_IDLE, AI_WORKING };
  AIState       state        = AI_IDLE;
  MochiApp*     mochiRef     = nullptr;
  TimeManager*  timeRef      = nullptr;

public:
  void begin(MochiApp* m, TimeManager* t) { mochiRef = m; timeRef = t; }

  void update() {
    if (state == AI_IDLE && mochiRef->shouldPollAI()) {
      state = AI_WORKING;
      doRequest();
      state = AI_IDLE;
    }
  }

private:
  void doRequest() {
    Serial.println("[AI] Starting request...");

    // Connect WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 12000) {
      delay(200);
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[AI] WiFi failed");
      WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
      mochiRef->markAIPollDone();
      return;
    }
    Serial.println("[AI] WiFi OK");

    // Build time context
    struct tm t;
    timeRef->getLocal(t);
    char timeBuf[16];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M", &t);

    // Build JSON request body manually — no library needed
    // Using response_mime_type forces Gemini to output only JSON
    String reqBody = "{\"contents\":[{\"parts\":[{\"text\":\"";
    reqBody += "You are Mochi, an adorable small robot pet on an OLED screen. ";
    reqBody += "Current time: ";
    reqBody += timeBuf;
    reqBody += ". Choose what to do for the next minute. ";
    reqBody += "Reply with ONLY a JSON object with these keys: ";
    reqBody += "expression (one of: normal happy sad excited sleepy sleeping annoyed bored shocked love thinking playful angry), ";
    reqBody += "showPhone (boolean), showCoffee (boolean), doLookAround (boolean), ";
    reqBody += "doBounce (boolean), durationSeconds (integer 5-55). ";
    reqBody += "Be creative and natural!\"}]}],";
    reqBody += "\"generationConfig\":{";
    reqBody += "\"response_mime_type\":\"application/json\",";
    reqBody += "\"temperature\":1.1,";
    reqBody += "\"maxOutputTokens\":80}}";

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(12);

    HTTPClient http;
    String url = String("https://") + GEMINI_HOST +
                 "/v1beta/models/gemini-1.5-flash:generateContent?key=" +
                 GEMINI_API_KEY;

    if (!http.begin(client, url)) {
      Serial.println("[AI] http.begin failed");
      WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
      mochiRef->markAIPollDone();
      return;
    }

    http.addHeader("Content-Type", "application/json");
    http.setTimeout(10000);
    int code = http.POST(reqBody);
    Serial.printf("[AI] HTTP %d\n", code);

    if (code == HTTP_CODE_OK) {
      String resp = http.getString();
      Serial.println("[AI] Raw: " + resp.substring(0, 200));
      parseAndApply(resp);
    } else {
      Serial.println("[AI] Error body: " + http.getString().substring(0, 100));
    }

    http.end();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    mochiRef->markAIPollDone();
    Serial.println("[AI] Done.");
  }

  void parseAndApply(const String& rawResp) {
    // Step 1: extract the text field from Gemini's response wrapper
    String innerJson = TinyJSON::extractGeminiText(rawResp);
    if (innerJson.length() == 0) {
      Serial.println("[AI] Could not extract text field");
      return;
    }
    innerJson.trim();
    Serial.println("[AI] Inner JSON: " + innerJson);

    // Step 2: parse the action JSON
    String exprStr   = TinyJSON::extractString(innerJson, "expression");
    bool  showPhone  = TinyJSON::extractBool(innerJson, "showPhone",   false);
    bool  showCoffee = TinyJSON::extractBool(innerJson, "showCoffee",  false);
    bool  lookAround = TinyJSON::extractBool(innerJson, "doLookAround",false);
    bool  doBounce   = TinyJSON::extractBool(innerJson, "doBounce",    true);
    int   durSec     = TinyJSON::extractInt (innerJson, "durationSeconds", 30);

    if (exprStr.length() == 0) exprStr = "normal";
    durSec = constrain(durSec, 5, 55);

    Serial.printf("[AI] expr=%s phone=%d coffee=%d look=%d bounce=%d dur=%ds\n",
                  exprStr.c_str(), showPhone, showCoffee, lookAround, doBounce, durSec);

    mochiRef->applyAIAction(
      mochiRef->expressionFromString(exprStr),
      showPhone, showCoffee, lookAround, doBounce,
      (unsigned long)durSec * 1000UL
    );
  }
};

/* ============================ Globals =============================== */
DisplayManager displayMgr;
TouchManager   touchMgr;
BatteryManager batteryMgr;
TimeManager    timeMgr;
MochiApp       mochiApp;
WatchApp       watchApp;
FlappyApp      flappyApp;
UpdateApp      updateApp;
AIManager      aiMgr;

AppMode       mode              = MODE_MOCHI;
unsigned long lastInteractionMs = 0;

void enterMode(AppMode m) {
  mode = m;
  switch (mode) {
    case MODE_MOCHI:  mochiApp.onEnter();  break;
    case MODE_WATCH:  watchApp.onEnter();  break;
    case MODE_FLAPPY: flappyApp.onEnter(); break;
    case MODE_UPDATE: updateApp.onEnter(); break;
  }
}

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  if (!displayMgr.begin())
    Serial.println("SH1106 init failed - check wiring");

  touchMgr.begin();
  batteryMgr.begin();
  timeMgr.begin();

  mochiApp.begin(&displayMgr, &timeMgr);
  watchApp.begin(&displayMgr, &timeMgr, &batteryMgr);
  flappyApp.begin(&displayMgr);
  updateApp.begin(&displayMgr);
  aiMgr.begin(&mochiApp, &timeMgr);

  lastInteractionMs = millis();
  enterMode(MODE_MOCHI);
}

void loop() {
  unsigned long now = millis();

  timeMgr.update();
  batteryMgr.update();

  TapEvent ev = touchMgr.update();
  if (ev != TAP_NONE) lastInteractionMs = now;

  /* ---- Touch routing ---- */
  if (ev == TAP_DOUBLE) {
    if (mode == MODE_FLAPPY) {
      // Double tap only switches mode from start screen
      if (flappyApp.onDoubleTap()) {
        enterMode(MODE_UPDATE);
      }
      // While playing: double tap is ignored
    } else {
      enterMode(mode == MODE_MOCHI  ? MODE_WATCH  :
                mode == MODE_WATCH  ? MODE_FLAPPY :
                mode == MODE_FLAPPY ? MODE_UPDATE : MODE_MOCHI);
    }
  } else if (ev == TAP_SINGLE) {
    switch (mode) {
      case MODE_MOCHI:  mochiApp.onSingleTap();  break;
      case MODE_FLAPPY: flappyApp.onSingleTap(); break;
      default: break;
    }
  }

  /* Prevent idle timeout during active game or OTA */
  if (mode == MODE_FLAPPY && flappyApp.isPlaying()) lastInteractionMs = now;
  if (mode == MODE_UPDATE)                          lastInteractionMs = now;

  if (mode != MODE_MOCHI && (now - lastInteractionMs) >= IDLE_TIMEOUT) {
    lastInteractionMs = now;
    enterMode(MODE_MOCHI);
  }

  /* AI polling — only on Mochi screen */
  if (mode == MODE_MOCHI) aiMgr.update();

  /* Active mode tick */
  switch (mode) {
    case MODE_MOCHI:  mochiApp.update();  break;
    case MODE_WATCH:  watchApp.update();  break;
    case MODE_FLAPPY: flappyApp.update(); break;
    case MODE_UPDATE: updateApp.update(); break;
  }
}

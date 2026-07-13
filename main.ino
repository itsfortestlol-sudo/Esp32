/***********************************************************************
 * MochiPod - ESP32 Handheld (Mochi Gen2 / Watch / Flappy Bird / OTA)
 * Board  : ESP32 DevKit V1 (ESP-WROOM-32)
 * Display: SH1106 128x64 I2C
 * Touch  : TTP223 (GPIO 4, active HIGH)
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
#include <ArduinoJson.h>
#include "secrets.h"

#define SSD1306_WHITE SH110X_WHITE
#define SSD1306_BLACK SH110X_BLACK

/* ============================ CONFIG ================================ */
const long GMT_OFFSET_SEC  = 21600;
const int  DST_OFFSET_SEC  = 0;
const char* NTP_SERVER     = "asia.pool.ntp.org";

const int   CURRENT_VERSION = 3;
const char* VERSION_URL     = "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/virsion.txt";
const char* BIN_URL         = "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/firmware.bin";
const char* GEMINI_API_KEY  = GEMINI_KEY; // defined in secrets.h
const char* GEMINI_HOST     = "generativelanguage.googleapis.com";

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
const unsigned long TOUCH_DEBOUNCE_MS   = 30;
const unsigned long DOUBLE_TAP_WINDOW_MS = 300;

/* ------------------------------ Idle ------------------------------- */
const unsigned long IDLE_TIMEOUT = 20000;

/* ========================= Sleep window ============================ */
const int SLEEP_HOUR_START = 22;
const int SLEEP_HOUR_END   = 8;

/* ========================= AI Timing =============================== */
const unsigned long AI_POLL_INTERVAL = 60000; // 1 minute

/* =========================== App Modes ============================= */
enum AppMode  { MODE_MOCHI, MODE_WATCH, MODE_FLAPPY, MODE_UPDATE };
enum TapEvent { TAP_NONE, TAP_SINGLE, TAP_DOUBLE };

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
  bool stableState    = false;
  bool lastRead       = false;
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
          firstTapMs = now;
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
  void update() {
    if (millis() - lastReadMs >= 5000) sample();
  }
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
  unsigned long startMs     = 0;
  unsigned long lastRetryMs = 0;
  bool synced      = false;
public:
  void begin() { startSync(); }

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
    struct tm t;
    getLocal(t);
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

  enum MochiExpression {
    EXPR_NORMAL, EXPR_HAPPY, EXPR_SAD,    EXPR_EXCITED,
    EXPR_SLEEPY,  EXPR_SLEEPING, EXPR_ANNOYED, EXPR_BORED,
    EXPR_SHOCKED, EXPR_LOVE,  EXPR_THINKING, EXPR_PLAYFUL,
    EXPR_ANGRY
  };

  struct MochiAction {
    MochiExpression expression = EXPR_NORMAL;
    bool showPhone    = false;
    bool showCoffee   = false;
    bool doLookAround = false;
    bool doBounce     = false;
    unsigned long durationMs = 5000;
  };

  MochiAction currentAction;
  MochiAction pendingAction;
  bool hasPendingAction = false;

  bool touchReacting    = false;
  unsigned long touchReactEndMs = 0;
  MochiExpression touchExpr = EXPR_HAPPY;

  unsigned long lastAIPollMs  = 0;
  unsigned long actionStartMs = 0;
  bool aiEnabled = false;

  float bouncePhase = 0.0f;
  float lookPhase   = 0.0f;
  bool  lookingRight = true;

  bool blinking      = false;
  unsigned long blinkStartMs = 0;
  unsigned long nextBlinkMs  = 0;

  struct ZzzParticle {
    float x, y, vy;
    int   size;
    bool  active;
    float alpha;
  };
  static const int ZZZ_MAX = 4;
  ZzzParticle zzz[ZZZ_MAX];
  unsigned long nextZzzMs = 0;

  float coffeeSip = 0.0f;
  bool  sipping   = false;
  unsigned long sipEndMs = 0;

  int  phoneScrollY  = 0;
  unsigned long nextScrollMs = 0;

  float sparklePhase = 0.0f;

  unsigned long lastFrameMs = 0;

  /* ---- Mouth type enum ---- */
  enum MouthType { MOUTH_SMILE_SMALL, MOUTH_SMILE_BIG, MOUTH_SAD };

public:
  void begin(DisplayManager* d, TimeManager* t) {
    dm = d; tm = t;
    initZzz();
    nextBlinkMs = millis() + random(2000, 4000);
    actionStartMs = millis();
    currentAction.expression = EXPR_NORMAL;
    currentAction.durationMs = 8000;
    currentAction.doBounce   = true;
  }

  void onEnter() {
    touchReacting = false;
    currentAction.expression = EXPR_NORMAL;
    currentAction.doBounce   = true;
    currentAction.durationMs = 8000;
    actionStartMs = millis();
  }

  void onSingleTap() {
    unsigned long now = millis();
    if (currentAction.expression == EXPR_SLEEPING) {
      touchExpr = EXPR_ANGRY;
    } else {
      int r = random(0, 4);
      if      (r == 0) touchExpr = EXPR_HAPPY;
      else if (r == 1) touchExpr = EXPR_LOVE;
      else if (r == 2) touchExpr = EXPR_PLAYFUL;
      else             touchExpr = EXPR_EXCITED;
    }
    touchReacting    = true;
    touchReactEndMs  = now + (currentAction.expression == EXPR_SLEEPING ? 3000 : 1500);
  }

  void applyAIAction(MochiExpression expr, bool phone, bool coffee,
                     bool lookAround, bool bounce, unsigned long durMs) {
    if (tm->isSleepTime() && expr != EXPR_SLEEPING) return;
    currentAction.expression  = expr;
    currentAction.showPhone   = phone;
    currentAction.showCoffee  = coffee;
    currentAction.doLookAround= lookAround;
    currentAction.doBounce    = bounce;
    currentAction.durationMs  = durMs;
    actionStartMs  = millis();
    coffeeSip      = 0.0f;
    sipping        = false;
    sipEndMs       = millis() + 1500;
    phoneScrollY   = 0;
    nextScrollMs   = millis() + 600;
    initZzz();
    nextZzzMs = millis();
  }

  /* Public so AIManager can call it */
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

  bool shouldPollAI() {
    if (tm->isSleepTime()) return false;
    return (millis() - lastAIPollMs >= AI_POLL_INTERVAL);
  }

  void markAIPollDone() { lastAIPollMs = millis(); }

  /* Allow AIManager to call applyAIAction via string */
  void applyAIActionFromStrings(const String& exprStr, bool phone, bool coffee,
                                bool lookAround, bool bounce, unsigned long durMs) {
    applyAIAction(expressionFromString(exprStr), phone, coffee,
                  lookAround, bounce, durMs);
  }

  void update() {
    unsigned long now = millis();
    if (now - lastFrameMs < 33) return;
    lastFrameMs = now;

    /* ---- Sleep override ---- */
    if (tm->isSleepTime()) {
      if (currentAction.expression != EXPR_SLEEPING && !touchReacting) {
        currentAction.expression  = EXPR_SLEEPING;
        currentAction.showPhone   = false;
        currentAction.showCoffee  = false;
        currentAction.doBounce    = false;
        currentAction.doLookAround= false;
        actionStartMs = now;
        initZzz();
        nextZzzMs = now;
      }
    } else {
      if (currentAction.expression == EXPR_SLEEPING && !tm->isSleepTime()) {
        currentAction.expression = EXPR_HAPPY;
        currentAction.doBounce   = true;
        currentAction.durationMs = 5000;
        actionStartMs = now;
      }
    }

    /* ---- Touch react expiry ---- */
    if (touchReacting && now >= touchReactEndMs) {
      touchReacting = false;
      if (touchExpr == EXPR_ANGRY) {
        currentAction.expression = EXPR_SLEEPING;
      }
    }

    /* ---- Blink logic ---- */
    MochiExpression displayExpr = touchReacting ? touchExpr : currentAction.expression;
    bool canBlink = (displayExpr != EXPR_SLEEPING  &&
                     displayExpr != EXPR_SHOCKED   &&
                     displayExpr != EXPR_ANGRY     &&
                     displayExpr != EXPR_LOVE);
    if (canBlink) {
      if (!blinking && now >= nextBlinkMs) {
        blinking = true; blinkStartMs = now;
      }
      if (blinking && now - blinkStartMs > 150) {
        blinking    = false;
        nextBlinkMs = now + random(2500, 5000);
      }
    } else {
      blinking = false;
    }

    /* ---- Bounce ---- */
    if (currentAction.doBounce || touchReacting) {
      bouncePhase += 0.04f;
      if (bouncePhase > TWO_PI) bouncePhase -= TWO_PI;
    }

    /* ---- Look around ---- */
    if (currentAction.doLookAround) {
      lookPhase += 0.02f;
      if (lookPhase > TWO_PI) lookPhase -= TWO_PI;
    } else {
      lookPhase = 0.0f;
    }

    /* ---- Sparkle ---- */
    sparklePhase += 0.08f;
    if (sparklePhase > TWO_PI) sparklePhase -= TWO_PI;

    /* ---- Coffee sip ---- */
    if (currentAction.showCoffee && !touchReacting) {
      if (!sipping && now >= sipEndMs) {
        sipping  = true;
        sipEndMs = now + 700;
      } else if (sipping && now >= sipEndMs) {
        sipping    = false;
        sipEndMs   = now + random(1500, 3000);
        coffeeSip  = 0.0f;
      }
      if (sipping) {
        float prog = 1.0f - (float)(sipEndMs - now) / 700.0f;
        coffeeSip = sinf(prog * PI);
      }
    }

    /* ---- Phone scroll ---- */
    if (currentAction.showPhone && !touchReacting && now >= nextScrollMs) {
      phoneScrollY  = random(0, 8);
      nextScrollMs  = now + random(500, 1200);
    }

    /* ---- Zzz particles ---- */
    if (displayExpr == EXPR_SLEEPING) {
      updateZzz(now);
    }

    draw(now);
  }

private:
  /* ------------------------------------------------------------------ */
  void initZzz() {
    for (int i = 0; i < ZZZ_MAX; i++) {
      zzz[i].active = false;
      zzz[i].x     = 85.0f;
      zzz[i].y     = 18.0f;
      zzz[i].vy    = -0.35f;
      zzz[i].size  = 1;
      zzz[i].alpha = 1.0f;
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
        zzz[i].alpha = 1.0f;
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
      zzz[i].alpha -= 0.008f;
      if (zzz[i].y < 2 || zzz[i].x > 126 || zzz[i].alpha <= 0)
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

  /* ---- Phone prop ---- */
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

  /* ---- Coffee cup prop ---- */
  void drawCoffeeCup(Adafruit_SH1106G& o, int cx, int cy, float sip) {
    int tx = (int)(sip * 4.0f);
    int ty = (int)(sip * 2.0f);
    o.drawLine(cx,          cy + 4 - ty, cx + 1,       cy + 12,       SSD1306_WHITE);
    o.drawLine(cx + 11 - tx, cy + 4 - ty, cx + 9,       cy + 12,       SSD1306_WHITE);
    o.drawFastHLine(cx + 1,  cy + 12,     8,                            SSD1306_WHITE);
    o.drawFastHLine(cx,      cy + 4 - ty, 11 - tx,                      SSD1306_WHITE);
    o.drawLine(cx + 11 - tx, cy + 6 - ty, cx + 13 - tx, cy + 6 - ty,   SSD1306_WHITE);
    o.drawLine(cx + 13 - tx, cy + 6 - ty, cx + 13 - tx, cy + 10,       SSD1306_WHITE);
    o.drawLine(cx + 13 - tx, cy + 10,    cx + 9,        cy + 10,       SSD1306_WHITE);
    if (sip < 0.2f) {
      o.drawPixel(cx + 3, cy + 2, SSD1306_WHITE);
      o.drawPixel(cx + 6, cy + 1, SSD1306_WHITE);
      o.drawPixel(cx + 9, cy + 2, SSD1306_WHITE);
    }
  }

  /* ---- Ellipse helpers ---- */
  void drawEllipseHelper(Adafruit_SH1106G& o, int cx, int cy,
                         int rx, int ry, uint16_t color) {
    for (int dy = -ry; dy <= ry; dy++) {
      float ratio = 1.0f - (float)(dy * dy) / (float)(ry * ry);
      if (ratio < 0) continue;
      int xw = (int)(rx * sqrtf(ratio));
      o.drawFastHLine(cx - xw, cy + dy, xw * 2 + 1, color);
    }
  }

  /* ---- Sparkle ---- */
  void drawSparkle(Adafruit_SH1106G& o, int cx, int cy, unsigned long /*now*/) {
    int r1 = (int)(6.0f + 2.0f * sinf(sparklePhase));
    int r2 = (int)(3.0f + 1.0f * cosf(sparklePhase));
    o.drawFastHLine(cx - r1, cy,      r1 * 2 + 1, SSD1306_BLACK);
    o.drawFastVLine(cx,      cy - r1, r1 * 2 + 1, SSD1306_BLACK);
    o.drawFastHLine(cx - r2, cy,      r2 * 2 + 1, SSD1306_WHITE);
    o.drawFastVLine(cx,      cy - r2, r2 * 2 + 1, SSD1306_WHITE);
  }

  /* ---- Happy eyes ---- */
  void drawHappyEyes(Adafruit_SH1106G& o, int lx, int rx, int ey, int ew) {
    int midY = ey + 16;
    for (int t = 0; t < 10; t++) {
      o.drawLine(lx,       midY, lx + ew/2, midY - 12 + t, SSD1306_WHITE);
      o.drawLine(lx + ew/2, midY - 12 + t, lx + ew, midY,  SSD1306_WHITE);
    }
    for (int t = 0; t < 10; t++) {
      o.drawLine(rx,       midY, rx + ew/2, midY - 12 + t, SSD1306_WHITE);
      o.drawLine(rx + ew/2, midY - 12 + t, rx + ew, midY,  SSD1306_WHITE);
    }
  }

  /* ---- Heart eye ---- */
  void drawHeartEye(Adafruit_SH1106G& o, int cx, int cy) {
    o.fillCircle(cx - 5, cy - 4, 6, SSD1306_WHITE);
    o.fillCircle(cx + 5, cy - 4, 6, SSD1306_WHITE);
    o.fillTriangle(cx - 10, cy - 2, cx + 10, cy - 2, cx, cy + 8, SSD1306_WHITE);
  }

  void drawSmallHeart(Adafruit_SH1106G& o, int x, int y) {
    o.fillCircle(x + 2, y,     3, SSD1306_WHITE);
    o.fillCircle(x + 6, y,     3, SSD1306_WHITE);
    o.fillTriangle(x, y + 1, x + 8, y + 1, x + 4, y + 6, SSD1306_WHITE);
  }

  /* ---- Cute mouth ---- */
  void drawCuteMouth(Adafruit_SH1106G& o, int cx, int cy, MouthType mt) {
    switch (mt) {
      case MOUTH_SMILE_SMALL:
        o.fillCircle(cx,     cy,     2, SSD1306_WHITE);
        o.fillCircle(cx - 5, cy + 1, 2, SSD1306_WHITE);
        o.fillCircle(cx + 5, cy + 1, 2, SSD1306_WHITE);
        o.fillRect(cx - 5, cy, 11, 2, SSD1306_WHITE);
        break;
      case MOUTH_SMILE_BIG:
        o.drawLine(55, cy,     59, cy + 4, SSD1306_WHITE);
        o.drawLine(59, cy + 4, 64, cy + 1, SSD1306_WHITE);
        o.drawLine(64, cy + 1, 69, cy + 4, SSD1306_WHITE);
        o.drawLine(69, cy + 4, 73, cy,     SSD1306_WHITE);
        o.drawLine(55, cy + 1, 59, cy + 5, SSD1306_WHITE);
        o.drawLine(69, cy + 5, 73, cy + 1, SSD1306_WHITE);
        break;
      case MOUTH_SAD:
        o.drawLine(58, cy + 3, 62, cy,     SSD1306_WHITE);
        o.drawLine(62, cy,     66, cy,     SSD1306_WHITE);
        o.drawLine(66, cy,     70, cy + 3, SSD1306_WHITE);
        o.drawLine(58, cy + 4, 62, cy + 1, SSD1306_WHITE);
        o.drawLine(66, cy + 1, 70, cy + 4, SSD1306_WHITE);
        break;
    }
  }

  /* ---- MAIN DRAW ---- */
  void draw(unsigned long now) {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();

    MochiExpression expr = touchReacting ? touchExpr : currentAction.expression;

    int bounce = 0;
    if (currentAction.doBounce || touchReacting)
      bounce = (int)(1.5f * sinf(bouncePhase));

    int eyeW   = 28;
    int eyeH   = 26;
    int eyeGap = 8;
    int eyeY   = 10 + bounce;
    int lEyeX  = 64 - eyeGap/2 - eyeW;
    int rEyeX  = 64 + eyeGap/2;

    switch (expr) {

      /* NORMAL */
      case EXPR_NORMAL: {
        int eH = eyeH, eY = eyeY;
        if (blinking) {
          float ph = (float)(now - blinkStartMs) / 150.0f;
          float k  = 1.0f - sinf(ph * PI);
          eH = max(3, (int)(eyeH * k));
          eY = eyeY + (eyeH - eH) / 2;
        }
        o.fillRoundRect(lEyeX, eY, eyeW, eH, 7, SSD1306_WHITE);
        o.fillRoundRect(rEyeX, eY, eyeW, eH, 7, SSD1306_WHITE);
        if (!blinking) {
          o.fillRect(lEyeX + 4, eY + 4, 5, 4, SSD1306_BLACK);
          o.fillRect(rEyeX + 4, eY + 4, 5, 4, SSD1306_BLACK);
        }
        drawCuteMouth(o, 64, 50 + bounce, MOUTH_SMILE_SMALL);
        break;
      }

      /* HAPPY */
      case EXPR_HAPPY:
        drawHappyEyes(o, lEyeX, rEyeX, eyeY, eyeW);
        drawCuteMouth(o, 64, 50 + bounce, MOUTH_SMILE_BIG);
        break;

      /* EXCITED */
      case EXPR_EXCITED: {
        int eH = eyeH + 4;
        o.fillRoundRect(lEyeX - 1, eyeY, eyeW + 2, eH, 7, SSD1306_WHITE);
        o.fillRoundRect(rEyeX - 1, eyeY, eyeW + 2, eH, 7, SSD1306_WHITE);
        drawSparkle(o, lEyeX + eyeW/2, eyeY + eH/2, now);
        drawSparkle(o, rEyeX + eyeW/2, eyeY + eH/2, now);
        drawCuteMouth(o, 64, 51 + bounce, MOUTH_SMILE_BIG);
        break;
      }

      /* SAD */
      case EXPR_SAD:
        o.fillRoundRect(lEyeX, eyeY, eyeW, eyeH, 7, SSD1306_WHITE);
        o.fillRoundRect(rEyeX, eyeY, eyeW, eyeH, 7, SSD1306_WHITE);
        o.fillRect(lEyeX, eyeY, eyeW, eyeH * 2/5, SSD1306_BLACK);
        o.fillRect(rEyeX, eyeY, eyeW, eyeH * 2/5, SSD1306_BLACK);
        o.drawFastHLine(lEyeX, eyeY + eyeH * 2/5, eyeW, SSD1306_WHITE);
        o.drawFastHLine(rEyeX, eyeY + eyeH * 2/5, eyeW, SSD1306_WHITE);
        o.fillCircle(lEyeX + eyeW - 4, eyeY + eyeH + 3, 2, SSD1306_WHITE);
        o.fillCircle(rEyeX + 4,        eyeY + eyeH + 3, 2, SSD1306_WHITE);
        drawCuteMouth(o, 64, 51 + bounce, MOUTH_SAD);
        break;

      /* SLEEPY */
      case EXPR_SLEEPY:
        o.fillRoundRect(lEyeX, eyeY, eyeW, eyeH, 7, SSD1306_WHITE);
        o.fillRoundRect(rEyeX, eyeY, eyeW, eyeH, 7, SSD1306_WHITE);
        o.fillRect(lEyeX, eyeY, eyeW, eyeH/2 + 2, SSD1306_BLACK);
        o.fillRect(rEyeX, eyeY, eyeW, eyeH/2 + 2, SSD1306_BLACK);
        o.drawFastHLine(lEyeX, eyeY + eyeH/2 + 2, eyeW, SSD1306_WHITE);
        o.drawFastHLine(rEyeX, eyeY + eyeH/2 + 2, eyeW, SSD1306_WHITE);
        drawEllipseHelper(o, 64, 51 + bounce, 5, 4, SSD1306_WHITE);
        break;

      /* SLEEPING */
      case EXPR_SLEEPING: {
        int midY = eyeY + eyeH / 2;
        for (int t = 0; t < 3; t++) {
          o.drawLine(lEyeX,        midY + 2, lEyeX + eyeW/2, midY - 5 + t, SSD1306_WHITE);
          o.drawLine(lEyeX + eyeW/2, midY - 5 + t, lEyeX + eyeW, midY + 2, SSD1306_WHITE);
          o.drawLine(rEyeX,        midY + 2, rEyeX + eyeW/2, midY - 5 + t, SSD1306_WHITE);
          o.drawLine(rEyeX + eyeW/2, midY - 5 + t, rEyeX + eyeW, midY + 2, SSD1306_WHITE);
        }
        o.drawLine(57, 51, 61, 49, SSD1306_WHITE);
        o.drawLine(61, 49, 67, 51, SSD1306_WHITE);
        o.drawCircle(7, 7, 5, SSD1306_WHITE);
        o.fillCircle(10, 5, 4, SSD1306_BLACK);
        o.drawPixel(18, 3, SSD1306_WHITE);
        o.drawPixel(20, 8, SSD1306_WHITE);
        o.drawPixel(25, 2, SSD1306_WHITE);
        drawZzz(o);
        break;
      }

      /* ANNOYED / BORED */
      case EXPR_ANNOYED:
      case EXPR_BORED:
        o.fillRoundRect(lEyeX, eyeY, eyeW, eyeH, 7, SSD1306_WHITE);
        o.fillRoundRect(rEyeX, eyeY, eyeW, eyeH, 7, SSD1306_WHITE);
        o.fillRect(lEyeX, eyeY, eyeW, eyeH * 2/5, SSD1306_BLACK);
        o.fillRect(rEyeX, eyeY, eyeW, eyeH * 2/5, SSD1306_BLACK);
        o.drawFastHLine(lEyeX, eyeY + eyeH * 2/5, eyeW, SSD1306_WHITE);
        o.drawFastHLine(rEyeX, eyeY + eyeH * 2/5, eyeW, SSD1306_WHITE);
        o.drawFastHLine(58, 51 + bounce, 16, SSD1306_WHITE);
        o.drawFastHLine(58, 52 + bounce, 16, SSD1306_WHITE);
        break;

      /* SHOCKED */
      case EXPR_SHOCKED:
        o.fillCircle(lEyeX + eyeW/2, eyeY + eyeH/2, 13, SSD1306_WHITE);
        o.fillCircle(rEyeX + eyeW/2, eyeY + eyeH/2, 13, SSD1306_WHITE);
        o.fillCircle(lEyeX + eyeW/2, eyeY + eyeH/2, 6,  SSD1306_BLACK);
        o.fillCircle(rEyeX + eyeW/2, eyeY + eyeH/2, 6,  SSD1306_BLACK);
        o.fillCircle(lEyeX + eyeW/2 - 3, eyeY + eyeH/2 - 3, 2, SSD1306_WHITE);
        o.fillCircle(rEyeX + eyeW/2 - 3, eyeY + eyeH/2 - 3, 2, SSD1306_WHITE);
        drawEllipseHelper(o, 64, 52 + bounce, 7, 5, SSD1306_WHITE);
        drawEllipseHelper(o, 64, 52 + bounce, 4, 3, SSD1306_BLACK);
        for (int i = -2; i <= 2; i++)
          o.drawFastVLine(64 + i * 8, 1, 6, SSD1306_WHITE);
        break;

      /* LOVE */
      case EXPR_LOVE: {
        drawHeartEye(o, lEyeX + eyeW/2, eyeY + eyeH/2 + 1);
        drawHeartEye(o, rEyeX + eyeW/2, eyeY + eyeH/2 + 1);
        int hOff = (int)(3.0f * sinf(sparklePhase));
        drawSmallHeart(o, lEyeX - 4,        eyeY - 5 - hOff);
        drawSmallHeart(o, rEyeX + eyeW + 1, eyeY - 5 - hOff);
        drawCuteMouth(o, 64, 51 + bounce, MOUTH_SMILE_BIG);
        break;
      }

      /* THINKING */
      case EXPR_THINKING:
        o.fillRoundRect(lEyeX, eyeY, eyeW, eyeH, 7, SSD1306_WHITE);
        o.fillRect(lEyeX + 4, eyeY + 4, 5, 4, SSD1306_BLACK);
        o.fillRoundRect(rEyeX, eyeY, eyeW, eyeH, 7, SSD1306_WHITE);
        o.fillRect(rEyeX, eyeY, eyeW, eyeH * 3/5, SSD1306_BLACK);
        o.drawFastHLine(rEyeX, eyeY + eyeH * 3/5, eyeW, SSD1306_WHITE);
        o.drawPixel(92,  8, SSD1306_WHITE);
        o.drawPixel(96,  6, SSD1306_WHITE);
        o.drawPixel(100, 8, SSD1306_WHITE);
        drawCuteMouth(o, 64, 51 + bounce, MOUTH_SMILE_SMALL);
        break;

      /* PLAYFUL */
      case EXPR_PLAYFUL: {
        int eH = eyeH, eY = eyeY;
        if (blinking) {
          float ph = (float)(now - blinkStartMs) / 150.0f;
          float k  = 1.0f - sinf(ph * PI);
          eH = max(3, (int)(eyeH * k));
          eY = eyeY + (eyeH - eH) / 2;
        }
        o.fillRoundRect(lEyeX, eY, eyeW, eH, 7, SSD1306_WHITE);
        o.fillRect(lEyeX + 4, eY + 4, 5, 4, SSD1306_BLACK);
        int wY = eyeY + eyeH / 2;
        for (int t = 0; t < 3; t++) {
          o.drawLine(rEyeX,        wY + 2, rEyeX + eyeW/2, wY - 4 + t, SSD1306_WHITE);
          o.drawLine(rEyeX + eyeW/2, wY - 4 + t, rEyeX + eyeW, wY + 2, SSD1306_WHITE);
        }
        o.fillRoundRect(59, 50 + bounce, 10, 7, 3, SSD1306_WHITE);
        o.drawFastHLine(59, 53 + bounce, 10, SSD1306_BLACK);
        o.drawFastHLine(60, 54 + bounce, 4,  SSD1306_BLACK);
        break;
      }

      /* ANGRY */
      case EXPR_ANGRY:
        o.fillCircle(lEyeX + eyeW/2, eyeY + eyeH/2, 12, SSD1306_WHITE);
        o.fillCircle(rEyeX + eyeW/2, eyeY + eyeH/2, 12, SSD1306_WHITE);
        o.fillCircle(lEyeX + eyeW/2, eyeY + eyeH/2, 5,  SSD1306_BLACK);
        o.fillCircle(rEyeX + eyeW/2, eyeY + eyeH/2, 5,  SSD1306_BLACK);
        o.drawLine(lEyeX,      eyeY - 4, lEyeX + eyeW, eyeY - 1, SSD1306_WHITE);
        o.drawLine(rEyeX,      eyeY - 1, rEyeX + eyeW, eyeY - 4, SSD1306_WHITE);
        o.drawLine(lEyeX,      eyeY - 3, lEyeX + eyeW, eyeY,     SSD1306_WHITE);
        o.drawLine(rEyeX,      eyeY,     rEyeX + eyeW, eyeY - 3, SSD1306_WHITE);
        o.fillRoundRect(55, 49 + bounce, 18, 10, 3, SSD1306_WHITE);
        o.fillRoundRect(58, 51 + bounce, 12,  6, 2, SSD1306_BLACK);
        o.setTextSize(2);
        o.setCursor(108, 2);
        o.print("!");
        o.setTextSize(1);
        for (int row = 4; row < 64; row += 8) {
          o.drawPixel(random(0,  10),  row, SSD1306_WHITE);
          o.drawPixel(random(118, 128), row, SSD1306_WHITE);
        }
        break;

      default:
        o.fillRoundRect(lEyeX, eyeY, eyeW, eyeH, 7, SSD1306_WHITE);
        o.fillRoundRect(rEyeX, eyeY, eyeW, eyeH, 7, SSD1306_WHITE);
        o.fillRect(lEyeX + 4, eyeY + 4, 5, 4, SSD1306_BLACK);
        o.fillRect(rEyeX + 4, eyeY + 4, 5, 4, SSD1306_BLACK);
        drawCuteMouth(o, 64, 50 + bounce, MOUTH_SMILE_SMALL);
        break;
    }

    /* Props */
    bool showingProps = !touchReacting;
    if (showingProps && currentAction.showPhone &&
        expr != EXPR_SLEEPING && expr != EXPR_ANGRY)
      drawPhone(o, 98, 20 + bounce);
    if (showingProps && currentAction.showCoffee &&
        expr != EXPR_SLEEPING && expr != EXPR_ANGRY)
      drawCoffeeCup(o, 100, 30 + bounce, coffeeSip);

    o.display();
  }
};

/* ============================ WatchApp ============================== */
class WatchApp {
  DisplayManager*  dm = nullptr;
  TimeManager*     tm = nullptr;
  BatteryManager*  bm = nullptr;
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
  float birdY = 32.0f;
  float birdV = 0.0f;
  static constexpr int   BIRD_X    = 22;
  static constexpr int   BIRD_R    = 4;
  static constexpr float GRAVITY   = 0.15f;
  static constexpr float FLAP_V    = -2.0f;
  static constexpr float PIPE_SPEED = 1.2f;
  static constexpr int   NPIPES    = 2;
  static constexpr int   PIPE_W    = 12;
  static constexpr int   GAP_H     = 44;
  static constexpr int   PIPE_SPACING = 80;

  float pipeX[NPIPES];
  int   gapY[NPIPES];
  bool  passed[NPIPES];

  int  score = 0;
  int  best  = 0;
  bool dirty = true;
  unsigned long lastFrameMs = 0;
  float wingPhase = 0.0f;

  static constexpr float BIRD_START_Y  = 30.0f;
  unsigned long gameStartMs = 0;
  static constexpr unsigned long PIPE_DELAY_MS = 2000;

public:
  void begin(DisplayManager* d) {
    dm = d;
    prefs.begin("flappy", false);
    best = prefs.getInt("best", 0);
  }

  void onEnter() { gstate = G_START; dirty = true; }

  bool isPlaying()       const { return gstate == G_PLAYING; }
  bool isOnStartScreen() const { return gstate == G_START;   }

  void onSingleTap() {
    if      (gstate == G_START)   startGame();
    else if (gstate == G_PLAYING) { birdV = FLAP_V; wingPhase = 0.0f; }
    else if (gstate == G_OVER)    startGame();
  }

  bool onDoubleTap() { return (gstate == G_START); }

  void startGame() {
    birdY = BIRD_START_Y; birdV = 0.0f;
    score = 0; wingPhase = 0.0f;
    gameStartMs = millis();
    for (int i = 0; i < NPIPES; i++) {
      pipeX[i]  = 128.0f + 50.0f + i * PIPE_SPACING;
      gapY[i]   = random(10, 64 - 10 - GAP_H);
      passed[i] = false;
    }
    gstate = G_PLAYING;
    dirty  = false;
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
          for (int j = 1; j < NPIPES; j++)
            if (pipeX[j] > maxX) maxX = pipeX[j];
          pipeX[i] = maxX + PIPE_SPACING;
          gapY[i]  = random(10, 64 - 10 - GAP_H);
          passed[i]= false;
        }
        if (!passed[i] && pipeX[i] + PIPE_W < BIRD_X - BIRD_R) {
          passed[i] = true; score++;
        }
      }
    }

    bool dead = (birdY - BIRD_R <= 1) || (birdY + BIRD_R >= 61);
    if (pipesActive) {
      for (int i = 0; i < NPIPES && !dead; i++) {
        bool inX  = ((BIRD_X + BIRD_R) > (int)pipeX[i]) &&
                    ((BIRD_X - BIRD_R) < (int)(pipeX[i] + PIPE_W));
        bool inGap= ((birdY - BIRD_R) >= gapY[i]) &&
                    ((birdY + BIRD_R) <= gapY[i] + GAP_H);
        if (inX && !inGap) dead = true;
      }
    }

    if (dead) {
      if (score > best) { best = score; prefs.putInt("best", best); }
      gstate = G_OVER; dirty = true;
      return;
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
    int bodyH = 63 - (botY + 4);
    o.fillRect(px - 1, botY, PIPE_W + 2, 4, SSD1306_WHITE);
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
    o.drawLine(bx - BIRD_R, by + 1, bx - BIRD_R - 2, by - 1, SSD1306_WHITE);
  }

  void drawStart() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    o.fillRoundRect(10, 4, 108, 16, 4, SSD1306_WHITE);
    o.setTextColor(SSD1306_BLACK);
    dm->centerText("FLAPPY MOCHI", 8, 1);
    o.setTextColor(SSD1306_WHITE);
    o.fillRect(0,   0,  5, 18, SSD1306_WHITE);
    o.fillRect(123, 0,  5, 18, SSD1306_WHITE);
    o.fillRect(0,   50, 5, 14, SSD1306_WHITE);
    o.fillRect(123, 50, 5, 14, SSD1306_WHITE);
    drawBird(o, 64, 36);
    o.drawFastHLine(0, 62, 128, SSD1306_WHITE);
    o.drawFastHLine(0, 63, 128, SSD1306_WHITE);
    dm->centerText("Tap - Start",    45, 1);
    dm->centerText("2xTap - Switch", 54, 1);
    o.display();
  }

  void drawGame(unsigned long now) {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();

    bool pipesActive = (now - gameStartMs > PIPE_DELAY_MS);
    if (pipesActive) {
      for (int i = 0; i < NPIPES; i++) {
        int px   = (int)pipeX[i];
        int topH = gapY[i];
        int botY = gapY[i] + GAP_H;
        if (px < 128 && px + PIPE_W > 0)
          drawPipe(o, px, topH, botY);
      }
    }
    o.drawFastHLine(0, 62, 128, SSD1306_WHITE);
    o.drawFastHLine(0, 63, 128, SSD1306_WHITE);
    for (int x = 0; x < 128; x += 6) o.drawPixel(x, 61, SSD1306_WHITE);

    drawBird(o, BIRD_X, (int)birdY);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", score);
    int sw = (int)(strlen(buf) * 6 + 8);
    o.drawRoundRect((128 - sw) / 2, 1, sw, 10, 2, SSD1306_WHITE);
    dm->centerText(buf, 3, 1);

    if (!pipesActive) dm->centerText("Get Ready!", 25, 1);

    o.display();
  }

  void drawGameOver() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    o.drawRoundRect(4,  2,  120, 60, 5, SSD1306_WHITE);
    o.drawRoundRect(6,  4,  116, 56, 3, SSD1306_WHITE);
    o.fillRoundRect(10, 6,  108, 14, 3, SSD1306_WHITE);
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
  OtaState state = OTA_IDLE;
  unsigned long statusTimer = 0;
  String errorMsg = "";

public:
  void begin(DisplayManager* d) { dm = d; }

  void onEnter() {
    state = OTA_CONNECTING;
    statusTimer = millis();
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  void update() {
    Adafruit_SH1106G& o = dm->oled;
    unsigned long now = millis();

    switch (state) {
      case OTA_CONNECTING:
        o.clearDisplay();
        dm->centerText("OTA UPDATE", 5, 1);
        dm->centerText("Connecting Wi-Fi", 22, 1);
        o.drawRect(14, 38, 100, 6, SSD1306_WHITE);
        {
          int dots = (now / 400) % 4;
          char dBuf[5] = "    ";
          for (int i = 0; i < dots; i++) dBuf[i] = '.';
          dm->centerText(dBuf, 50, 1);
        }
        o.display();
        if (WiFi.status() == WL_CONNECTED) {
          state = OTA_CHECKING;
        } else if (now - statusTimer > 20000) {
          fail("Wi-Fi Timeout");
        }
        break;

      case OTA_CHECKING:
        o.clearDisplay();
        dm->centerText("OTA UPDATE", 5, 1);
        dm->centerText("Checking GitHub...", 28, 1);
        o.display();
        runCheck();
        break;

      case OTA_DOWNLOADING:
        break;

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
    errorMsg = msg;
    state = OTA_FAIL;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  void runCheck() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (http.begin(client, VERSION_URL)) {
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        payload.trim();
        int newVersion = payload.toInt();
        if (newVersion > CURRENT_VERSION) {
          state = OTA_DOWNLOADING;
          executeUpdate(client);
        } else {
          state = OTA_UP_TO_DATE;
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
        }
      } else {
        fail("HTTP Err: " + String(httpCode));
      }
      http.end();
    } else {
      fail("Connection Fail");
    }
  }

  void executeUpdate(WiFiClientSecure& client) {
    HTTPClient http;
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    dm->centerText("DOWNLOADING...", 25, 1);
    o.display();

    if (http.begin(client, BIN_URL)) {
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
        int contentLength = http.getSize();
        if (Update.begin(contentLength)) {
          WiFiClient* stream = http.getStreamPtr();
          size_t written = 0;
          uint8_t buff[256];
          while (http.connected() && written < (size_t)contentLength) {
            size_t available = stream->available();
            if (available) {
              int c = stream->readBytes(buff,
                (available > sizeof(buff)) ? sizeof(buff) : available);
              Update.write(buff, c);
              written += c;
              o.clearDisplay();
              dm->centerText("FLASHING UPDATE", 5, 1);
              o.drawRect(14, 26, 100, 12, SSD1306_WHITE);
              int pw = (int)map((long)written, 0L, (long)contentLength, 0L, 96L);
              if (pw > 0) o.fillRect(16, 28, pw, 8, SSD1306_WHITE);
              char pBuf[8];
              snprintf(pBuf, sizeof(pBuf), "%d%%",
                       (int)((written * 100) / contentLength));
              dm->centerText(pBuf, 44, 1);
              o.display();
            }
          }
          if (Update.end() && Update.isFinished()) {
            o.clearDisplay();
            dm->centerText("SUCCESS!", 10, 1);
            dm->centerText("Rebooting...", 35, 1);
            o.display();
            delay(2000);
            ESP.restart();
          } else {
            fail("Flash Error");
          }
        } else {
          fail("No Space");
        }
      } else {
        fail("Bin HTTP Err: " + String(httpCode));
      }
      http.end();
    } else {
      fail("Bin Link Fail");
    }
  }
};

/* ============================ AIManager ============================= */
class AIManager {
  enum AIState {
    AI_IDLE, AI_CONNECTING, AI_REQUESTING, AI_DONE
  };
  AIState state        = AI_IDLE;
  unsigned long stateStartMs = 0;

  MochiApp*    mochiRef = nullptr;
  TimeManager* timeRef  = nullptr;

public:
  void begin(MochiApp* m, TimeManager* t) {
    mochiRef = m;
    timeRef  = t;
  }

  void update() {
    unsigned long now = millis();

    switch (state) {

      case AI_IDLE:
        if (mochiRef->shouldPollAI()) {
          WiFi.mode(WIFI_STA);
          WiFi.begin(WIFI_SSID, WIFI_PASS);
          state = AI_CONNECTING;
          stateStartMs = now;
          Serial.println("[AI] Connecting WiFi...");
        }
        break;

      case AI_CONNECTING:
        if (WiFi.status() == WL_CONNECTED) {
          Serial.println("[AI] WiFi connected, requesting...");
          state = AI_REQUESTING;
          stateStartMs = now;
          doRequest();
        } else if (now - stateStartMs > 20000) {
          Serial.println("[AI] WiFi timeout");
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
          mochiRef->markAIPollDone();
          state = AI_IDLE;
        }
        break;

      case AI_REQUESTING:
        // Handled synchronously in doRequest()
        break;

      case AI_DONE:
        state = AI_IDLE;
        break;
    }
  }

private:
  void doRequest() {
    struct tm t;
    timeRef->getLocal(t);
    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", t.tm_hour, t.tm_min);

    // Build prompt
    String prompt =
      "You are controlling a cute robot pet called Mochi displayed on a 128x64 OLED. "
      "Current time: " + String(timeBuf) + ". "
      "Respond ONLY with a JSON object like: "
      "{\"expression\":\"happy\",\"showPhone\":false,\"showCoffee\":false,"
      "\"doLookAround\":false,\"doBounce\":true,\"durationMs\":8000} "
      "Valid expressions: normal,happy,sad,excited,sleepy,sleeping,annoyed,"
      "bored,shocked,love,thinking,playful,angry. "
      "Choose an appropriate mood for the time of day.";

    // Build Gemini request body
    StaticJsonDocument<1024> reqDoc;
    JsonArray contents = reqDoc.createNestedArray("contents");
    JsonObject part    = contents.createNestedObject();
    JsonArray parts    = part.createNestedArray("parts");
    JsonObject textObj = parts.createNestedObject();
    textObj["text"]    = prompt;

    String reqBody;
    serializeJson(reqDoc, reqBody);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    String url = String("https://") + GEMINI_HOST +
                 "/v1beta/models/gemini-1.5-flash-latest:generateContent?key=" +
                 GEMINI_API_KEY;

    Serial.println("[AI] POST " + url);

    if (!http.begin(client, url)) {
      Serial.println("[AI] http.begin failed");
      cleanup();
      return;
    }

    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST(reqBody);
    Serial.printf("[AI] HTTP code: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      String resp = http.getString();
      Serial.println("[AI] Response: " + resp);
      parseAndApply(resp);
    } else {
      Serial.println("[AI] Request failed: " + String(httpCode));
    }

    http.end();
    cleanup();
  }

  void parseAndApply(const String& resp) {
    // Find JSON inside Gemini response text field
    int start = resp.indexOf('{');
    int end   = resp.lastIndexOf('}');
    if (start < 0 || end < 0 || end <= start) {
      Serial.println("[AI] No JSON found in response");
      return;
    }
    String jsonStr = resp.substring(start, end + 1);
    Serial.println("[AI] Extracted JSON: " + jsonStr);

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, jsonStr);
    if (err) {
      Serial.println("[AI] JSON parse error: " + String(err.c_str()));
      return;
    }

    String exprStr      = doc["expression"]   | "normal";
    bool   showPhone    = doc["showPhone"]     | false;
    bool   showCoffee   = doc["showCoffee"]    | false;
    bool   doLookAround = doc["doLookAround"]  | false;
    bool   doBounce     = doc["doBounce"]      | true;
    unsigned long durMs = doc["durationMs"]    | 8000;

    mochiRef->applyAIActionFromStrings(exprStr, showPhone, showCoffee,
                                       doLookAround, doBounce, durMs);
    Serial.println("[AI] Applied expression: " + exprStr);
  }

  void cleanup() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    mochiRef->markAIPollDone();
    state = AI_DONE;
  }
};

/* ========================== Global instances ======================== */
DisplayManager displayMgr;
TouchManager   touchMgr;
BatteryManager batteryMgr;
TimeManager    timeMgr;
MochiApp       mochiApp;
WatchApp       watchApp;
FlappyApp      flappyApp;
UpdateApp      updateApp;
AIManager      aiMgr;

AppMode currentMode = MODE_MOCHI;
unsigned long lastActivityMs = 0;

/* ========================== Mode switching ========================== */
void switchMode(AppMode newMode) {
  currentMode = newMode;
  lastActivityMs = millis();
  switch (newMode) {
    case MODE_MOCHI:  mochiApp.onEnter();  break;
    case MODE_WATCH:  watchApp.onEnter();  break;
    case MODE_FLAPPY: flappyApp.onEnter(); break;
    case MODE_UPDATE: updateApp.onEnter(); break;
  }
}

/* ========================== Splash screen =========================== */
void drawSplash() {
  Adafruit_SH1106G& o = displayMgr.oled;
  o.clearDisplay();
  o.fillRoundRect(10, 4, 108, 18, 5, SSD1306_WHITE);
  o.setTextColor(SSD1306_BLACK);
  displayMgr.centerText("MochiPod v3", 9, 1);
  o.setTextColor(SSD1306_WHITE);
  displayMgr.centerText("Initializing...", 30, 1);
  displayMgr.centerText("by itsfortestlol", 44, 1);
  char vBuf[16];
  snprintf(vBuf, sizeof(vBuf), "fw: %d", CURRENT_VERSION);
  displayMgr.centerText(vBuf, 55, 1);
  o.display();
  delay(2000);
}

/* ============================= setup() ============================= */
void setup() {
  Serial.begin(115200);
  delay(100);

  if (!displayMgr.begin()) {
    Serial.println("OLED init failed!");
    while (true) delay(1000);
  }

  touchMgr.begin();
  batteryMgr.begin();

  drawSplash();

  timeMgr.begin();
  mochiApp.begin(&displayMgr, &timeMgr);
  watchApp.begin(&displayMgr, &timeMgr, &batteryMgr);
  flappyApp.begin(&displayMgr);
  updateApp.begin(&displayMgr);
  aiMgr.begin(&mochiApp, &timeMgr);

  lastActivityMs = millis();
  Serial.println("[BOOT] MochiPod ready");
}

/* ============================= loop() ============================== */
void loop() {
  unsigned long now = millis();

  /* Update managers */
  batteryMgr.update();
  timeMgr.update();

  /* Touch events */
  TapEvent tap = touchMgr.update();
  if (tap != TAP_NONE) lastActivityMs = now;

  /* Route tap events to current app */
  if (tap == TAP_SINGLE) {
    switch (currentMode) {
      case MODE_MOCHI:
        mochiApp.onSingleTap();
        break;
      case MODE_WATCH:
        // Single tap on watch: do nothing special
        break;
      case MODE_FLAPPY:
        flappyApp.onSingleTap();
        break;
      case MODE_UPDATE:
        break;
    }
  }

  if (tap == TAP_DOUBLE) {
    switch (currentMode) {
      case MODE_MOCHI:
        switchMode(MODE_WATCH);
        break;
      case MODE_WATCH:
        switchMode(MODE_FLAPPY);
        break;
      case MODE_FLAPPY:
        if (flappyApp.onDoubleTap()) {
          switchMode(MODE_UPDATE);
        }
        break;
      case MODE_UPDATE:
        switchMode(MODE_MOCHI);
        break;
    }
  }

  /* Update current app */
  switch (currentMode) {
    case MODE_MOCHI:  mochiApp.update();  break;
    case MODE_WATCH:  watchApp.update();  break;
    case MODE_FLAPPY: flappyApp.update(); break;
    case MODE_UPDATE: updateApp.update(); break;
  }

  /* AI polling (only in Mochi mode) */
  if (currentMode == MODE_MOCHI) {
    aiMgr.update();
  }

  /* Idle timeout: return to Mochi from Watch after inactivity */
  if (currentMode == MODE_WATCH &&
      now - lastActivityMs > IDLE_TIMEOUT) {
    switchMode(MODE_MOCHI);
  }
}

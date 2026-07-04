/**********************************************************************
 * MochiPod - ESP32 Handheld (Mochi / Watch / Flappy Bird / OTA Update)
 * Board   : ESP32 DevKit V1 (ESP-WROOM-32)
 * Display : SH1106 128x64 I2C    Touch: TTP223 (GPIO 4, active HIGH)
 * Core    : ESP32 Arduino core 2.x / 3.x
 *********************************************************************/
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>  // Switched library header to SH110X
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>

/* Global compatibility macros to ensure original colors function unaltered */
#define SSD1306_WHITE SH110X_WHITE
#define SSD1306_BLACK SH110X_BLACK

/* ============================ USER CONFIG =========================== */
const char* WIFI_SSID      = "Afia Mahir";
const char* WIFI_PASS      = "afiamahir2026"; [span_5](start_span)//[span_5](end_span)
const long  GMT_OFFSET_SEC = 21600;           [span_6](start_span)// Bangladesh GMT+6[span_6](end_span)
const int   DST_OFFSET_SEC = 0;
const char* NTP_SERVER     = "asia.pool.ntp.org"; [span_7](start_span)// Optimization: Regional Asian pool[span_7](end_span)

// OTA Update Configuration
const int   CURRENT_VERSION = 1;
const char* VERSION_URL     = "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/main.ino"; [span_8](start_span)//[span_8](end_span)
const char* BIN_URL         = "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/firmware.bin";

/* ------------------------------ Pins ------------------------------- */
#define PIN_OLED_SDA   21
#define PIN_OLED_SCL   22
#define PIN_TOUCH      4      // TTP223 SIG, active HIGH
[span_9](start_span)#define PIN_VBAT       34     // ADC1_CH6, via 100k/100k divider[span_9](end_span)

/* ------------------------- Battery calibration --------------------- */
const float VBAT_DIVIDER   = 2.0f;  [span_10](start_span)// 100k/100k halves the voltage[span_10](end_span)
const float VBAT_CAL       = 1.00f; [span_11](start_span)// fine-tune vs multimeter reading[span_11](end_span)
const float VBAT_FULL      = 4.20f; [span_12](start_span)// = 100 %[span_12](end_span)
const float VBAT_EMPTY     = 3.30f; [span_13](start_span)// = 0 %[span_13](end_span)

/* --------------------------- Touch timing -------------------------- */
const unsigned long TOUCH_DEBOUNCE_MS    = 30;  // TTP223 is clean; [span_14](start_span)30ms is plenty[span_14](end_span)
const unsigned long DOUBLE_TAP_WINDOW_MS = 300; [span_15](start_span)// max gap between two taps[span_15](end_span)

/* ------------------------------ Idle ------------------------------- */
const unsigned long IDLE_TIMEOUT = 20000; [span_16](start_span)// 20 s -> back to Mochi[span_16](end_span)

/* =========================== App Modes ============================= */
enum AppMode { MODE_MOCHI, MODE_WATCH, MODE_FLAPPY, MODE_UPDATE }; [span_17](start_span)//[span_17](end_span)
enum TapEvent { TAP_NONE, TAP_SINGLE, TAP_DOUBLE };

/* ======================== DisplayManager =========================== */
class DisplayManager {
public:
  Adafruit_SH1106G oled; [span_18](start_span)// Switched hardware instance class to SH1106G[span_18](end_span)
  DisplayManager() : oled(128, 64, &Wire, -1) {}

  bool begin() {
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    Wire.setClock(400000);                     [span_19](start_span)// fast I2C = smoother frames[span_19](end_span)
    if (!oled.begin(0x3C, true)) return false; [span_20](start_span)// Initialized using SH1106 constraints[span_20](end_span)
    oled.clearDisplay();
    oled.setTextColor(SH110X_WHITE);
    oled.display();
    return true; [span_21](start_span)//[span_21](end_span)
  }
  /* Helper: centered text at a given text size */
  void centerText(const char* s, int16_t y, uint8_t size) {
    oled.setTextSize(size);
    int16_t w = strlen(s) * 6 * size; [span_22](start_span)//[span_22](end_span)
    oled.setCursor((128 - w) / 2, y);
    oled.print(s);
  }
};

/* ========================= TouchManager ============================
 * Debounced edge detection + single/double tap discrimination.
 * A single tap is reported only after DOUBLE_TAP_WINDOW_MS passes
 * [span_23](start_span)without a second tap.[span_23](end_span)
 */
class TouchManager {
  bool stableState = false, lastRead = false;
  unsigned long lastChangeMs = 0, firstTapMs = 0;
  bool waitingSecondTap = false; [span_24](start_span)//[span_24](end_span)
public:
  void begin() { pinMode(PIN_TOUCH, INPUT); [span_25](start_span)} //[span_25](end_span)

  TapEvent update() {
    unsigned long now = millis();
    TapEvent ev = TAP_NONE;
    bool raw = digitalRead(PIN_TOUCH) == HIGH; [span_26](start_span)//[span_26](end_span)
    if (raw != lastRead) { lastRead = raw; lastChangeMs = now; [span_27](start_span)} //[span_27](end_span)

    if ((now - lastChangeMs) >= TOUCH_DEBOUNCE_MS && raw != stableState) {
      stableState = raw;
      [span_28](start_span)if (stableState) {                      // debounced press (rising edge)[span_28](end_span)
        if (waitingSecondTap && (now - firstTapMs) <= DOUBLE_TAP_WINDOW_MS) {
          waitingSecondTap = false;
          ev = TAP_DOUBLE; [span_29](start_span)//[span_29](end_span)
        } else {
          firstTapMs = now;
          waitingSecondTap = true; [span_30](start_span)//[span_30](end_span)
        }
      }
    }
    /* First tap "expires" -> it was a single tap */
    if (waitingSecondTap && (now - firstTapMs) > DOUBLE_TAP_WINDOW_MS) {
      waitingSecondTap = false;
      ev = TAP_SINGLE; [span_31](start_span)//[span_31](end_span)
    }
    return ev;
  }
};

/* ======================== BatteryManager =========================== */
class BatteryManager {
  unsigned long lastReadMs = 0;
  float voltage = 3.7f;
  int percent = 50; [span_32](start_span)//[span_32](end_span)
public:
  void begin() {
    analogSetPinAttenuation(PIN_VBAT, ADC_11db); [span_33](start_span)// full 0-3.3V range[span_33](end_span)
    sample();                                     // initial reading
  }
  void update() {
    if (millis() - lastReadMs >= 5000) sample(); [span_34](start_span)// every 5 s[span_34](end_span)
  }
  void sample() {
    lastReadMs = millis();
    uint32_t mv = 0;
    for (int i = 0; i < 10; i++) mv += analogReadMilliVolts(PIN_VBAT); [span_35](start_span)//[span_35](end_span)
    voltage = (mv / 10) / 1000.0f * VBAT_DIVIDER * VBAT_CAL; [span_36](start_span)//[span_36](end_span)
    float p = (voltage - VBAT_EMPTY) / (VBAT_FULL - VBAT_EMPTY) * 100.0f; [span_37](start_span)//[span_37](end_span)
    percent = constrain((int)(p + 0.5f), 0, 100);
  }
  int   getPercent() const { return percent; }
  float getVoltage() const { return voltage; }
};

/* ========================== TimeManager ============================
 * Non-blocking background sync loop. If connection fails, drops 
 * [span_38](start_span)out cleanly and initiates background re-try passes every 60s.[span_38](end_span)
 */
class TimeManager {
  enum St { CONNECTING, SYNCING, DONE };
  St state = CONNECTING;
  unsigned long startMs = 0;
  unsigned long lastRetryMs = 0; [span_39](start_span)//[span_39](end_span)
  bool synced = false;
public:
  void begin() {
    startSync();
  [span_40](start_span)} //[span_40](end_span)
  void startSync() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    state = CONNECTING;
    startMs = millis(); [span_41](start_span)//[span_41](end_span)
  }
  void update() {
    unsigned long now = millis();
    [span_42](start_span)if (state == CONNECTING) { //[span_42](end_span)
      if (WiFi.status() == WL_CONNECTED) {
        configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER); [span_43](start_span)//[span_43](end_span)
        state = SYNCING; startMs = now;
      } else if (now - startMs > 30000) {   // Extended from 15s to 30s
        shutdownWifi();
        lastRetryMs = now; [span_44](start_span)//[span_44](end_span)
      }
    } else if (state == SYNCING) {
      if (time(nullptr) > 1700000000) {          // Sane epoch verified => synced
        synced = true;
        shutdownWifi();                          [span_45](start_span)// save battery[span_45](end_span)
      } else if (now - startMs > 15000) {   // Extended from 10s to 15s
        shutdownWifi();
        lastRetryMs = now; [span_46](start_span)//[span_46](end_span)
      }
    } else if (state == DONE && !synced) {
      /* If sync dropped or failed, trigger a background evaluation sequence every 60 seconds */
      if (now - lastRetryMs > 60000) {
        startSync();
      [span_47](start_span)} //[span_47](end_span)
    }
  }
  void shutdownWifi() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    state = DONE;
  [span_48](start_span)} //[span_48](end_span)
  bool isSynced() const { return synced; }
  void getLocal(struct tm& t) {
    time_t current = time(nullptr);
    localtime_r(&current, &t); [span_49](start_span)//[span_49](end_span)
  }
};

/* ============================ MochiApp ==============================
 * Dasai-Mochi-style face: two rounded-rect eyes + small mouth, drawn
 * procedurally (crisp 1-bit pixel look).
 * [span_50](start_span)Blinks, bounces, looks around, random expressions, and a tap reaction.[span_50](end_span) */
class MochiApp {
  DisplayManager* dm;
  enum Expr { EX_NORMAL, EX_LOOK_L, EX_LOOK_R, EX_HAPPY, EX_SURPRISED }; [span_51](start_span)//[span_51](end_span)
  Expr expr = EX_NORMAL;
  unsigned long nextExprMs = 0, nextBlinkMs = 0, blinkStartMs = 0; [span_52](start_span)//[span_52](end_span)
  unsigned long reactionEndMs = 0, lastFrameMs = 0;
  bool blinking = false, reactionHappy = true; [span_53](start_span)//[span_53](end_span)
public:
  void begin(DisplayManager* d) {
    dm = d;
    nextExprMs  = millis() + random(3000, 6000); [span_54](start_span)//[span_54](end_span)
    nextBlinkMs = millis() + random(2000, 5000);
  [span_55](start_span)} //[span_55](end_span)
  void onEnter() { expr = EX_NORMAL; reactionEndMs = 0; [span_56](start_span)} //[span_56](end_span)

  void onSingleTap() {                         // reaction animation
    reactionHappy = random(0, 2);              [span_57](start_span)// happy or surprised[span_57](end_span)
    reactionEndMs = millis() + 1500; [span_58](start_span)//[span_58](end_span)
  }

  void update() {
    unsigned long now = millis();
    if (now - lastFrameMs < 33) return;        [span_59](start_span)// ~30 fps cap[span_59](end_span)
    lastFrameMs = now; [span_60](start_span)//[span_60](end_span)
    /* Random expression scheduler (skipped during reaction) */
    if (now >= nextExprMs && now >= reactionEndMs) {
      Expr pool[] = { EX_NORMAL, EX_NORMAL, EX_LOOK_L, EX_LOOK_R, EX_HAPPY };
      expr = pool[random(0, 5)]; [span_61](start_span)//[span_61](end_span)
      nextExprMs = now + random(3000, 6000); [span_62](start_span)//[span_62](end_span)
    }
    /* Blink scheduler */
    if (!blinking && now >= nextBlinkMs) { blinking = true; blinkStartMs = now; [span_63](start_span)} //[span_63](end_span)
    if (blinking && now - blinkStartMs > 180) {
      blinking = false;
      nextBlinkMs = now + random(2000, 5000); [span_64](start_span)//[span_64](end_span)
    }
    draw(now);
  [span_65](start_span)} //[span_65](end_span)

  void draw(unsigned long now) {
    Adafruit_SH1106G& o = dm->oled; [span_66](start_span)// Internal display reference cast to SH1106G[span_66](end_span)
    o.clearDisplay(); [span_67](start_span)//[span_67](end_span)
    /* Gentle bounce: whole face floats up/down */
    int bounce = (int)(2.0f * sinf(now / 400.0f)); [span_68](start_span)//[span_68](end_span)
    int lookX  = 0;
    Expr e = expr;
    if (now < reactionEndMs) e = reactionHappy ? EX_HAPPY : EX_SURPRISED;
    if (e == EX_LOOK_L) lookX = -8; [span_69](start_span)//[span_69](end_span)
    if (e == EX_LOOK_R) lookX =  8;
    int eyeY = 18 + bounce; [span_70](start_span)//[span_70](end_span)
    int lx = 34 + lookX, rx = 74 + lookX; [span_71](start_span)// eye left edges[span_71](end_span)
    int eyeW = 20, eyeH = 26; [span_72](start_span)//[span_72](end_span)
    /* Blink squeezes eye height toward a thin line */
    if (blinking) {
      float ph = (now - blinkStartMs) / 180.0f; [span_73](start_span)// 0..1[span_73](end_span)
      float k  = 1.0f - sinf(ph * PI);          [span_74](start_span)// 1->0->1[span_74](end_span)
      eyeH = max(3, (int)(26 * k)); [span_75](start_span)//[span_75](end_span)
      eyeY = 18 + bounce + (26 - eyeH) / 2;
    [span_76](start_span)} //[span_76](end_span)

    if (e == EX_HAPPY) {
      /* "^ ^" happy eyes: thick upward arcs */
      for (int t = 0; t < 3; t++) {
        o.drawLine(lx, eyeY + 16, lx + 10, eyeY + 6 + t, SSD1306_WHITE); [span_77](start_span)//[span_77](end_span)
        o.drawLine(lx + 10, eyeY + 6 + t, lx + 20, eyeY + 16, SSD1306_WHITE); [span_78](start_span)//[span_78](end_span)
        o.drawLine(rx, eyeY + 16, rx + 10, eyeY + 6 + t, SSD1306_WHITE); [span_79](start_span)//[span_79](end_span)
        o.drawLine(rx + 10, eyeY + 6 + t, rx + 20, eyeY + 16, SSD1306_WHITE); [span_80](start_span)//[span_80](end_span)
      }
      /* Small open smile */
      o.fillCircle(64 + lookX, 50 + bounce, 4, SSD1306_WHITE);
      o.fillRect(58 + lookX, 44 + bounce, 13, 6, SSD1306_BLACK); [span_81](start_span)//[span_81](end_span)
    } else if (e == EX_SURPRISED) {
      /* Round wide eyes + "o" mouth */
      o.fillCircle(lx + 10, eyeY + 13, 11, SSD1306_WHITE); [span_82](start_span)//[span_82](end_span)
      o.fillCircle(rx + 10, eyeY + 13, 11, SSD1306_WHITE);
      o.drawCircle(64 + lookX, 52 + bounce, 4, SSD1306_WHITE); [span_83](start_span)//[span_83](end_span)
    } else {
      /* Classic rounded-rect mochi eyes */
      o.fillRoundRect(lx, eyeY, eyeW, eyeH, 8, SSD1306_WHITE); [span_84](start_span)//[span_84](end_span)
      o.fillRoundRect(rx, eyeY, eyeW, eyeH, 8, SSD1306_WHITE);
      /* Tiny flat mouth */
      o.fillRoundRect(60 + lookX, 52 + bounce, 8, 2, 1, SSD1306_WHITE); [span_85](start_span)//[span_85](end_span)
    }
    o.display();
  }
};

/* ============================ WatchApp ============================== */
class WatchApp {
  DisplayManager* dm;
  TimeManager* tm;
  BatteryManager* bm;
  int lastSecond = -1; [span_86](start_span)//[span_86](end_span)
public:
  void begin(DisplayManager* d, TimeManager* t, BatteryManager* b) {
    dm = d; tm = t; bm = b; [span_87](start_span)//[span_87](end_span)
  }
  void onEnter() { lastSecond = -1; [span_88](start_span)}          // force immediate redraw[span_88](end_span)

  void update() {
    struct tm t;
    tm->getLocal(t); [span_89](start_span)//[span_89](end_span)
    if (t.tm_sec == lastSecond) return;        // redraw only once per second
    lastSecond = t.tm_sec;
    draw(t);
  [span_90](start_span)} //[span_90](end_span)

  void draw(struct tm& t) {
    Adafruit_SH1106G& o = dm->oled; [span_91](start_span)// Internal display reference cast to SH1106G[span_91](end_span)
    o.clearDisplay();
    char buf[24]; [span_92](start_span)//[span_92](end_span)

    [span_93](start_span)/* NOTE: Battery icon and tracking percentage layout logic has been entirely removed from this view[span_93](end_span) */

    /* --- Big HH:MM, centered --- */
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    dm->centerText(buf, 15, 3); [span_94](start_span)// Shifted slightly higher for better empty space balance[span_94](end_span)

    /* --- Seconds, small, under the time --- */
    snprintf(buf, sizeof(buf), ":%02d", t.tm_sec);
    o.setTextSize(1); [span_95](start_span)//[span_95](end_span)
    o.setCursor(102, 33); // Repositioned relatively adjacent to the primary time cluster
    o.print(buf);

    /* --- Date DD MMM YYYY --- */
    strftime(buf, sizeof(buf), "%d %b %Y", &t);
    dm->centerText(buf, 48, 1); [span_96](start_span)//[span_96](end_span)

    o.display();
  }
};

/* ============================ FlappyApp ============================= */
class FlappyApp {
  DisplayManager* dm;
  Preferences prefs;
[span_97](start_span)public: //[span_97](end_span)
  enum GState { G_START, G_PLAYING, G_OVER };
  GState gstate = G_START;
[span_98](start_span)private: //[span_98](end_span)
  /* Bird */
  float birdY = 32, birdV = 0;
  static constexpr int BIRD_X = 24, BIRD_R = 3; [span_99](start_span)//[span_99](end_span)
  /* Physics (per 33 ms frame) */
  static constexpr float GRAVITY = 0.28f, FLAP_V = -2.9f;
  [span_100](start_span)/* Pipes */ //[span_100](end_span)
  static constexpr int NPIPES = 3, PIPE_W = 12, GAP_H = 26, PIPE_SPACING = 52; [span_101](start_span)//[span_101](end_span)
  int pipeX[NPIPES], gapY[NPIPES];
  bool passed[NPIPES];
  int score = 0, best = 0;
  unsigned long lastFrameMs = 0;
  bool dirty = true;                           [span_102](start_span)// static screens draw once[span_102](end_span)
public:
  void begin(DisplayManager* d) {
    dm = d;
    prefs.begin("flappy", false); [span_103](start_span)//[span_103](end_span)
    best = prefs.getInt("best", 0);
  }
  void onEnter() { gstate = G_START; dirty = true; [span_104](start_span)} //[span_104](end_span)
  bool isPlaying() const { return gstate == G_PLAYING; [span_105](start_span)} //[span_105](end_span)

  void onSingleTap() {
    if (gstate == G_START)        startGame();
    else if (gstate == G_PLAYING) birdV = FLAP_V; [span_106](start_span)//[span_106](end_span)
    else                          startGame(); [span_107](start_span)// Game Over -> restart[span_107](end_span)
  }

  void startGame() {
    birdY = 32; birdV = 0; score = 0; [span_108](start_span)//[span_108](end_span)
    for (int i = 0; i < NPIPES; i++) {
      pipeX[i] = 128 + i * PIPE_SPACING;
      gapY[i]  = random(10, 64 - 10 - GAP_H); [span_109](start_span)//[span_109](end_span)
      passed[i] = false;
    }
    gstate = G_PLAYING;
  [span_110](start_span)} //[span_110](end_span)

  void update() {
    unsigned long now = millis();
    if (now - lastFrameMs < 33) return;        [span_111](start_span)// ~30 fps[span_111](end_span)
    lastFrameMs = now;

    if (gstate == G_START)   { if (dirty) { drawStart(); dirty = false; } return; [span_112](start_span)} //[span_112](end_span)
    if (gstate == G_OVER)    { if (dirty) { drawGameOver(); dirty = false; } return; [span_113](start_span)} //[span_113](end_span)

    /* ---- Physics ---- */
    birdV += GRAVITY;
    birdY += birdV; [span_114](start_span)//[span_114](end_span)

    /* ---- Pipes ---- */
    for (int i = 0; i < NPIPES; i++) {
      pipeX[i] -= 2;
      [span_115](start_span)if (pipeX[i] + PIPE_W < 0) {             // recycle off-screen pipe[span_115](end_span)
        pipeX[i] = pipeX[(i + NPIPES - 1) % NPIPES] + PIPE_SPACING;
        gapY[i]  = random(10, 64 - 10 - GAP_H); [span_116](start_span)//[span_116](end_span)
        passed[i] = false;
      [span_117](start_span)} //[span_117](end_span)
      if (!passed[i] && pipeX[i] + PIPE_W < BIRD_X - BIRD_R) {
        passed[i] = true;
        score++; [span_118](start_span)//[span_118](end_span)
      }
    }

    /* ---- Collisions ---- */
    bool dead = (birdY - BIRD_R <= 0) || (birdY + BIRD_R >= 63); [span_119](start_span)//[span_119](end_span)
    for (int i = 0; i < NPIPES && !dead; i++) {
      bool inX = (BIRD_X + BIRD_R > pipeX[i]) && (BIRD_X - BIRD_R < pipeX[i] + PIPE_W); [span_120](start_span)//[span_120](end_span)
      bool inGap = (birdY - BIRD_R > gapY[i]) && (birdY + BIRD_R < gapY[i] + GAP_H); [span_121](start_span)//[span_121](end_span)
      if (inX && !inGap) dead = true;
    }
    if (dead) {
      if (score > best) { best = score; prefs.putInt("best", best); [span_122](start_span)} //[span_122](end_span)
      gstate = G_OVER; dirty = true;
      return;
    }
    drawGame();
  [span_123](start_span)} //[span_123](end_span)

  void drawStart() {
    Adafruit_SH1106G& o = dm->oled; [span_124](start_span)// Internal display reference cast to SH1106G[span_124](end_span)
    o.clearDisplay();
    dm->centerText("FLAPPY BIRD", 10, 1);
    o.drawRoundRect(34, 28, 60, 16, 4, SSD1306_WHITE); [span_125](start_span)//[span_125](end_span)
    dm->centerText("START", 32, 1);
    dm->centerText("tap to play", 54, 1);
    o.display();
  [span_126](start_span)} //[span_126](end_span)

  void drawGame() {
    Adafruit_SH1106G& o = dm->oled; [span_127](start_span)// Internal display reference cast to SH1106G[span_127](end_span)
    o.clearDisplay(); [span_128](start_span)//[span_128](end_span)
    for (int i = 0; i < NPIPES; i++) {         // pipes
      o.fillRect(pipeX[i], 0, PIPE_W, gapY[i], SSD1306_WHITE);
      o.fillRect(pipeX[i], gapY[i] + GAP_H, PIPE_W, 64 - gapY[i] - GAP_H, SSD1306_WHITE); [span_129](start_span)//[span_129](end_span)
    }
    o.drawFastHLine(0, 63, 128, SSD1306_WHITE); [span_130](start_span)// ground[span_130](end_span)
    o.fillCircle(BIRD_X, (int)birdY, BIRD_R, SSD1306_WHITE); // bird
    o.drawPixel(BIRD_X + 2, (int)birdY - 1, SSD1306_BLACK); [span_131](start_span)// eye[span_131](end_span)
    char buf[8];                                            // score
    snprintf(buf, sizeof(buf), "%d", score);
    dm->centerText(buf, 0, 1);
    o.display();
  [span_132](start_span)} //[span_132](end_span)

  void drawGameOver() {
    Adafruit_SH1106G& o = dm->oled; [span_133](start_span)// Internal display reference cast to SH1106G[span_133](end_span)
    o.clearDisplay();
    dm->centerText("GAME OVER", 8, 1);
    char buf[20];
    snprintf(buf, sizeof(buf), "Score: %d", score); [span_134](start_span)//[span_134](end_span)
    dm->centerText(buf, 26, 1);
    snprintf(buf, sizeof(buf), "Best:  %d", best);
    dm->centerText("tap to retry", 54, 1); [span_135](start_span)//[span_135](end_span)
    o.display();
  }
};

/* ============================ UpdateApp ============================= */
class UpdateApp {
  DisplayManager* dm; [span_136](start_span)//[span_136](end_span)
  enum OtaState { OTA_IDLE, OTA_CONNECTING, OTA_CHECKING, OTA_DOWNLOADING, OTA_FAIL, OTA_UP_TO_DATE };
  OtaState state = OTA_IDLE;
  unsigned long statusTimer = 0;
  String errorMsg = ""; [span_137](start_span)//[span_137](end_span)

public:
  void begin(DisplayManager* d) { dm = d; [span_138](start_span)} //[span_138](end_span)
  void onEnter() {
    state = OTA_CONNECTING;
    statusTimer = millis();
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  [span_139](start_span)} //[span_139](end_span)

  void update() {
    Adafruit_SH1106G& o = dm->oled;
    unsigned long now = millis();
    [span_140](start_span)switch (state) { //[span_140](end_span)
      case OTA_CONNECTING:
        o.clearDisplay(); [span_141](start_span)//[span_141](end_span)
        dm->centerText("OTA UPDATE", 5, 1);
        dm->centerText("Connecting Wi-Fi", 25, 1);
        o.fillRect(14, 45, 100, 4, SSD1306_BLACK);
        o.setCursor(60, 42);
        o.print((now / 500) % 2 == 0 ? "..." : " "); [span_142](start_span)//[span_142](end_span)
        o.display();
        [span_143](start_span)if (WiFi.status() == WL_CONNECTED) { //[span_143](end_span)
          state = OTA_CHECKING;
        [span_144](start_span)} else if (now - statusTimer > 20000) { //[span_144](end_span)
          fail("Wi-Fi Timeout");
        }
        break;

      case OTA_CHECKING:
        o.clearDisplay(); [span_145](start_span)//[span_145](end_span)
        dm->centerText("OTA UPDATE", 5, 1);
        dm->centerText("Checking GitHub...", 30, 1);
        o.display();
        runCheck();
        break; [span_146](start_span)//[span_146](end_span)
      [span_147](start_span)case OTA_DOWNLOADING: //[span_147](end_span)
        break;

      case OTA_FAIL:
        o.clearDisplay(); [span_148](start_span)//[span_148](end_span)
        dm->centerText("UPDATE FAILED", 5, 1);
        dm->centerText(errorMsg.c_str(), 28, 1);
        dm->centerText("Dbl Tap to Exit", 50, 1);
        o.display();
        break; [span_149](start_span)//[span_149](end_span)
      case OTA_UP_TO_DATE:
        o.clearDisplay();
        dm->centerText("SYSTEM OK", 10, 1);
        dm->centerText("No New Version", 30, 1);
        dm->centerText("Dbl Tap to Exit", 50, 1); [span_150](start_span)//[span_150](end_span)
        o.display();
        break;
    }
  }

private:
  void fail(String msg) {
    errorMsg = msg;
    state = OTA_FAIL; [span_151](start_span)//[span_151](end_span)
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  void runCheck() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http; [span_152](start_span)//[span_152](end_span)
    if (http.begin(client, VERSION_URL)) {
      int httpCode = http.GET(); [span_153](start_span)//[span_153](end_span)
      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        payload.trim(); [span_154](start_span)//[span_154](end_span)
        int newVersion = payload.toInt();

        if (newVersion > CURRENT_VERSION) {
          state = OTA_DOWNLOADING;
          executeUpdate(client); [span_155](start_span)//[span_155](end_span)
        } else {
          state = OTA_UP_TO_DATE;
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
        [span_156](start_span)} //[span_156](end_span)
      } else {
        fail("HTTP Err: " + String(httpCode)); [span_157](start_span)//[span_157](end_span)
      }
      http.end();
    } else {
      fail("Connection Fail"); [span_158](start_span)//[span_158](end_span)
    }
  }

  void executeUpdate(WiFiClientSecure& client) {
    HTTPClient http;
    Adafruit_SH1106G& o = dm->oled;

    o.clearDisplay();
    dm->centerText("DOWNLOADING...", 25, 1); [span_159](start_span)//[span_159](end_span)
    o.display();

    if (http.begin(client, BIN_URL)) {
      int httpCode = http.GET(); [span_160](start_span)//[span_160](end_span)
      if (httpCode == HTTP_CODE_OK) {
        int contentLength = http.getSize(); [span_161](start_span)//[span_161](end_span)
        if (Update.begin(contentLength)) {
          WiFiClient* stream = http.getStreamPtr();
          size_t written = 0;
          uint8_t buff[256]; [span_162](start_span)//[span_162](end_span)
          
          while (http.connected() && written < contentLength) {
            size_t size = stream->available();
            [span_163](start_span)if (size) { //[span_163](end_span)
              int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size)); [span_164](start_span)//[span_164](end_span)
              Update.write(buff, c);
              written += c;
              
              o.clearDisplay();
              dm->centerText("FLASHING UPDATE", 5, 1);
              int progressWidth = map(written, 0, contentLength, 0, 100);
              o.drawRect(14, 28, 100, 10, SSD1306_WHITE); [span_165](start_span)//[span_165](end_span)
              o.fillRect(14, 28, progressWidth, 10, SSD1306_WHITE);
              o.display(); [span_166](start_span)//[span_166](end_span)
            }
          }

          if (Update.end() && Update.isFinished()) {
            o.clearDisplay();
            dm->centerText("SUCCESS!", 10, 1); [span_167](start_span)//[span_167](end_span)
            dm->centerText("Rebooting...", 35, 1);
            o.display();
            delay(2000);
            ESP.restart();
          } else {
            fail("Flash Error"); [span_168](start_span)//[span_168](end_span)
          }
        } else {
          fail("No Space"); [span_169](start_span)//[span_169](end_span)
        }
      } else {
        fail("Bin HTTP Err: " + String(httpCode)); [span_170](start_span)//[span_170](end_span)
      }
      http.end();
    } else {
      fail("Bin Link Fail"); [span_171](start_span)//[span_171](end_span)
    }
  }
};

/* ============================ Globals =============================== */
DisplayManager displayMgr;
TouchManager   touchMgr;
BatteryManager batteryMgr;
TimeManager    timeMgr; [span_172](start_span)//[span_172](end_span)
MochiApp       mochiApp;
WatchApp       watchApp; [span_173](start_span)//[span_173](end_span)
FlappyApp      flappyApp;
UpdateApp      updateApp;

AppMode mode = MODE_MOCHI;
unsigned long lastInteractionMs = 0; [span_174](start_span)//[span_174](end_span)

void enterMode(AppMode m) {
  mode = m;
  [span_175](start_span)switch (mode) { //[span_175](end_span)
    case MODE_MOCHI:  mochiApp.onEnter();  break;
    case MODE_WATCH:  watchApp.onEnter();  break;
    case MODE_FLAPPY: flappyApp.onEnter(); break;
    case MODE_UPDATE: updateApp.onEnter(); break; [span_176](start_span)//[span_176](end_span)
  }
}

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());
  [span_177](start_span)if (!displayMgr.begin()) { //[span_177](end_span)
    Serial.println("SH1106 init failed - check wiring/address (0x3C)");
  }
  touchMgr.begin();
  batteryMgr.begin();
  timeMgr.begin(); [span_178](start_span)// non-blocking WiFi/NTP[span_178](end_span)

  mochiApp.begin(&displayMgr);
  watchApp.begin(&displayMgr, &timeMgr, &batteryMgr);
  flappyApp.begin(&displayMgr);
  updateApp.begin(&displayMgr);

  lastInteractionMs = millis();
  enterMode(MODE_MOCHI);
[span_179](start_span)} //[span_179](end_span)

void loop() {
  unsigned long now = millis();

  /* Background services */
  timeMgr.update();
  batteryMgr.update(); [span_180](start_span)//[span_180](end_span)

  /* Touch events */
  TapEvent ev = touchMgr.update();
  if (ev != TAP_NONE) lastInteractionMs = now; [span_181](start_span)//[span_181](end_span)
  if (ev == TAP_DOUBLE) {
    /* Circular: Mochi -> Watch -> Flappy -> Update -> Mochi */
    enterMode(mode == MODE_MOCHI  ? MODE_WATCH :
              mode == MODE_WATCH  ? MODE_FLAPPY :
              mode == MODE_FLAPPY ? MODE_UPDATE : MODE_MOCHI);
  [span_182](start_span)} else if (ev == TAP_SINGLE) { //[span_182](end_span)
    switch (mode) {
      case MODE_MOCHI:  mochiApp.onSingleTap(); break; [span_183](start_span)//[span_183](end_span)
      case MODE_WATCH:  /* no single-tap action */ break;
      case MODE_FLAPPY: flappyApp.onSingleTap(); break;
      case MODE_UPDATE: /* No action */ break; [span_184](start_span)//[span_184](end_span)
    }
  }

  /* Idle timeout: Watch & Flappy (not while game is running) */
  if (mode == MODE_FLAPPY && flappyApp.isPlaying()) {
    lastInteractionMs = now; [span_185](start_span)// pause idle timer in-game[span_185](end_span)
  }
  if (mode == MODE_UPDATE) {
    lastInteractionMs = now; [span_186](start_span)// Don't time out while updating[span_186](end_span)
  }
  if (mode != MODE_MOCHI && (now - lastInteractionMs) >= IDLE_TIMEOUT) {
    lastInteractionMs = now;
    enterMode(MODE_MOCHI); [span_187](start_span)//[span_187](end_span)
  }

  /* Run active mode */
  switch (mode) {
    case MODE_MOCHI:  mochiApp.update();  break;
    case MODE_WATCH:  watchApp.update();  break; [span_188](start_span)//[span_188](end_span)
    case MODE_FLAPPY: flappyApp.update(); break;
    case MODE_UPDATE: updateApp.update(); break;
  }
}

/**********************************************************************
 * MochiPod - ESP32 Handheld (Mochi / Watch / Flappy Bird / OTA Update)
 * Board   : ESP32 DevKit V1 (ESP-WROOM-32)
 * Display : SH1106 128x64 I2C    Touch: TTP223 (GPIO 4, active HIGH)
 * Core    : ESP32 Arduino core 2.x / 3.x
 *********************************************************************/
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>

#define SSD1306_WHITE SH110X_WHITE
#define SSD1306_BLACK SH110X_BLACK

/* ============================ USER CONFIG =========================== */
const char* WIFI_SSID      = "Afia Mahir";
const char* WIFI_PASS      = "afiamahir2026";
const long  GMT_OFFSET_SEC = 21600;
const int   DST_OFFSET_SEC = 0;
const char* NTP_SERVER     = "asia.pool.ntp.org";

const int   CURRENT_VERSION = 1;
const char* VERSION_URL     = "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/virsion.txt";
const char* BIN_URL         = "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/firmware.bin";

/* ------------------------------ Pins ------------------------------- */
#define PIN_OLED_SDA   21
#define PIN_OLED_SCL   22
#define PIN_TOUCH      4
#define PIN_VBAT       34

/* ------------------------- Battery calibration --------------------- */
const float VBAT_DIVIDER = 2.0f;
const float VBAT_CAL     = 1.00f;
const float VBAT_FULL    = 4.20f;
const float VBAT_EMPTY   = 3.30f;

/* --------------------------- Touch timing -------------------------- */
const unsigned long TOUCH_DEBOUNCE_MS    = 30;
const unsigned long DOUBLE_TAP_WINDOW_MS = 300;

/* ------------------------------ Idle ------------------------------- */
const unsigned long IDLE_TIMEOUT     = 20000;
const unsigned long NEGLECT_TIMEOUT  = 600000UL; // 10 minutes

/* ======================= Global Shared State ======================= */
// Cross-app communication flags
bool g_newHighScore      = false;  // Set by FlappyApp, read+cleared by MochiApp
int  g_happiness         = 75;     // 0-100, global Mochi mood
int  g_currentHour       = 12;     // Updated by TimeManager each loop

/* =========================== App Modes ============================= */
enum AppMode { MODE_MOCHI, MODE_WATCH, MODE_FLAPPY, MODE_UPDATE };
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
    int16_t w = strlen(s) * 6 * size;
    oled.setCursor((128 - w) / 2, y);
    oled.print(s);
  }
};

/* ========================= TouchManager ============================ */
class TouchManager {
  bool stableState = false, lastRead = false;
  unsigned long lastChangeMs = 0, firstTapMs = 0;
  bool waitingSecondTap = false;
public:
  void begin() { pinMode(PIN_TOUCH, INPUT); }

  TapEvent update() {
    unsigned long now = millis();
    TapEvent ev = TAP_NONE;
    bool raw = digitalRead(PIN_TOUCH) == HIGH;
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
  float voltage = 3.7f;
  int percent = 50;
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
  St state = CONNECTING;
  unsigned long startMs = 0;
  unsigned long lastRetryMs = 0;
  bool synced = false;
public:
  void begin() { startSync(); }

  void startSync() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    state = CONNECTING;
    startMs = millis();
  }

  void update() {
    unsigned long now = millis();
    if (state == CONNECTING) {
      if (WiFi.status() == WL_CONNECTED) {
        configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
        state = SYNCING;
        startMs = now;
      } else if (now - startMs > 30000) {
        shutdownWifi();
        lastRetryMs = now;
      }
    } else if (state == SYNCING) {
      if (time(nullptr) > 1700000000) {
        synced = true;
        shutdownWifi();
      } else if (now - startMs > 15000) {
        shutdownWifi();
        lastRetryMs = now;
      }
    } else if (state == DONE && !synced) {
      if (now - lastRetryMs > 60000) startSync();
    }

    // Update global hour every cycle
    if (synced) {
      struct tm t;
      getLocal(t);
      g_currentHour = t.tm_hour;
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
};

/* ============================ MochiApp ============================== */
class MochiApp {
  DisplayManager* dm;

  enum Activity {
    // Normal activities
    ACT_IDLE,
    ACT_WATCHING_PHONE,
    ACT_DRINKING_COFFEE,
    ACT_EATING,
    ACT_YAWNING,
    ACT_ROLLING_EYES,
    ACT_WHISTLING,
    ACT_POUTY,
    ACT_BLANK_STARE,
    ACT_BIG_GRIN,
    ACT_THINKING,
    ACT_STRETCHING,
    ACT_DANCING,
    ACT_WINKING,
    // Neglect / sad states
    ACT_SAD,
    ACT_CRYING,
    ACT_SLEEPING,
    ACT_LONELY,
    // Touch reactions
    ACT_TOUCHED_HAPPY,
    ACT_TOUCHED_BLUSH,
    ACT_TOUCHED_GIGGLE,
    // Special states
    ACT_CELEBRATING,   // triggered by new Flappy high score
    ACT_NIGHT_GROGGY,  // tapped during night sleep
  };

  Activity currentActivity   = ACT_IDLE;
  unsigned long activityStartMs  = 0;
  unsigned long nextActivityMs   = 0;
  unsigned long lastFrameMs      = 0;
  unsigned long lastTouchMs      = 0;

  bool blinking          = false;
  unsigned long nextBlinkMs  = 0;
  unsigned long blinkStartMs = 0;

  bool neglected         = false;
  bool touchReaction     = false;
  unsigned long touchReactionEndMs = 0;

  // Sub-animation state
  int  animFrame         = 0;
  unsigned long lastAnimFrameMs = 0;
  int  zCount            = 0;
  unsigned long lastZMs  = 0;
  float dancePhase       = 0.0f;
  int   tearY1           = 20;
  int   tearY2           = 20;

  // Night-groggy one-shot flag
  bool groggySent        = false;

  // Happiness decay timer
  unsigned long lastHappyDecayMs = 0;

public:
  void begin(DisplayManager* d) {
    dm = d;
    lastTouchMs        = millis();
    lastHappyDecayMs   = millis();
    nextBlinkMs        = millis() + random(2000, 5000);
    scheduleNextActivity(3000);
  }

  void onEnter() {
    // If returning from Flappy with a new high score, celebrate!
    if (g_newHighScore) {
      g_newHighScore = false;
      setActivity(ACT_CELEBRATING);
      touchReactionEndMs = millis() + 3000;
      touchReaction = true;
    } else {
      currentActivity = ACT_IDLE;
      neglected = false;
      touchReaction = false;
    }
    scheduleNextActivity(2000);
  }

  void onSingleTap() {
    unsigned long now = millis();

    // Boost happiness on touch
    g_happiness = constrain(g_happiness + 8, 0, 100);
    lastTouchMs = now;
    neglected   = false;

    // If it's night and Mochi is sleeping, show groggy instead
    bool isNight = (g_currentHour >= 22 || g_currentHour < 6);
    if (isNight && (currentActivity == ACT_SLEEPING || neglected)) {
      setActivity(ACT_NIGHT_GROGGY);
      touchReaction     = true;
      touchReactionEndMs = now + 2500;
      return;
    }

    // Normal touch reaction
    int r = random(0, 3);
    if (r == 0)      setActivity(ACT_TOUCHED_HAPPY);
    else if (r == 1) setActivity(ACT_TOUCHED_BLUSH);
    else             setActivity(ACT_TOUCHED_GIGGLE);

    touchReaction     = true;
    touchReactionEndMs = now + 2000;
  }

  void update() {
    unsigned long now = millis();
    if (now - lastFrameMs < 33) return;
    lastFrameMs = now;

    // ---- Happiness decay (1 point every 20s of no touch) ----
    if (now - lastHappyDecayMs >= 20000) {
      lastHappyDecayMs = now;
      g_happiness = constrain(g_happiness - 1, 0, 100);
    }

    // ---- Night-time auto-sleep ----
    bool isNight = (g_currentHour >= 22 || g_currentHour < 6);
    if (isNight && !touchReaction) {
      if (currentActivity != ACT_SLEEPING) {
        setActivity(ACT_SLEEPING);
        neglected = false;
      }
    } else {
      // ---- Neglect detection ----
      if (!neglected && (now - lastTouchMs) > NEGLECT_TIMEOUT) {
        neglected = true;
        int r = random(0, 4);
        Activity neglectStates[] = { ACT_SAD, ACT_CRYING, ACT_SLEEPING, ACT_LONELY };
        setActivity(neglectStates[r]);
      }

      // ---- Touch reaction timeout ----
      if (touchReaction && now > touchReactionEndMs) {
        touchReaction = false;
        neglected     = false;
        scheduleNextActivity(1000);
      }

      // ---- Autonomous activity scheduling ----
      if (!neglected && !touchReaction && now >= nextActivityMs) {
        setActivity(pickActivityByMood());
        scheduleNextActivity(random(4000, 9000));
      }
    }

    // ---- Blink logic ----
    bool eyesCanBlink = (currentActivity != ACT_SLEEPING &&
                         currentActivity != ACT_CRYING   &&
                         currentActivity != ACT_WINKING);
    if (!blinking && eyesCanBlink && now >= nextBlinkMs) {
      blinking = true;
      blinkStartMs = now;
    }
    if (blinking && now - blinkStartMs > 150) {
      blinking = false;
      nextBlinkMs = now + random(2000, 5000);
    }

    // ---- Advance animation sub-frames (~100ms each) ----
    if (now - lastAnimFrameMs > 100) {
      animFrame++;
      lastAnimFrameMs = now;
    }

    draw(now);
  }

private:
  /* ------------------------------------------------------------------ */
  void setActivity(Activity a) {
    currentActivity  = a;
    activityStartMs  = millis();
    animFrame        = 0;
    zCount           = 0;
    dancePhase       = 0.0f;
  }

  void scheduleNextActivity(unsigned long delayMs) {
    nextActivityMs = millis() + delayMs;
  }

  /* ---- Mood-aware activity picker ---------------------------------- */
  Activity pickActivityByMood() {
    int h = g_happiness;

    if (h < 25) {
      // Very sad: only sad states
      Activity pool[] = { ACT_SAD, ACT_CRYING, ACT_LONELY, ACT_BLANK_STARE, ACT_POUTY };
      return pool[random(0, 5)];
    } else if (h < 50) {
      // Low mood: mostly neutral/sad
      Activity pool[] = {
        ACT_IDLE, ACT_BLANK_STARE, ACT_POUTY, ACT_THINKING,
        ACT_SAD, ACT_LONELY, ACT_YAWNING, ACT_ROLLING_EYES
      };
      return pool[random(0, 8)];
    } else if (h < 75) {
      // Medium mood: balanced activities
      Activity pool[] = {
        ACT_IDLE, ACT_IDLE, ACT_WATCHING_PHONE, ACT_DRINKING_COFFEE,
        ACT_EATING, ACT_THINKING, ACT_STRETCHING, ACT_YAWNING,
        ACT_WHISTLING, ACT_ROLLING_EYES
      };
      return pool[random(0, 10)];
    } else {
      // High happiness: full range including celebrations
      Activity pool[] = {
        ACT_IDLE, ACT_WATCHING_PHONE, ACT_DRINKING_COFFEE,
        ACT_EATING, ACT_YAWNING, ACT_WHISTLING,
        ACT_BIG_GRIN, ACT_THINKING, ACT_STRETCHING,
        ACT_DANCING, ACT_WINKING, ACT_BIG_GRIN
      };
      return pool[random(0, 12)];
    }
  }

  /* ---- Eye helper -------------------------------------------------- */
  void drawEye(Adafruit_SH1106G& o, int x, int y, int w, int h,
               int pdx, int pdy, bool closed) {
    if (closed) {
      o.drawFastHLine(x, y + h / 2,     w, SSD1306_WHITE);
      o.drawFastHLine(x, y + h / 2 + 1, w, SSD1306_WHITE);
      return;
    }
    o.fillRoundRect(x, y, w, h, 4, SSD1306_WHITE);
    int px = constrain(x + w / 2 + pdx - 2, x + 1, x + w - 4);
    int py = constrain(y + h / 2 + pdy - 2, y + 1, y + h - 4);
    o.fillRect(px, py, 4, 4, SSD1306_BLACK);
  }

  /* ---- Main draw --------------------------------------------------- */
  void draw(unsigned long now) {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();

    int bounce = (int)(1.5f * sinf(now / 500.0f));
    int cx     = 64;
    int baseY  = 10 + bounce;

    int lEyeX = 36, rEyeX = 72;
    int eyeY  = baseY + 8;
    int eyeW  = 18, eyeH = 18;

    switch (currentActivity) {

      /* ---------- IDLE ---------- */
      case ACT_IDLE: {
        drawEye(o, lEyeX, eyeY, eyeW, eyeH, 0, 0, blinking);
        drawEye(o, rEyeX, eyeY, eyeW, eyeH, 0, 0, blinking);
        o.drawLine(54, 42 + bounce, 58, 46 + bounce, SSD1306_WHITE);
        o.drawFastHLine(58, 46 + bounce, 10, SSD1306_WHITE);
        o.drawLine(68, 46 + bounce, 72, 42 + bounce, SSD1306_WHITE);
        break;
      }

      /* ---------- WATCHING PHONE ---------- */
      case ACT_WATCHING_PHONE: {
        drawEye(o, lEyeX, eyeY + 4, eyeW, eyeH - 4, 0, 3, false);
        drawEye(o, rEyeX, eyeY + 4, eyeW, eyeH - 4, 0, 3, false);
        int ph = (animFrame % 2 == 0) ? 1 : 0;
        o.drawRect(44, 46 + bounce, 40, 14, SSD1306_WHITE);
        o.fillRect(46, 48 + bounce, 36, 10, ph ? SSD1306_WHITE : SSD1306_BLACK);
        if (!ph) {
          o.drawFastHLine(48, 50 + bounce, 20, SSD1306_WHITE);
          o.drawFastHLine(48, 53 + bounce, 14, SSD1306_WHITE);
        }
        o.drawFastHLine(58, 44 + bounce, 12, SSD1306_WHITE);
        break;
      }

      /* ---------- DRINKING COFFEE ---------- */
      case ACT_DRINKING_COFFEE: {
        drawEye(o, lEyeX, eyeY + 5, eyeW, eyeH / 2, 0, 0, false);
        drawEye(o, rEyeX, eyeY + 5, eyeW, eyeH / 2, 0, 0, false);
        o.drawLine(55, 41 + bounce, 59, 44 + bounce, SSD1306_WHITE);
        o.drawFastHLine(59, 44 + bounce, 10, SSD1306_WHITE);
        o.drawLine(69, 44 + bounce, 73, 41 + bounce, SSD1306_WHITE);
        o.drawRect(52, 48 + bounce, 24, 14, SSD1306_WHITE);
        o.drawFastHLine(50, 48 + bounce, 28, SSD1306_WHITE);
        o.drawLine(76, 50 + bounce, 80, 50 + bounce, SSD1306_WHITE);
        o.drawLine(80, 50 + bounce, 80, 58 + bounce, SSD1306_WHITE);
        o.drawLine(76, 58 + bounce, 80, 58 + bounce, SSD1306_WHITE);
        if (animFrame % 4 < 2) {
          o.drawPixel(58, 45 + bounce, SSD1306_WHITE);
          o.drawPixel(64, 44 + bounce, SSD1306_WHITE);
          o.drawPixel(70, 45 + bounce, SSD1306_WHITE);
        }
        break;
      }

      /* ---------- EATING ---------- */
      case ACT_EATING: {
        int phase = (animFrame / 2) % 3;
        drawEye(o, lEyeX, eyeY, eyeW, eyeH, 0, 0, blinking);
        drawEye(o, rEyeX, eyeY, eyeW, eyeH, 0, 0, blinking);
        if (phase == 0) {
          o.fillRoundRect(52, 40 + bounce, 24, 6, 3, SSD1306_WHITE);
          o.fillCircle(78, 43 + bounce, 4, SSD1306_WHITE);
        } else if (phase == 1) {
          o.fillRoundRect(52, 40 + bounce, 24, 8, 3, SSD1306_WHITE);
          o.fillRect(54, 42 + bounce, 20, 4, SSD1306_BLACK);
        } else {
          o.fillRoundRect(52, 38 + bounce, 24, 12, 3, SSD1306_WHITE);
          o.fillRect(54, 40 + bounce, 20, 7, SSD1306_BLACK);
          o.fillRect(58, 28 + bounce, 8, 8, SSD1306_WHITE);
        }
        o.drawCircle(30, 38 + bounce, 4, SSD1306_WHITE);
        o.drawCircle(98, 38 + bounce, 4, SSD1306_WHITE);
        break;
      }

      /* ---------- YAWNING ---------- */
      case ACT_YAWNING: {
        int phase = (animFrame / 3) % 6;
        bool eyesClosed = (phase > 2);
        drawEye(o, lEyeX, eyeY + (eyesClosed ? 4 : 0), eyeW,
                eyesClosed ? eyeH - 4 : eyeH, 0, 0, eyesClosed);
        drawEye(o, rEyeX, eyeY + (eyesClosed ? 4 : 0), eyeW,
                eyesClosed ? eyeH - 4 : eyeH, 0, 0, eyesClosed);
        int mOpen = (phase < 3) ? phase * 4 : (5 - phase) * 4;
        mOpen = constrain(mOpen, 2, 16);
        o.fillRoundRect(52, 42 + bounce, 24, mOpen, 4, SSD1306_WHITE);
        if (mOpen > 8) o.fillRoundRect(54, 44 + bounce, 20, mOpen - 4, 3, SSD1306_BLACK);
        if (phase >= 2 && phase <= 4)
          o.fillRoundRect(46, 44 + bounce, 36, 8, 4, SSD1306_WHITE);
        break;
      }

      /* ---------- ROLLING EYES ---------- */
      case ACT_ROLLING_EYES: {
        float angle = animFrame * 0.4f;
        int pdx = (int)(5 * cosf(angle));
        int pdy = (int)(5 * sinf(angle));
        drawEye(o, lEyeX, eyeY, eyeW, eyeH, pdx, pdy, false);
        drawEye(o, rEyeX, eyeY, eyeW, eyeH, pdx, pdy, false);
        o.drawFastHLine(55, 43 + bounce, 18, SSD1306_WHITE);
        o.drawFastHLine(55, 44 + bounce, 18, SSD1306_WHITE);
        o.fillTriangle(88, 20 + bounce, 92, 20 + bounce, 90, 26 + bounce, SSD1306_WHITE);
        break;
      }

      /* ---------- WHISTLING ---------- */
      case ACT_WHISTLING: {
        drawEye(o, lEyeX, eyeY, eyeW, eyeH - 4, 0, 0, blinking);
        drawEye(o, rEyeX, eyeY, eyeW, eyeH - 4, 0, 0, blinking);
        int msize = 5 + (animFrame % 3);
        o.drawCircle(cx, 44 + bounce, msize, SSD1306_WHITE);
        o.drawCircle(cx, 44 + bounce, msize - 1, SSD1306_WHITE);
        if (animFrame % 6 < 3) {
          o.setCursor(90, 15 + bounce);
          o.setTextSize(1);
          o.print("~");
          o.setCursor(100, 22 + bounce);
          o.print("~");
        }
        break;
      }

      /* ---------- POUTY ---------- */
      case ACT_POUTY: {
        drawEye(o, lEyeX, eyeY + 6, eyeW, eyeH / 2 + 2, -3, 0, false);
        drawEye(o, rEyeX, eyeY + 6, eyeW, eyeH / 2 + 2, -3, 0, false);
        o.drawLine(52, 44 + bounce, 58, 46 + bounce, SSD1306_WHITE);
        o.drawFastHLine(58, 46 + bounce, 12, SSD1306_WHITE);
        o.drawLine(70, 46 + bounce, 76, 44 + bounce, SSD1306_WHITE);
        o.drawFastHLine(58, 47 + bounce, 12, SSD1306_WHITE);
        o.drawLine(36, eyeY - 4 + bounce, 54, eyeY - 6 + bounce, SSD1306_WHITE);
        o.drawLine(72, eyeY - 6 + bounce, 90, eyeY - 4 + bounce, SSD1306_WHITE);
        break;
      }

      /* ---------- BLANK STARE ---------- */
      case ACT_BLANK_STARE: {
        o.drawFastHLine(lEyeX, eyeY + eyeH / 2,     eyeW, SSD1306_WHITE);
        o.drawFastHLine(lEyeX, eyeY + eyeH / 2 + 1, eyeW, SSD1306_WHITE);
        o.drawFastHLine(rEyeX, eyeY + eyeH / 2,     eyeW, SSD1306_WHITE);
        o.drawFastHLine(rEyeX, eyeY + eyeH / 2 + 1, eyeW, SSD1306_WHITE);
        o.drawFastHLine(56, 43 + bounce, 16, SSD1306_WHITE);
        break;
      }

      /* ---------- BIG GRIN ---------- */
      case ACT_BIG_GRIN: {
        for (int t = 0; t < 3; t++) {
          o.drawLine(lEyeX, eyeY + eyeH, lEyeX + eyeW / 2, eyeY + t + 4, SSD1306_WHITE);
          o.drawLine(lEyeX + eyeW / 2, eyeY + t + 4, lEyeX + eyeW, eyeY + eyeH, SSD1306_WHITE);
          o.drawLine(rEyeX, eyeY + eyeH, rEyeX + eyeW / 2, eyeY + t + 4, SSD1306_WHITE);
          o.drawLine(rEyeX + eyeW / 2, eyeY + t + 4, rEyeX + eyeW, eyeY + eyeH, SSD1306_WHITE);
        }
        o.fillRoundRect(46, 38 + bounce, 36, 16, 5, SSD1306_WHITE);
        o.fillRect(48, 40 + bounce, 32, 8, SSD1306_BLACK);
        for (int t = 0; t < 4; t++)
          o.fillRect(49 + t * 8, 40 + bounce, 6, 5, SSD1306_WHITE);
        for (int d = 0; d < 3; d++) {
          o.drawCircle(28,  38 + bounce, d + 2, SSD1306_WHITE);
          o.drawCircle(100, 38 + bounce, d + 2, SSD1306_WHITE);
        }
        break;
      }

      /* ---------- THINKING ---------- */
      case ACT_THINKING: {
        drawEye(o, lEyeX, eyeY, eyeW, eyeH, 3, -3, blinking);
        drawEye(o, rEyeX, eyeY, eyeW, eyeH, 3, -3, blinking);
        o.drawLine(55, 43 + bounce, 58, 45 + bounce, SSD1306_WHITE);
        o.drawFastHLine(58, 45 + bounce, 12, SSD1306_WHITE);
        o.fillRoundRect(56, 48 + bounce, 20, 10, 3, SSD1306_WHITE);
        int tb = (animFrame / 3) % 4;
        for (int i = 0; i <= tb && i < 3; i++)
          o.fillCircle(85 + i * 8, 12 + bounce - i * 3, 2 + i, SSD1306_WHITE);
        break;
      }

      /* ---------- STRETCHING ---------- */
      case ACT_STRETCHING: {
        int phase = (animFrame / 4) % 4;
        drawEye(o, lEyeX, eyeY + 3, eyeW, eyeH - 6, 0, 0, true);
        drawEye(o, rEyeX, eyeY + 3, eyeW, eyeH - 6, 0, 0, true);
        int mw = 12 + phase * 4;
        o.drawFastHLine(cx - mw / 2, 44 + bounce, mw, SSD1306_WHITE);
        int armLen = constrain(10 + phase * 8, 10, 40);
        o.drawLine(lEyeX - 2, 36 + bounce, lEyeX - 2 - armLen, 30 + bounce, SSD1306_WHITE);
        o.drawLine(rEyeX + eyeW + 2, 36 + bounce, rEyeX + eyeW + 2 + armLen, 30 + bounce, SSD1306_WHITE);
        o.fillCircle(lEyeX - 2 - armLen, 30 + bounce, 3, SSD1306_WHITE);
        o.fillCircle(rEyeX + eyeW + 2 + armLen, 30 + bounce, 3, SSD1306_WHITE);
        break;
      }

      /* ---------- DANCING ---------- */
      case ACT_DANCING: {
        dancePhase += 0.3f;
        int db  = (int)(4 * sinf(dancePhase));
        int ley = eyeY + (db > 0 ? 0 : 2);
        int rey = eyeY + (db > 0 ? 2 : 0);
        drawEye(o, lEyeX + db / 2, ley, eyeW, eyeH, 0, 0, false);
        drawEye(o, rEyeX + db / 2, rey, eyeW, eyeH, 0, 0, false);
        o.drawLine(52, 43 + bounce + db / 2, 57, 47 + bounce + db / 2, SSD1306_WHITE);
        o.drawFastHLine(57, 47 + bounce + db / 2, 14, SSD1306_WHITE);
        o.drawLine(71, 47 + bounce + db / 2, 76, 43 + bounce + db / 2, SSD1306_WHITE);
        if ((int)(dancePhase * 2) % 4 < 2) {
          o.setCursor(14, 20);
          o.setTextSize(1);
          o.print("~");
          o.setCursor(106, 20);
          o.print("~");
        }
        break;
      }

      /* ---------- WINKING ---------- */
      case ACT_WINKING: {
        drawEye(o, lEyeX, eyeY, eyeW, eyeH, 0, 0, false);
        for (int i = 0; i < 3; i++) {
          o.drawLine(rEyeX,           eyeY + 12 + i, rEyeX + eyeW / 2, eyeY + 6 + i, SSD1306_WHITE);
          o.drawLine(rEyeX + eyeW / 2, eyeY + 6 + i, rEyeX + eyeW,     eyeY + 12 + i, SSD1306_WHITE);
        }
        o.drawLine(54, 42 + bounce, 58, 46 + bounce, SSD1306_WHITE);
        o.drawFastHLine(58, 46 + bounce, 12, SSD1306_WHITE);
        o.drawLine(70, 46 + bounce, 76, 42 + bounce, SSD1306_WHITE);
        break;
      }

      /* ---------- SAD ---------- */
      case ACT_SAD: {
        drawEye(o, lEyeX, eyeY + 4, eyeW, eyeH - 4, 0, 3, blinking);
        drawEye(o, rEyeX, eyeY + 4, eyeW, eyeH - 4, 0, 3, blinking);
        o.drawLine(lEyeX,           eyeY - 4 + bounce, lEyeX + eyeW, eyeY + bounce, SSD1306_WHITE);
        o.drawLine(rEyeX,           eyeY + bounce,     rEyeX + eyeW, eyeY - 4 + bounce, SSD1306_WHITE);
        o.drawLine(52, 46 + bounce, 57, 42 + bounce, SSD1306_WHITE);
        o.drawFastHLine(57, 42 + bounce, 14, SSD1306_WHITE);
        o.drawLine(71, 42 + bounce, 76, 46 + bounce, SSD1306_WHITE);
        dm->centerText("...", 56, 1);
        break;
      }

      /* ---------- CRYING ---------- */
      case ACT_CRYING: {
        for (int i = 0; i < 3; i++) {
          o.drawLine(lEyeX + i * 3, eyeY + 10, lEyeX + i * 3 + 2, eyeY + 7, SSD1306_WHITE);
          o.drawLine(rEyeX + i * 3, eyeY + 10, rEyeX + i * 3 + 2, eyeY + 7, SSD1306_WHITE);
        }
        o.drawFastHLine(lEyeX, eyeY + 10, eyeW, SSD1306_WHITE);
        o.drawFastHLine(rEyeX, eyeY + 10, eyeW, SSD1306_WHITE);
        o.drawLine(lEyeX,           eyeY - 2 + bounce, lEyeX + eyeW, eyeY + 2 + bounce, SSD1306_WHITE);
        o.drawLine(rEyeX,           eyeY + 2 + bounce, rEyeX + eyeW, eyeY - 2 + bounce, SSD1306_WHITE);
        int t1 = eyeY + 14 + (animFrame * 2) % 30;
        int t2 = eyeY + 14 + (animFrame * 2 + 15) % 30;
        o.fillRoundRect(lEyeX + 6, t1 + bounce, 4, 6, 2, SSD1306_WHITE);
        o.fillRoundRect(rEyeX + 6, t2 + bounce, 4, 6, 2, SSD1306_WHITE);
        o.drawLine(50, 48 + bounce, 56, 44 + bounce, SSD1306_WHITE);
        o.drawFastHLine(56, 44 + bounce, 16, SSD1306_WHITE);
        o.drawLine(72, 44 + bounce, 78, 48 + bounce, SSD1306_WHITE);
        if (animFrame % 6 < 3) dm->centerText("(T_T)", 56, 1);
        break;
      }

      /* ---------- SLEEPING ---------- */
      case ACT_SLEEPING: {
        // Curved closed eyes
        for (int i = 0; i < 3; i++) {
          o.drawLine(lEyeX + i * 3, eyeY + 9 + bounce, lEyeX + i * 3 + 2, eyeY + 6 + bounce, SSD1306_WHITE);
          o.drawLine(rEyeX + i * 3, eyeY + 9 + bounce, rEyeX + i * 3 + 2, eyeY + 6 + bounce, SSD1306_WHITE);
        }
        o.drawFastHLine(lEyeX, eyeY + 9 + bounce, eyeW, SSD1306_WHITE);
        o.drawFastHLine(rEyeX, eyeY + 9 + bounce, eyeW, SSD1306_WHITE);
        o.drawFastHLine(59, 43 + bounce, 10, SSD1306_WHITE);
        // ZZZ
        if (millis() - lastZMs > 800) { lastZMs = millis(); zCount = (zCount + 1) % 4; }
        for (int z = 0; z < zCount; z++) {
          o.setCursor(78 + z * 7, 20 - z * 7 + bounce);
          o.setTextSize(1);
          o.print("z");
        }
        break;
      }

      /* ---------- LONELY ---------- */
      case ACT_LONELY: {
        drawEye(o, lEyeX, eyeY + 3, eyeW, eyeH - 4, -4, 2, blinking);
        drawEye(o, rEyeX, eyeY + 3, eyeW, eyeH - 4, -4, 2, blinking);
        o.drawFastHLine(54, 44 + bounce, 20, SSD1306_WHITE);
        if (animFrame % 8 < 4) {
          o.drawLine(30, 30 + bounce, 20, 20 + bounce, SSD1306_WHITE);
          o.drawLine(20, 20 + bounce, 25, 18 + bounce, SSD1306_WHITE);
        }
        dm->centerText("...", 56, 1);
        break;
      }

      /* ---------- TOUCHED HAPPY ---------- */
      case ACT_TOUCHED_HAPPY: {
        for (int t = 0; t < 3; t++) {
          o.drawLine(lEyeX, eyeY + eyeH, lEyeX + eyeW / 2, eyeY + t + 3, SSD1306_WHITE);
          o.drawLine(lEyeX + eyeW / 2, eyeY + t + 3, lEyeX + eyeW, eyeY + eyeH, SSD1306_WHITE);
          o.drawLine(rEyeX, eyeY + eyeH, rEyeX + eyeW / 2, eyeY + t + 3, SSD1306_WHITE);
          o.drawLine(rEyeX + eyeW / 2, eyeY + t + 3, rEyeX + eyeW, eyeY + eyeH, SSD1306_WHITE);
        }
        o.drawLine(48, 43 + bounce, 54, 48 + bounce, SSD1306_WHITE);
        o.drawFastHLine(54, 48 + bounce, 20, SSD1306_WHITE);
        o.drawLine(74, 48 + bounce, 80, 43 + bounce, SSD1306_WHITE);
        if (animFrame % 8 < 4) {
          o.drawLine(20, 10, 24, 10, SSD1306_WHITE);
          o.drawLine(22, 8, 22, 12, SSD1306_WHITE);
          o.drawLine(100, 18, 104, 18, SSD1306_WHITE);
          o.drawLine(102, 16, 102, 20, SSD1306_WHITE);
        }
        dm->centerText("^_^", 54, 1);
        break;
      }

      /* ---------- TOUCHED BLUSH ---------- */
      case ACT_TOUCHED_BLUSH: {
        drawEye(o, lEyeX, eyeY + 2, eyeW, eyeH - 4, 0, 0, false);
        drawEye(o, rEyeX, eyeY + 2, eyeW, eyeH - 4, 0, 0, false);
        o.drawLine(56, 44 + bounce, 60, 46 + bounce, SSD1306_WHITE);
        o.drawFastHLine(60, 46 + bounce, 8, SSD1306_WHITE);
        o.drawLine(68, 46 + bounce, 72, 44 + bounce, SSD1306_WHITE);
        for (int r = 2; r <= 6; r += 2) {
          o.drawCircle(26,  38 + bounce, r, SSD1306_WHITE);
          o.drawCircle(102, 38 + bounce, r, SSD1306_WHITE);
        }
        if (animFrame % 6 < 3) dm->centerText(">////<", 56, 1);
        break;
      }

      /* ---------- TOUCHED GIGGLE ---------- */
      case ACT_TOUCHED_GIGGLE: {
        int gb = (animFrame % 2 == 0) ? 2 : -2;
        for (int t = 0; t < 3; t++) {
          o.drawLine(lEyeX,           eyeY + eyeH + gb, lEyeX + eyeW / 2, eyeY + t + 5 + gb, SSD1306_WHITE);
          o.drawLine(lEyeX + eyeW / 2, eyeY + t + 5 + gb, lEyeX + eyeW,   eyeY + eyeH + gb, SSD1306_WHITE);
          o.drawLine(rEyeX,           eyeY + eyeH + gb, rEyeX + eyeW / 2, eyeY + t + 5 + gb, SSD1306_WHITE);
          o.drawLine(rEyeX + eyeW / 2, eyeY + t + 5 + gb, rEyeX + eyeW,   eyeY + eyeH + gb, SSD1306_WHITE);
        }
        o.fillRoundRect(50, 39 + bounce + gb, 28, 14, 5, SSD1306_WHITE);
        o.fillRect(52,     41 + bounce + gb, 24, 8, SSD1306_BLACK);
        if (animFrame % 4 < 2) dm->centerText("hehe~", 55, 1);
        break;
      }

      /* ---------- CELEBRATING (new high score) ---------- */
      case ACT_CELEBRATING: {
        // Huge eyes, confetti dots, party text
        dancePhase += 0.5f;
        int db = (int)(5 * sinf(dancePhase));
        for (int t = 0; t < 3; t++) {
          o.drawLine(lEyeX, eyeY + eyeH + db, lEyeX + eyeW / 2, eyeY + t + 2 + db, SSD1306_WHITE);
          o.drawLine(lEyeX + eyeW / 2, eyeY + t + 2 + db, lEyeX + eyeW, eyeY + eyeH + db, SSD1306_WHITE);
          o.drawLine(rEyeX, eyeY + eyeH + db, rEyeX + eyeW / 2, eyeY + t + 2 + db, SSD1306_WHITE);
          o.drawLine(rEyeX + eyeW / 2, eyeY + t + 2 + db, rEyeX + eyeW, eyeY + eyeH + db, SSD1306_WHITE);
        }
        o.fillRoundRect(44, 38 + bounce + db, 40, 14, 5, SSD1306_WHITE);
        o.fillRect(46, 40 + bounce + db, 36, 8, SSD1306_BLACK);
        for (int t = 0; t < 4; t++)
          o.fillRect(47 + t * 9, 40 + bounce + db, 7, 5, SSD1306_WHITE);
        // Confetti
        for (int c = 0; c < 8; c++) {
          int cx2 = (c * 17 + animFrame * 3) % 128;
          int cy2 = (c * 11 + animFrame * 2) % 30;
          o.drawPixel(cx2, cy2, SSD1306_WHITE);
          o.fillRect(cx2, cy2, 2, 2, SSD1306_WHITE);
        }
        dm->centerText("NEW BEST!", 56, 1);
        break;
      }

      /* ---------- NIGHT GROGGY ---------- */
      case ACT_NIGHT_GROGGY: {
        // Half-open annoyed eyes, messy hair implied by jagged top
        drawEye(o, lEyeX, eyeY + 8, eyeW, eyeH / 2 - 2, 0, 0, false);
        drawEye(o, rEyeX, eyeY + 8, eyeW, eyeH / 2 - 2, 0, 0, false);
        // Heavy eyelids
        o.fillRect(lEyeX, eyeY + 8, eyeW, 5, SSD1306_BLACK);
        o.fillRect(rEyeX, eyeY + 8, eyeW, 5, SSD1306_BLACK);
        o.drawFastHLine(lEyeX, eyeY + 13, eyeW, SSD1306_WHITE);
        o.drawFastHLine(rEyeX, eyeY + 13, eyeW, SSD1306_WHITE);
        // Annoyed flat mouth
        o.drawFastHLine(55, 43 + bounce, 18, SSD1306_WHITE);
        // Angry brows
        o.drawLine(lEyeX,           eyeY + 4 + bounce, lEyeX + eyeW, eyeY + bounce, SSD1306_WHITE);
        o.drawLine(rEyeX,           eyeY + bounce,     rEyeX + eyeW, eyeY + 4 + bounce, SSD1306_WHITE);
        // "Zzz..." indicator
        o.setCursor(85, 8 + bounce);
        o.setTextSize(1);
        o.print("zzz");
        dm->centerText("...go away", 54, 1);
        break;
      }

      default: break;
    }

    o.display();
  }
};

/* ============================ WatchApp ============================== */
class WatchApp {
  DisplayManager* dm;
  TimeManager*    tm;
  int lastSecond = -1;
public:
  void begin(DisplayManager* d, TimeManager* t) { dm = d; tm = t; }
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
  DisplayManager* dm;
  Preferences prefs;

public:
  enum GState { G_START, G_READY, G_PLAYING, G_OVER };
  GState gstate = G_START;

private:
  /* Bird */
  float birdY  = 32.0f;
  float birdV  = 0.0f;
  static constexpr int   BIRD_X = 28;
  static constexpr int   BIRD_R = 4;

  /* Physics — tuned for playability */
  static constexpr float GRAVITY    = 0.20f;  // gentle fall
  static constexpr float FLAP_V     = -2.6f;  // comfortable flap lift
  static constexpr float MAX_FALL_V = 3.5f;   // terminal velocity cap

  /* Pipes */
  static constexpr int NPIPES       = 3;
  static constexpr int PIPE_W       = 10;
  static constexpr int GAP_H        = 30;     // generous gap
  static constexpr int PIPE_SPACING = 55;
  static constexpr int PIPE_SPEED   = 2;

  int  pipeX[NPIPES];
  int  gapY[NPIPES];
  bool passed[NPIPES];

  int  score    = 0;
  int  best     = 0;
  bool newBest  = false;

  unsigned long lastFrameMs = 0;
  bool dirty = true;

  /* Grace period: bird floats for 1.2s before gravity kicks in */
  bool          graceActive    = false;
  unsigned long graceStartMs   = 0;
  static constexpr unsigned long GRACE_MS = 1200;

  /* Pipe scroll-in: first pipe starts further right for breathing room */
  static constexpr int FIRST_PIPE_OFFSET = 80;

public:
  void begin(DisplayManager* d) {
    dm = d;
    prefs.begin("flappy", false);
    best = prefs.getInt("best", 0);
  }

  void onEnter() { gstate = G_START; dirty = true; newBest = false; }
  bool isPlaying() const { return gstate == G_PLAYING || gstate == G_READY; }

  void onSingleTap() {
    switch (gstate) {
      case G_START:
        initGame();
        gstate = G_READY;
        dirty  = false;
        break;
      case G_READY:
        // First tap launches bird and starts gravity
        gstate      = G_PLAYING;
        graceActive = false;
        birdV       = FLAP_V;
        break;
      case G_PLAYING:
        birdV = FLAP_V;
        break;
      case G_OVER:
        initGame();
        gstate = G_READY;
        dirty  = false;
        break;
    }
  }

  void update() {
    unsigned long now = millis();
    if (now - lastFrameMs < 33) return;   // ~30 fps
    lastFrameMs = now;

    switch (gstate) {
      case G_START:
        if (dirty) { drawStart(); dirty = false; }
        break;

      case G_READY:
        // Bird bobs in place, pipes are frozen, waiting for first tap
        birdY = 32.0f + 3.0f * sinf(now / 400.0f);
        drawReadyScreen(now);
        break;

      case G_PLAYING:
        tickPhysics();
        scrollPipes();
        if (checkCollision()) {
          handleDeath();
        } else {
          drawGame();
        }
        break;

      case G_OVER:
        if (dirty) { drawGameOver(); dirty = false; }
        break;
    }
  }

private:
  void initGame() {
    birdY      = 32.0f;
    birdV      = 0.0f;
    score      = 0;
    newBest    = false;
    graceActive = true;
    graceStartMs = millis();

    for (int i = 0; i < NPIPES; i++) {
      // Stagger pipes across screen with generous first-pipe offset
      pipeX[i]  = 128 + FIRST_PIPE_OFFSET + i * PIPE_SPACING;
      gapY[i]   = random(8, 64 - 8 - GAP_H);
      passed[i] = false;
    }
  }

  void tickPhysics() {
    // Grace period: no gravity, bird just drifts
    if (graceActive) {
      if (millis() - graceStartMs < GRACE_MS) {
        birdY = 32.0f + 2.0f * sinf(millis() / 300.0f);
        return;
      }
      graceActive = false;
    }
    birdV += GRAVITY;
    birdV  = constrain(birdV, FLAP_V * 1.2f, MAX_FALL_V);
    birdY += birdV;
    birdY  = constrain(birdY, (float)BIRD_R, 63.0f - BIRD_R);
  }

  void scrollPipes() {
    for (int i = 0; i < NPIPES; i++) {
      pipeX[i] -= PIPE_SPEED;

      // Recycle pipe that has fully left screen
      if (pipeX[i] + PIPE_W < 0) {
        // Place it after the rightmost active pipe
        int rightmost = pipeX[0];
        for (int j = 1; j < NPIPES; j++)
          if (pipeX[j] > rightmost) rightmost = pipeX[j];

        pipeX[i]  = rightmost + PIPE_SPACING;
        gapY[i]   = random(8, 64 - 8 - GAP_H);
        passed[i] = false;
      }

      // Score when bird clears a pipe
      if (!passed[i] && (pipeX[i] + PIPE_W) < (BIRD_X - BIRD_R)) {
        passed[i] = true;
        score++;
        // Small happiness boost for each pipe cleared
        g_happiness = constrain(g_happiness + 3, 0, 100);
      }
    }
  }

  bool checkCollision() {
    // Ceiling / floor
    if (birdY - BIRD_R <= 0 || birdY + BIRD_R >= 63) return true;

    // Pipe collision (AABB vs circle approximation)
    for (int i = 0; i < NPIPES; i++) {
      bool inX = (BIRD_X + BIRD_R - 1 > pipeX[i]) &&
                 (BIRD_X - BIRD_R + 1 < pipeX[i] + PIPE_W);
      if (!inX) continue;
      bool inGap = ((int)birdY - BIRD_R + 1 > gapY[i]) &&
                   ((int)birdY + BIRD_R - 1 < gapY[i] + GAP_H);
      if (!inGap) return true;
    }
    return false;
  }

  void handleDeath() {
    if (score > best) {
      best    = score;
      newBest = true;
      prefs.putInt("best", best);
      g_newHighScore = true;   // signal MochiApp
      g_happiness    = constrain(g_happiness + 20, 0, 100);
    }
    gstate = G_OVER;
    dirty  = true;
  }

  /* ---- Draw helpers ---- */
  void drawBird() {
    Adafruit_SH1106G& o = dm->oled;
    int by = (int)birdY;
    o.fillCircle(BIRD_X, by, BIRD_R, SSD1306_WHITE);
    // Eye
    o.drawPixel(BIRD_X + 2, by - 1, SSD1306_BLACK);
    // Wing flap
    if (birdV < -0.5f) {
      o.drawLine(BIRD_X - 3, by,     BIRD_X - 6, by - 3, SSD1306_WHITE);
      o.drawLine(BIRD_X - 6, by - 3, BIRD_X - 3, by - 1, SSD1306_WHITE);
    } else {
      o.drawLine(BIRD_X - 3, by, BIRD_X - 6, by + 2, SSD1306_WHITE);
      o.drawLine(BIRD_X - 6, by + 2, BIRD_X - 3, by + 1, SSD1306_WHITE);
    }
  }

  void drawPipes() {
    Adafruit_SH1106G& o = dm->oled;
    for (int i = 0; i < NPIPES; i++) {
      int px = pipeX[i];
      if (px > 128 || px + PIPE_W < 0) continue;  // off-screen skip

      // Top pipe
      o.fillRect(px, 0, PIPE_W, gapY[i], SSD1306_WHITE);
      // Pipe cap (bottom of top pipe)
      o.fillRect(px - 1, gapY[i] - 3, PIPE_W + 2, 3, SSD1306_WHITE);

      // Bottom pipe
      int botTop = gapY[i] + GAP_H;
      // Pipe cap (top of bottom pipe)
      o.fillRect(px - 1, botTop, PIPE_W + 2, 3, SSD1306_WHITE);
      o.fillRect(px, botTop + 3, PIPE_W, 64 - botTop - 3, SSD1306_WHITE);
    }
  }

  void drawStart() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    dm->centerText("FLAPPY BIRD", 6, 1);
    // Demo bird
    o.fillCircle(64, 30, BIRD_R, SSD1306_WHITE);
    o.drawPixel(66, 29, SSD1306_BLACK);
    o.drawRoundRect(30, 42, 68, 14, 4, SSD1306_WHITE);
    dm->centerText("Tap to Start", 46, 1);
    char buf[20];
    snprintf(buf, sizeof(buf), "Best: %d", best);
    dm->centerText(buf, 56, 1);
    o.display();
  }

  void drawReadyScreen(unsigned long now) {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    drawPipes();
    drawBird();
    o.drawFastHLine(0, 63, 128, SSD1306_WHITE);
    dm->centerText("TAP TO FLY!", 2, 1);
    o.display();
  }

  void drawGame() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    drawPipes();
    drawBird();
    o.drawFastHLine(0, 63, 128, SSD1306_WHITE);
    // Score
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", score);
    dm->centerText(buf, 2, 1);
    o.display();
  }

  void drawGameOver() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    dm->centerText("GAME OVER", 4, 1);
    char buf[24];
    snprintf(buf, sizeof(buf), "Score: %d", score);
    dm->centerText(buf, 20, 1);
    snprintf(buf, sizeof(buf), "Best:  %d", best);
    dm->centerText(buf, 32, 1);
    if (newBest) {
      dm->centerText("** NEW BEST! **", 44, 1);
    }
    dm->centerText("Tap to retry", 54, 1);
    o.display();
  }
};

/* ============================ UpdateApp ============================= */
class UpdateApp {
  DisplayManager* dm;
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
    state       = OTA_CONNECTING;
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
        dm->centerText("Connecting Wi-Fi", 25, 1);
        o.setCursor(55, 42);
        o.setTextSize(1);
        o.print((now / 500) % 2 == 0 ? "..." : "   ");
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
        dm->centerText("Checking GitHub...", 30, 1);
        o.display();
        runCheck();
        break;

      case OTA_DOWNLOADING:
        break;

      case OTA_FAIL:
        o.clearDisplay();
        dm->centerText("UPDATE FAILED", 5, 1);
        dm->centerText(errorMsg.c_str(), 28, 1);
        dm->centerText("Dbl Tap to Exit", 50, 1);
        o.display();
        break;

      case OTA_UP_TO_DATE:
        o.clearDisplay();
        dm->centerText("SYSTEM OK", 10, 1);
        dm->centerText("No New Version", 30, 1);
        dm->centerText("Dbl Tap to Exit", 50, 1);
        o.display();
        break;

      default: break;
    }
  }

private:
  void fail(String msg) {
    errorMsg = msg;
    state    = OTA_FAIL;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  void runCheck() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (http.begin(client, VERSION_URL)) {
      int code = http.GET();
      if (code == HTTP_CODE_OK) {
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
        fail("HTTP Err: " + String(code));
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
      int code = http.GET();
      if (code == HTTP_CODE_OK) {
        int contentLength = http.getSize();
        if (Update.begin(contentLength)) {
          WiFiClient* stream = http.getStreamPtr();
          size_t written = 0;
          uint8_t buff[256];
          while (http.connected() && written < (size_t)contentLength) {
            size_t avail = stream->available();
            if (avail) {
              int c = stream->readBytes(buff, min(avail, sizeof(buff)));
              Update.write(buff, c);
              written += c;
              o.clearDisplay();
              dm->centerText("FLASHING...", 5, 1);
              int pw = map(written, 0, contentLength, 0, 100);
              o.drawRect(14, 28, 100, 10, SSD1306_WHITE);
              o.fillRect(14, 28, pw, 10, SSD1306_WHITE);
              char pct[8];
              snprintf(pct, sizeof(pct), "%d%%", pw);
              dm->centerText(pct, 44, 1);
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
        fail("Bin HTTP: " + String(code));
      }
      http.end();
    } else {
      fail("Bin Link Fail");
    }
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

AppMode mode = MODE_MOCHI;
unsigned long lastInteractionMs = 0;

/* ------------------------------------------------------------------ */
void enterMode(AppMode m) {
  mode = m;
  switch (mode) {
    case MODE_MOCHI:  mochiApp.onEnter();  break;
    case MODE_WATCH:  watchApp.onEnter();  break;
    case MODE_FLAPPY: flappyApp.onEnter(); break;
    case MODE_UPDATE: updateApp.onEnter(); break;
  }
}

/* ================================================================== */
void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  if (!displayMgr.begin()) {
    Serial.println("SH1106 init failed - check wiring/address (0x3C)");
  }

  touchMgr.begin();
  batteryMgr.begin();
  timeMgr.begin();

  mochiApp.begin(&displayMgr);
  watchApp.begin(&displayMgr, &timeMgr);
  flappyApp.begin(&displayMgr);
  updateApp.begin(&displayMgr);

  lastInteractionMs = millis();
  enterMode(MODE_MOCHI);
}

void loop() {
  unsigned long now = millis();

  // Background services
  timeMgr.update();
  batteryMgr.update();

  // Touch events
  TapEvent ev = touchMgr.update();
  if (ev != TAP_NONE) lastInteractionMs = now;

  // Mode switching on double-tap
  if (ev == TAP_DOUBLE) {
    enterMode(mode == MODE_MOCHI  ? MODE_WATCH  :
              mode == MODE_WATCH  ? MODE_FLAPPY :
              mode == MODE_FLAPPY ? MODE_UPDATE : MODE_MOCHI);
  } else if (ev == TAP_SINGLE) {
    switch (mode) {
      case MODE_MOCHI:  mochiApp.onSingleTap();  break;
      case MODE_WATCH:  /* no action */           break;
      case MODE_FLAPPY: flappyApp.onSingleTap(); break;
      case MODE_UPDATE: /* no action */           break;
    }
  }

  // Keep idle timer alive during active game / OTA
  if (mode == MODE_FLAPPY && flappyApp.isPlaying()) lastInteractionMs = now;
  if (mode == MODE_UPDATE)                          lastInteractionMs = now;

  // Idle timeout -> back to Mochi
  if (mode != MODE_MOCHI && (now - lastInteractionMs) >= IDLE_TIMEOUT) {
    lastInteractionMs = now;
    enterMode(MODE_MOCHI);
  }

  // Run active app
  switch (mode) {
    case MODE_MOCHI:  mochiApp.update();  break;
    case MODE_WATCH:  watchApp.update();  break;
    case MODE_FLAPPY: flappyApp.update(); break;
    case MODE_UPDATE: updateApp.update(); break;
  }
}

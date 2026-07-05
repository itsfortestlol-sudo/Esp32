/***********************************************************************
 * MochiPod - ESP32 Handheld (Mochi Gen2 / Watch / Flappy Bird / OTA)
 * Board  : ESP32 DevKit V1 (ESP-WROOM-32)
 * Display: SH1106 128x64 I2C
 * Touch  : TTP223 (GPIO 4, active HIGH)
 * Core   : ESP32 Arduino core 2.x / 3.x
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

#define SSD1306_WHITE SH110X_WHITE
#define SSD1306_BLACK SH110X_BLACK

/* ============================ USER CONFIG =========================== */
const char* WIFI_SSID        = "Afia Mahir";
const char* WIFI_PASS        = "afiamahir2026";
const long  GMT_OFFSET_SEC   = 21600;
const int   DST_OFFSET_SEC   = 0;
const char* NTP_SERVER       = "asia.pool.ntp.org";

const int   CURRENT_VERSION  = 2;
const char* VERSION_URL      = "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/virsion.txt";
const char* BIN_URL          = "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/firmware.bin";

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
  bool stableState     = false;
  bool lastRead        = false;
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
  St   state      = CONNECTING;
  unsigned long startMs     = 0;
  unsigned long lastRetryMs = 0;
  bool synced     = false;
public:
  void begin()      { startSync(); }

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

  /* Returns true if current hour is inside sleep window */
  bool isSleepTime() {
    if (!synced) return false;
    struct tm t;
    getLocal(t);
    int h = t.tm_hour;
    return (h >= SLEEP_HOUR_START || h < SLEEP_HOUR_END);
  }
};

/* ============================ MochiApp ==============================
 * Dasai Mochi Generation 2 — full AI state machine with activities,
 * circadian sleep, and expanded facial expressions.
 *********************************************************************/
class MochiApp {
  DisplayManager* dm  = nullptr;
  TimeManager*    tm  = nullptr;

  /* ---- Activity / expression state machine ---- */
  enum Activity {
    ACT_CHILL,          // idle neutral
    ACT_BORED,          // dissatisfied
    ACT_PHONE,          // watching phone
    ACT_COFFEE,         // drinking coffee
    ACT_KISSY,          // kissy / whistle
    ACT_SHOCKED,        // shocked / screaming
    ACT_SLEEPING,       // Zzz sleep
    ACT_ANNOYED_WAKE    // woken-up rage (3 s then back to sleep)
  };

  Activity  activity        = ACT_CHILL;
  Activity  prevActivity    = ACT_CHILL; // used to restore after shock
  unsigned long activityEndMs   = 0;    // when current timed activity ends
  unsigned long nextActivityMs  = 0;    // next autonomous switch
  unsigned long lastFrameMs     = 0;

  /* ---- Blink ---- */
  bool blinking       = false;
  unsigned long blinkStartMs    = 0;
  unsigned long nextBlinkMs     = 0;

  /* ---- Bounce ---- */
  float bouncePhase   = 0.0f;

  /* ---- Zzz particles ---- */
  struct ZzzParticle {
    float x, y, vy;
    int   size;   // 1 or 2
    bool  active;
  };
  static const int ZZZ_MAX = 4;
  ZzzParticle zzz[ZZZ_MAX];
  unsigned long nextZzzMs = 0;

  /* ---- Annoy shake ---- */
  unsigned long annoyEndMs   = 0;
  int           shakeOffset  = 0;

  /* ---- Coffee cup sip animation ---- */
  float coffeeSip    = 0.0f;   // 0..1 tilts cup
  bool  sipping      = false;
  unsigned long sipEndMs = 0;

  /* ---- Phone scroll animation ---- */
  int  phoneScrollY  = 0;
  unsigned long nextScrollMs = 0;

public:
  void begin(DisplayManager* d, TimeManager* t) {
    dm = d; tm = t;
    initZzz();
    nextActivityMs = millis() + random(4000, 8000);
    nextBlinkMs    = millis() + random(2000, 5000);
  }

  void onEnter() {
    activity      = ACT_CHILL;
    activityEndMs = 0;
    nextActivityMs = millis() + random(3000, 6000);
  }

  void onSingleTap() {
    unsigned long now = millis();
    if (activity == ACT_SLEEPING) {
      // Woken up — angry!
      setActivity(ACT_ANNOYED_WAKE, 3000);
      annoyEndMs = now + 3000;
    } else {
      // Normal tap -> brief shock then return
      prevActivity = activity;
      setActivity(ACT_SHOCKED, 1200);
    }
  }

  void update() {
    unsigned long now = millis();
    if (now - lastFrameMs < 33) return; // ~30 fps
    lastFrameMs = now;

    /* ---- Circadian sleep check ---- */
    if (tm->isSleepTime()) {
      if (activity != ACT_SLEEPING && activity != ACT_ANNOYED_WAKE) {
        setActivity(ACT_SLEEPING, 0); // 0 = indefinite
      }
    } else {
      if (activity == ACT_SLEEPING) {
        setActivity(ACT_CHILL, 0);
        nextActivityMs = now + random(3000, 6000);
      }
    }

    /* ---- Timed activity expiry ---- */
    if (activityEndMs > 0 && now >= activityEndMs) {
      activityEndMs = 0;
      if (activity == ACT_ANNOYED_WAKE) {
        setActivity(ACT_SLEEPING, 0); // back to sleep
      } else if (activity == ACT_SHOCKED) {
        activity = prevActivity;
      } else {
        activity = ACT_CHILL;
        nextActivityMs = now + random(3000, 6000);
      }
    }

    /* ---- Autonomous activity scheduler ---- */
    if (activityEndMs == 0 && now >= nextActivityMs &&
        activity != ACT_SLEEPING && activity != ACT_ANNOYED_WAKE) {
      // Weighted pool of autonomous behaviours
      int roll = random(0, 100);
      if      (roll < 25) setActivity(ACT_CHILL,   0);
      else if (roll < 45) setActivity(ACT_BORED,   random(3000, 6000));
      else if (roll < 65) setActivity(ACT_PHONE,   random(4000, 8000));
      else if (roll < 80) setActivity(ACT_COFFEE,  random(3000, 5000));
      else if (roll < 90) setActivity(ACT_KISSY,   random(2000, 4000));
      else                setActivity(ACT_SHOCKED, 1500);
      nextActivityMs = now + random(5000, 10000);
    }

    /* ---- Blink (skip during sleep/shocked/annoyed) ---- */
    if (activity != ACT_SLEEPING && activity != ACT_ANNOYED_WAKE &&
        activity != ACT_SHOCKED) {
      if (!blinking && now >= nextBlinkMs) {
        blinking     = true;
        blinkStartMs = now;
      }
      if (blinking && now - blinkStartMs > 160) {
        blinking    = false;
        nextBlinkMs = now + random(2000, 5000);
      }
    } else {
      blinking = false;
    }

    /* ---- Bounce phase ---- */
    bouncePhase += 0.025f;
    if (bouncePhase > TWO_PI) bouncePhase -= TWO_PI;

    /* ---- Coffee sip ---- */
    if (activity == ACT_COFFEE) {
      if (!sipping && now >= sipEndMs) {
        sipping  = true;
        sipEndMs = now + 800;
      } else if (sipping && now >= sipEndMs) {
        sipping  = false;
        sipEndMs = now + random(1500, 3000);
        coffeeSip = 0.0f;
      }
      if (sipping) {
        float prog = 1.0f - (float)(sipEndMs - now) / 800.0f;
        coffeeSip = sinf(prog * PI);
      }
    }

    /* ---- Phone scroll ---- */
    if (activity == ACT_PHONE && now >= nextScrollMs) {
      phoneScrollY = random(0, 8);
      nextScrollMs = now + random(600, 1400);
    }

    /* ---- Zzz particles ---- */
    if (activity == ACT_SLEEPING || activity == ACT_ANNOYED_WAKE) {
      updateZzz(now);
    }

    /* ---- Shake offset for annoyed ---- */
    if (activity == ACT_ANNOYED_WAKE) {
      shakeOffset = (random(0, 2) == 0) ? random(-3, 3) : 0;
    } else {
      shakeOffset = 0;
    }

    draw(now);
  }

private:
  void setActivity(Activity a, unsigned long durationMs) {
    activity      = a;
    activityEndMs = (durationMs > 0) ? millis() + durationMs : 0;
    // Reset sub-state
    coffeeSip  = 0.0f;
    sipping    = false;
    sipEndMs   = millis() + 1500;
    phoneScrollY = 0;
    nextScrollMs = millis() + 600;
    initZzz();
    nextZzzMs = millis();
  }

  void initZzz() {
    for (int i = 0; i < ZZZ_MAX; i++) {
      zzz[i].active = false;
      zzz[i].x     = 80.0f;
      zzz[i].y     = 20.0f;
      zzz[i].vy    = -0.4f;
      zzz[i].size  = 1;
    }
  }

  void spawnZzz(unsigned long now) {
    if (now < nextZzzMs) return;
    nextZzzMs = now + 900;
    for (int i = 0; i < ZZZ_MAX; i++) {
      if (!zzz[i].active) {
        zzz[i].active = true;
        zzz[i].x     = 82.0f + random(-3, 4);
        zzz[i].y     = 22.0f;
        zzz[i].vy    = -0.35f - random(0, 3) * 0.05f;
        zzz[i].size  = (i % 2 == 0) ? 1 : 2;
        break;
      }
    }
  }

  void updateZzz(unsigned long now) {
    spawnZzz(now);
    for (int i = 0; i < ZZZ_MAX; i++) {
      if (!zzz[i].active) continue;
      zzz[i].y += zzz[i].vy;
      zzz[i].x += 0.15f;
      if (zzz[i].y < 0 || zzz[i].x > 128) zzz[i].active = false;
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

  /* ---- Sub-renderers ---- */

  // Prop: tiny smartphone (16x22 px) at given top-left
  void drawPhone(Adafruit_SH1106G& o, int px, int py) {
    // Body
    o.drawRoundRect(px, py, 16, 22, 2, SSD1306_WHITE);
    // Screen area
    o.fillRect(px + 2, py + 2, 12, 16, SSD1306_WHITE);
    // Scrolling content lines (inverted on screen)
    int lineY = py + 3 + phoneScrollY;
    for (int l = 0; l < 3; l++) {
      int ly = lineY + l * 4;
      if (ly >= py + 2 && ly < py + 17) {
        o.drawFastHLine(px + 3, ly, 10, SSD1306_BLACK);
      }
    }
    // Home button
    o.drawCircle(px + 8, py + 19, 1, SSD1306_WHITE);
  }

  // Prop: tiny coffee cup (14x14) at given top-left
  void drawCoffeeCup(Adafruit_SH1106G& o, int cx, int cy, float sip) {
    int tiltX = (int)(sip * 5.0f);
    int tiltY = (int)(sip * 2.0f);
    // Cup body
    o.drawLine(cx,        cy + 5 - tiltY, cx + 2,      cy + 13, SSD1306_WHITE);
    o.drawLine(cx + 12 - tiltX, cy + 5 - tiltY, cx + 10, cy + 13, SSD1306_WHITE);
    o.drawFastHLine(cx + 2,  cy + 13, 8, SSD1306_WHITE);
    o.drawFastHLine(cx,      cy + 5 - tiltY, 12 - tiltX, SSD1306_WHITE);
    // Handle
    o.drawLine(cx + 12 - tiltX, cy + 7 - tiltY,
               cx + 14 - tiltX, cy + 7 - tiltY, SSD1306_WHITE);
    o.drawLine(cx + 14 - tiltX, cy + 7 - tiltY,
               cx + 14 - tiltX, cy + 11,          SSD1306_WHITE);
    o.drawLine(cx + 14 - tiltX, cy + 11,
               cx + 10,         cy + 11,           SSD1306_WHITE);
    // Steam wisps
    if (sip < 0.3f) {
      o.drawPixel(cx + 4, cy + 3, SSD1306_WHITE);
      o.drawPixel(cx + 7, cy + 2, SSD1306_WHITE);
      o.drawPixel(cx + 10, cy + 3, SSD1306_WHITE);
    }
  }

  /* ---- Main draw ---- */
  void draw(unsigned long now) {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();

    int bounce   = (int)(2.5f * sinf(bouncePhase));
    int baseX    = shakeOffset; // horizontal shake during annoyance
    int faceX    = 64 + baseX;  // face center X

    // Eye positions relative to face center
    int eyeBaseY = 18 + bounce;
    int lx = faceX - 30; // left eye left edge
    int rx = faceX + 10; // right eye left edge

    /* ============================================================
     * Draw face based on current activity
     * ============================================================ */
    if (activity == ACT_SLEEPING || activity == ACT_ANNOYED_WAKE) {
      drawFaceSleeping(o, lx, rx, eyeBaseY, bounce, faceX, now);
    } else if (activity == ACT_SHOCKED) {
      drawFaceShocked(o, lx, rx, eyeBaseY, bounce, faceX);
    } else if (activity == ACT_KISSY) {
      drawFaceKissy(o, lx, rx, eyeBaseY, bounce, faceX);
    } else if (activity == ACT_BORED) {
      drawFaceBored(o, lx, rx, eyeBaseY, bounce, faceX);
    } else {
      // CHILL, PHONE, COFFEE all use neutral eyes + mouth
      drawFaceNeutral(o, lx, rx, eyeBaseY, bounce, faceX);
    }

    /* ---- Props ---- */
    if (activity == ACT_PHONE) {
      drawPhone(o, faceX + 18, 20 + bounce);
    } else if (activity == ACT_COFFEE) {
      drawCoffeeCup(o, faceX + 20, 28 + bounce, coffeeSip);
    }

    /* ---- Zzz ---- */
    if (activity == ACT_SLEEPING || activity == ACT_ANNOYED_WAKE) {
      drawZzz(o);
    }

    o.display();
  }

  /* ---- Neutral / Chill face ---- */
  void drawFaceNeutral(Adafruit_SH1106G& o,
                       int lx, int rx, int eyeY, int bounce, int faceX) {
    int eyeW = 20, eyeH = 26;
    if (blinking) {
      float ph = (millis() - blinkStartMs) / 160.0f;
      float k  = 1.0f - sinf(ph * PI);
      eyeH = max(3, (int)(26 * k));
      eyeY = eyeY + (26 - eyeH) / 2;
    }
    o.fillRoundRect(lx, eyeY, eyeW, eyeH, 8, SSD1306_WHITE);
    o.fillRoundRect(rx, eyeY, eyeW, eyeH, 8, SSD1306_WHITE);
    // Gleam highlights
    o.fillRect(lx + 3, eyeY + 3, 4, 3, SSD1306_BLACK);
    o.fillRect(rx + 3, eyeY + 3, 4, 3, SSD1306_BLACK);
    // Tiny flat mouth
    o.fillRoundRect(faceX - 4, 52 + bounce, 8, 2, 1, SSD1306_WHITE);
  }

  /* ---- Bored / Dissatisfied ---- */
  void drawFaceBored(Adafruit_SH1106G& o,
                     int lx, int rx, int eyeY, int bounce, int faceX) {
    // Half-lidded eyes (top third filled black)
    int eyeW = 20, eyeH = 26;
    o.fillRoundRect(lx, eyeY,     eyeW, eyeH, 8, SSD1306_WHITE);
    o.fillRoundRect(rx, eyeY,     eyeW, eyeH, 8, SSD1306_WHITE);
    o.fillRect(lx,     eyeY,     eyeW, eyeH / 3, SSD1306_BLACK);
    o.fillRect(rx,     eyeY,     eyeW, eyeH / 3, SSD1306_BLACK);
    // Droopy eyelid line
    o.drawFastHLine(lx, eyeY + eyeH / 3, eyeW, SSD1306_WHITE);
    o.drawFastHLine(rx, eyeY + eyeH / 3, eyeW, SSD1306_WHITE);
    // Flat / slight-frown mouth
    o.drawLine(faceX - 6, 53 + bounce, faceX - 2, 55 + bounce, SSD1306_WHITE);
    o.drawLine(faceX - 2, 55 + bounce, faceX + 6, 53 + bounce, SSD1306_WHITE);
  }

  /* ---- Kissy / Whistle ---- */
  void drawFaceKissy(Adafruit_SH1106G& o,
                     int lx, int rx, int eyeY, int bounce, int faceX) {
    // Happy closed arcs for eyes
    for (int t = 0; t < 3; t++) {
      o.drawLine(lx,      eyeY + 18, lx + 10, eyeY + 8 + t, SSD1306_WHITE);
      o.drawLine(lx + 10, eyeY + 8 + t, lx + 20, eyeY + 18, SSD1306_WHITE);
      o.drawLine(rx,      eyeY + 18, rx + 10, eyeY + 8 + t, SSD1306_WHITE);
      o.drawLine(rx + 10, eyeY + 8 + t, rx + 20, eyeY + 18, SSD1306_WHITE);
    }
    // Small "O" puckered mouth
    o.drawCircle(faceX, 52 + bounce, 4, SSD1306_WHITE);
  }

  /* ---- Shocked / Screaming ---- */
  void drawFaceShocked(Adafruit_SH1106G& o,
                       int lx, int rx, int eyeY, int bounce, int faceX) {
    // Wide circular eyes
    o.fillCircle(lx + 10, eyeY + 13, 12, SSD1306_WHITE);
    o.fillCircle(rx + 10, eyeY + 13, 12, SSD1306_WHITE);
    // Pupils
    o.fillCircle(lx + 10, eyeY + 13, 5, SSD1306_BLACK);
    o.fillCircle(rx + 10, eyeY + 13, 5, SSD1306_BLACK);
    // Shrunken highlights
    o.fillCircle(lx + 8,  eyeY + 11, 2, SSD1306_WHITE);
    o.fillCircle(rx + 8,  eyeY + 11, 2, SSD1306_WHITE);
    // Wide open screaming mouth
    o.fillRoundRect(faceX - 8, 49 + bounce, 16, 11, 4, SSD1306_WHITE);
    o.fillRoundRect(faceX - 5, 51 + bounce, 10, 7,  2, SSD1306_BLACK);
    // Vertical scream lines above face
    for (int i = -2; i <= 2; i++) {
      o.drawFastVLine(faceX + i * 6, 2, 8, SSD1306_WHITE);
    }
  }

  /* ---- Sleeping face ---- */
  void drawFaceSleeping(Adafruit_SH1106G& o,
                        int lx, int rx, int eyeY, int bounce, int faceX,
                        unsigned long now) {
    if (activity == ACT_ANNOYED_WAKE) {
      // Woken-up rage: shocked base + red-tint effect via alternating lines
      drawFaceShocked(o, lx, rx, eyeY, bounce, faceX);
      // Rage stripes (horizontal scan lines for anger flush effect)
      for (int row = 0; row < 64; row += 4) {
        o.drawFastHLine(0, row, 128, SSD1306_BLACK);
      }
      // "!!" exclamation
      o.setTextSize(2);
      o.setCursor(faceX - 10, 2);
      o.print("!!");
      o.setTextSize(1);
    } else {
      // Peaceful closed eyes (smooth arcs)
      for (int t = 0; t < 2; t++) {
        o.drawLine(lx,      eyeY + 13, lx + 10, eyeY + 5 + t, SSD1306_WHITE);
        o.drawLine(lx + 10, eyeY + 5 + t, lx + 20, eyeY + 13, SSD1306_WHITE);
        o.drawLine(rx,      eyeY + 13, rx + 10, eyeY + 5 + t, SSD1306_WHITE);
        o.drawLine(rx + 10, eyeY + 5 + t, rx + 20, eyeY + 13, SSD1306_WHITE);
      }
      // Serene tiny smile
      o.drawLine(faceX - 5, 52 + bounce, faceX,     50 + bounce, SSD1306_WHITE);
      o.drawLine(faceX,     50 + bounce, faceX + 5, 52 + bounce, SSD1306_WHITE);
      // Moon icon top-left
      o.drawCircle(8, 8, 6, SSD1306_WHITE);
      o.fillCircle(11, 6, 5, SSD1306_BLACK);
    }
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

    /* Battery icon + %, top-right */
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

    /* Big HH:MM */
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    dm->centerText(buf, 20, 3);

    /* Seconds */
    snprintf(buf, sizeof(buf), ":%02d", t.tm_sec);
    o.setTextSize(1);
    o.setCursor(110, 38);
    o.print(buf);

    /* Date */
    strftime(buf, sizeof(buf), "%d %b %Y", &t);
    dm->centerText(buf, 52, 1);

    o.display();
  }
};

/* ============================ FlappyApp ============================= *
 * Redesigned rendering with cleaner arcade aesthetics.               *
 * Physics rebalanced for a floatier, easier feel.                    *
 **********************************************************************/
class FlappyApp {
  DisplayManager* dm = nullptr;
  Preferences prefs;
public:
  enum GState { G_START, G_PLAYING, G_OVER };
  GState gstate = G_START;

private:
  /* Bird */
  float birdY    = 32.0f;
  float birdV    = 0.0f;
  static constexpr int   BIRD_X = 22;
  static constexpr int   BIRD_R = 4;   // slightly larger bird

  /* Physics — floatier, more forgiving */
  static constexpr float GRAVITY     = 0.18f;  // was 0.28
  static constexpr float FLAP_V      = -2.2f;  // was -2.9  (less violent)
  static constexpr float PIPE_SPEED  = 1.4f;   // was 2.0   (slower pipes)

  /* Pipes */
  static constexpr int NPIPES        = 3;
  static constexpr int PIPE_W        = 14;
  static constexpr int GAP_H         = 40;     // was 26 — much larger gap
  static constexpr int PIPE_SPACING  = 60;     // was 52 — more breathing room

  float pipeX[NPIPES];
  int   gapY[NPIPES];
  bool  passed[NPIPES];

  int  score      = 0;
  int  best       = 0;
  bool dirty      = true;
  unsigned long lastFrameMs = 0;

  /* Wing flap animation */
  float wingPhase = 0.0f;

public:
  void begin(DisplayManager* d) {
    dm = d;
    prefs.begin("flappy", false);
    best = prefs.getInt("best", 0);
  }

  void onEnter()  { gstate = G_START; dirty = true; }
  bool isPlaying() const { return gstate == G_PLAYING; }

  void onSingleTap() {
    if      (gstate == G_START)   startGame();
    else if (gstate == G_PLAYING) { birdV = FLAP_V; wingPhase = 0.0f; }
    else                           startGame();
  }

  void startGame() {
    birdY = 32.0f; birdV = 0.0f; score = 0; wingPhase = 0.0f;
    for (int i = 0; i < NPIPES; i++) {
      pipeX[i]  = 128.0f + i * PIPE_SPACING;
      gapY[i]   = random(8, 64 - 8 - GAP_H);
      passed[i] = false;
    }
    gstate = G_PLAYING;
  }

  void update() {
    unsigned long now = millis();
    if (now - lastFrameMs < 33) return;
    lastFrameMs = now;

    if (gstate == G_START) { if (dirty) { drawStart(); dirty = false; } return; }
    if (gstate == G_OVER)  { if (dirty) { drawGameOver(); dirty = false; } return; }

    /* Physics */
    birdV += GRAVITY;
    birdY += birdV;
    wingPhase += 0.18f;
    if (wingPhase > TWO_PI) wingPhase -= TWO_PI;

    /* Pipes */
    for (int i = 0; i < NPIPES; i++) {
      pipeX[i] -= PIPE_SPEED;
      if (pipeX[i] + PIPE_W < 0) {
        // Find rightmost pipe to chain from
        float maxX = pipeX[0];
        for (int j = 1; j < NPIPES; j++) if (pipeX[j] > maxX) maxX = pipeX[j];
        pipeX[i]  = maxX + PIPE_SPACING;
        gapY[i]   = random(8, 64 - 8 - GAP_H);
        passed[i] = false;
      }
      if (!passed[i] && pipeX[i] + PIPE_W < BIRD_X - BIRD_R) {
        passed[i] = true;
        score++;
      }
    }

    /* Collisions */
    bool dead = (birdY - BIRD_R <= 1) || (birdY + BIRD_R >= 62);
    for (int i = 0; i < NPIPES && !dead; i++) {
      bool inX  = ((float)(BIRD_X + BIRD_R) > pipeX[i]) &&
                  ((float)(BIRD_X - BIRD_R) < pipeX[i] + PIPE_W);
      bool inGap = ((birdY - BIRD_R) > gapY[i]) &&
                   ((birdY + BIRD_R) < gapY[i] + GAP_H);
      if (inX && !inGap) dead = true;
    }
    if (dead) {
      if (score > best) { best = score; prefs.putInt("best", best); }
      gstate = G_OVER; dirty = true;
      return;
    }
    drawGame();
  }

private:
  /* ---- Pipe with decorative cap ---- */
  void drawPipe(Adafruit_SH1106G& o, int px, int topH, int botY) {
    // Top pipe body
    if (topH > 4) {
      o.fillRect(px + 1, 0, PIPE_W - 2, topH - 4, SSD1306_WHITE);
    }
    // Top pipe cap (wider)
    o.fillRect(px - 1, topH - 4, PIPE_W + 2, 4, SSD1306_WHITE);
    // Top pipe inner shadow line
    o.drawFastVLine(px + 2, 0, topH - 4, SSD1306_BLACK);

    // Bottom pipe cap
    o.fillRect(px - 1, botY, PIPE_W + 2, 4, SSD1306_WHITE);
    // Bottom pipe body
    int bodyH = 63 - (botY + 4);
    if (bodyH > 0) {
      o.fillRect(px + 1, botY + 4, PIPE_W - 2, bodyH, SSD1306_WHITE);
    }
    // Bottom pipe inner shadow line
    o.drawFastVLine(px + 2, botY + 4, bodyH, SSD1306_BLACK);
  }

  /* ---- Cartoon bird with wing flap ---- */
  void drawBird(Adafruit_SH1106G& o, int bx, int by) {
    // Body
    o.fillCircle(bx, by, BIRD_R, SSD1306_WHITE);
    // Wing (flaps up/down)
    int wingOff = (int)(2.0f * sinf(wingPhase));
    o.fillRoundRect(bx - BIRD_R - 2, by - 1 + wingOff, 6, 3, 1, SSD1306_WHITE);
    // Beak
    o.fillTriangle(bx + BIRD_R - 1, by - 1,
                   bx + BIRD_R + 3, by,
                   bx + BIRD_R - 1, by + 2, SSD1306_WHITE);
    // Eye
    o.drawPixel(bx + 2, by - 2, SSD1306_BLACK);
    // Tail feather
    o.drawLine(bx - BIRD_R, by + 1, bx - BIRD_R - 3, by - 1, SSD1306_WHITE);
  }

  void drawStart() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    // Title box
    o.fillRoundRect(10, 4, 108, 16, 4, SSD1306_WHITE);
    o.setTextColor(SSD1306_BLACK);
    dm->centerText("FLAPPY MOCHI", 8, 1);
    o.setTextColor(SSD1306_WHITE);
    // Decorative pipes
    o.fillRect(0,   0, 6, 20, SSD1306_WHITE);
    o.fillRect(122, 0, 6, 20, SSD1306_WHITE);
    o.fillRect(0,   48, 6, 16, SSD1306_WHITE);
    o.fillRect(122, 48, 6, 16, SSD1306_WHITE);
    // Bird preview
    drawBird(o, 64, 38);
    // Ground
    o.drawFastHLine(0, 62, 128, SSD1306_WHITE);
    o.drawFastHLine(0, 63, 128, SSD1306_WHITE);
    dm->centerText("Tap to Start!", 53, 1);
    o.display();
  }

  void drawGame() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();

    // Pipes
    for (int i = 0; i < NPIPES; i++) {
      int px    = (int)pipeX[i];
      int topH  = gapY[i];
      int botY  = gapY[i] + GAP_H;
      drawPipe(o, px, topH, botY);
    }

    // Ground (double-line with dotted texture)
    o.drawFastHLine(0, 62, 128, SSD1306_WHITE);
    o.drawFastHLine(0, 63, 128, SSD1306_WHITE);
    for (int x = 0; x < 128; x += 4) o.drawPixel(x, 61, SSD1306_WHITE);

    // Bird
    drawBird(o, BIRD_X, (int)birdY);

    // Score in a small box
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", score);
    int sw = (int)(strlen(buf) * 6 + 6);
    o.drawRoundRect((128 - sw) / 2, 1, sw, 10, 2, SSD1306_WHITE);
    dm->centerText(buf, 3, 1);

    o.display();
  }

  void drawGameOver() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();

    // Main panel
    o.drawRoundRect(4, 2, 120, 60, 5, SSD1306_WHITE);
    o.drawRoundRect(6, 4, 116, 56, 3, SSD1306_WHITE);

    // Header bar
    o.fillRoundRect(10, 6, 108, 14, 3, SSD1306_WHITE);
    o.setTextColor(SSD1306_BLACK);
    dm->centerText("GAME OVER", 10, 1);
    o.setTextColor(SSD1306_WHITE);

    char buf[20];
    snprintf(buf, sizeof(buf), "Score: %d", score);
    dm->centerText(buf, 26, 1);
    snprintf(buf, sizeof(buf), "Best:  %d", best);
    dm->centerText(buf, 38, 1);
    // Divider
    o.drawFastHLine(20, 34, 88, SSD1306_WHITE);
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
        // Handled inside executeUpdate() with blocking progress bar
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
    state    = OTA_FAIL;
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

/* ============================ Globals =============================== */
DisplayManager displayMgr;
TouchManager   touchMgr;
BatteryManager batteryMgr;
TimeManager    timeMgr;
MochiApp       mochiApp;
WatchApp       watchApp;
FlappyApp      flappyApp;
UpdateApp      updateApp;

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

  if (!displayMgr.begin()) {
    Serial.println("SH1106 init failed - check wiring/address (0x3C)");
  }

  touchMgr.begin();
  batteryMgr.begin();
  timeMgr.begin();

  mochiApp.begin(&displayMgr, &timeMgr);
  watchApp.begin(&displayMgr, &timeMgr, &batteryMgr);
  flappyApp.begin(&displayMgr);
  updateApp.begin(&displayMgr);

  lastInteractionMs = millis();
  enterMode(MODE_MOCHI);
}

void loop() {
  unsigned long now = millis();

  /* Background services */
  timeMgr.update();
  batteryMgr.update();

  /* Touch events */
  TapEvent ev = touchMgr.update();
  if (ev != TAP_NONE) lastInteractionMs = now;

  if (ev == TAP_DOUBLE) {
    /* Circular mode switch: Mochi -> Watch -> Flappy -> Update -> Mochi */
    enterMode(mode == MODE_MOCHI  ? MODE_WATCH  :
              mode == MODE_WATCH  ? MODE_FLAPPY :
              mode == MODE_FLAPPY ? MODE_UPDATE : MODE_MOCHI);
  } else if (ev == TAP_SINGLE) {
    switch (mode) {
      case MODE_MOCHI:  mochiApp.onSingleTap();  break;
      case MODE_FLAPPY: flappyApp.onSingleTap(); break;
      case MODE_WATCH:  /* no single-tap action */ break;
      case MODE_UPDATE: /* no single-tap action */ break;
    }
  }

  /* Idle timeout: pause timer while Flappy is active or OTA running */
  if (mode == MODE_FLAPPY && flappyApp.isPlaying()) lastInteractionMs = now;
  if (mode == MODE_UPDATE)                          lastInteractionMs = now;

  if (mode != MODE_MOCHI && (now - lastInteractionMs) >= IDLE_TIMEOUT) {
    lastInteractionMs = now;
    enterMode(MODE_MOCHI);
  }

  /* Active mode tick */
  switch (mode) {
    case MODE_MOCHI:  mochiApp.update();  break;
    case MODE_WATCH:  watchApp.update();  break;
    case MODE_FLAPPY: flappyApp.update(); break;
    case MODE_UPDATE: updateApp.update(); break;
  }
}

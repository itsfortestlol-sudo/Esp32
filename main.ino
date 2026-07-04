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
const char* VERSION_URL     = "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/main.ino";
const char* BIN_URL         = "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/firmware.bin";

#define PIN_OLED_SDA   21
#define PIN_OLED_SCL   22
#define PIN_TOUCH      4
#define PIN_VBAT       34

const float VBAT_DIVIDER   = 2.0f;
const float VBAT_CAL       = 1.00f;
const float VBAT_FULL      = 4.20f;
const float VBAT_EMPTY     = 3.30f;

const unsigned long TOUCH_DEBOUNCE_MS    = 30;
const unsigned long DOUBLE_TAP_WINDOW_MS = 300;
const unsigned long IDLE_TIMEOUT = 20000;

// Mochi idle neglect timeout (10 minutes)
const unsigned long NEGLECT_TIMEOUT = 600000UL;

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
        state = SYNCING; startMs = now;
      } else if (now - startMs > 30000) {
        shutdownWifi(); lastRetryMs = now;
      }
    } else if (state == SYNCING) {
      if (time(nullptr) > 1700000000) {
        synced = true; shutdownWifi();
      } else if (now - startMs > 15000) {
        shutdownWifi(); lastRetryMs = now;
      }
    } else if (state == DONE && !synced) {
      if (now - lastRetryMs > 60000) startSync();
    }
  }
  void shutdownWifi() {
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF); state = DONE;
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

  // All possible autonomous activities + emotional states
  enum Activity {
    ACT_IDLE,           // normal resting
    ACT_WATCHING_PHONE, // looking at phone
    ACT_DRINKING_COFFEE,// drinking coffee
    ACT_EATING,         // eating something
    ACT_YAWNING,        // yawning
    ACT_ROLLING_EYES,   // rolling eyes 🙄
    ACT_WHISTLING,      // whistling 😗
    ACT_POUTY,          // pouty face 😒
    ACT_BLANK_STARE,    // blank stare 😑
    ACT_BIG_GRIN,       // big grin 😁
    ACT_THINKING,       // thinking with hand on chin
    ACT_STRETCHING,     // stretching arms
    ACT_DANCING,        // little bounce dance
    ACT_WINKING,        // wink
    // Neglect states (triggered after long no-interaction)
    ACT_SAD,            // sad face
    ACT_CRYING,         // crying
    ACT_SLEEPING,       // sleeping zzz
    ACT_LONELY,         // lonely stare
    // Touch reaction states
    ACT_TOUCHED_HAPPY,  // feels good from touch
    ACT_TOUCHED_BLUSH,  // blushing
    ACT_TOUCHED_GIGGLE, // giggling
  };

  Activity currentActivity = ACT_IDLE;
  Activity nextActivity = ACT_IDLE;
  unsigned long activityStartMs = 0;
  unsigned long activityDurationMs = 3000;
  unsigned long nextActivityMs = 0;
  unsigned long lastFrameMs = 0;
  unsigned long lastInteractionMs = 0;
  unsigned long lastTouchMs = 0;

  bool blinking = false;
  unsigned long nextBlinkMs = 0;
  unsigned long blinkStartMs = 0;

  bool neglected = false;
  bool touchReaction = false;
  unsigned long touchReactionEndMs = 0;

  // Animation sub-frame counters
  int animFrame = 0;
  unsigned long lastAnimFrameMs = 0;

  // For sleeping animation
  int zCount = 0;
  unsigned long lastZMs = 0;

  // For eating animation
  int eatFrame = 0;

  // For dancing
  float dancePhase = 0;

  // For crying
  int tearY1 = 20, tearY2 = 20;

public:
  void begin(DisplayManager* d) {
    dm = d;
    lastInteractionMs = millis();
    lastTouchMs = millis();
    scheduleNextActivity(3000);
    nextBlinkMs = millis() + random(2000, 5000);
  }

  void onEnter() {
    currentActivity = ACT_IDLE;
    neglected = false;
    touchReaction = false;
    scheduleNextActivity(2000);
  }

  void onSingleTap() {
    lastInteractionMs = millis();
    lastTouchMs = millis();
    neglected = false;

    // Pick a touch reaction
    int r = random(0, 3);
    if (r == 0) currentActivity = ACT_TOUCHED_HAPPY;
    else if (r == 1) currentActivity = ACT_TOUCHED_BLUSH;
    else currentActivity = ACT_TOUCHED_GIGGLE;

    touchReaction = true;
    touchReactionEndMs = millis() + 2000;
    activityStartMs = millis();
    animFrame = 0;
  }

  void update() {
    unsigned long now = millis();
    if (now - lastFrameMs < 33) return;
    lastFrameMs = now;

    // Check for neglect
    unsigned long timeSinceTouch = now - lastTouchMs;
    if (timeSinceTouch > NEGLECT_TIMEOUT && !neglected) {
      neglected = true;
      // Pick a neglect state randomly
      int r = random(0, 4);
      if (r == 0) currentActivity = ACT_SAD;
      else if (r == 1) currentActivity = ACT_CRYING;
      else if (r == 2) currentActivity = ACT_SLEEPING;
      else currentActivity = ACT_LONELY;
      activityStartMs = now;
      animFrame = 0;
      zCount = 0;
    }

    // Touch reaction timeout
    if (touchReaction && now > touchReactionEndMs) {
      touchReaction = false;
      neglected = false;
      scheduleNextActivity(1000);
    }

    // Schedule next autonomous activity (only if not neglected or in touch reaction)
    if (!neglected && !touchReaction && now >= nextActivityMs) {
      currentActivity = pickRandomActivity();
      activityStartMs = now;
      animFrame = 0;
      scheduleNextActivity(random(4000, 9000));
    }

    // Blink logic (independent of activity for most states)
    if (!blinking && now >= nextBlinkMs &&
        currentActivity != ACT_SLEEPING &&
        currentActivity != ACT_CRYING &&
        currentActivity != ACT_WINKING) {
      blinking = true;
      blinkStartMs = now;
    }
    if (blinking && now - blinkStartMs > 150) {
      blinking = false;
      nextBlinkMs = now + random(2000, 5000);
    }

    // Advance animation frames at ~100ms per frame
    if (now - lastAnimFrameMs > 100) {
      animFrame++;
      lastAnimFrameMs = now;
    }

    draw(now);
  }

private:
  void scheduleNextActivity(unsigned long delayMs) {
    nextActivityMs = millis() + delayMs;
  }

  Activity pickRandomActivity() {
    // Weighted random pick from autonomous activities
    Activity pool[] = {
      ACT_IDLE, ACT_IDLE, ACT_IDLE,
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
    };
    int sz = sizeof(pool) / sizeof(pool[0]);
    return pool[random(0, sz)];
  }

  // Helper: draw a rounded rect eye with optional pupil offset
  void drawEye(Adafruit_SH1106G& o, int x, int y, int w, int h, int pupilDx, int pupilDy, bool closed) {
    if (closed) {
      // Draw as a horizontal line (closed eye)
      o.drawFastHLine(x, y + h / 2, w, SSD1306_WHITE);
      o.drawFastHLine(x, y + h / 2 + 1, w, SSD1306_WHITE);
      return;
    }
    o.fillRoundRect(x, y, w, h, 4, SSD1306_WHITE);
    // Pupil
    int px = x + w / 2 + pupilDx - 2;
    int py = y + h / 2 + pupilDy - 2;
    px = constrain(px, x + 1, x + w - 4);
    py = constrain(py, y + 1, y + h - 4);
    o.fillRect(px, py, 4, 4, SSD1306_BLACK);
  }

  void draw(unsigned long now) {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();

    int bounce = (int)(1.5f * sinf(now / 500.0f));
    int cx = 64; // center x
    int baseY = 10 + bounce;

    // Eye base positions
    int lEyeX = 36, rEyeX = 72;
    int eyeY = baseY + 8;
    int eyeW = 18, eyeH = 18;

    // Default pupil offsets
    int pupilDx = 0, pupilDy = 0;

    switch (currentActivity) {

      case ACT_IDLE: {
        // Simple calm face
        drawEye(o, lEyeX, eyeY, eyeW, eyeH, 0, 0, blinking);
        drawEye(o, rEyeX, eyeY, eyeW, eyeH, 0, 0, blinking);
        // Small smile
        o.drawLine(54, 42 + bounce, 58, 46 + bounce, SSD1306_WHITE);
        o.drawLine(58, 46 + bounce, 70, 46 + bounce, SSD1306_WHITE);
        o.drawLine(70, 46 + bounce, 74, 42 + bounce, SSD1306_WHITE);
        break;
      }

      case ACT_WATCHING_PHONE: {
        // Eyes looking down at a phone
        drawEye(o, lEyeX, eyeY + 4, eyeW, eyeH - 4, 0, 3, false);
        drawEye(o, rEyeX, eyeY + 4, eyeW, eyeH - 4, 0, 3, false);
        // Phone rectangle below face
        int ph = (animFrame % 2 == 0) ? 0 : 1; // subtle glow flicker
        o.drawRect(44, 46 + bounce, 40, 14, SSD1306_WHITE);
        o.fillRect(46, 48 + bounce, 36, 10, ph ? SSD1306_WHITE : SSD1306_BLACK);
        if (!ph) {
          // Draw tiny "screen content" lines
          o.drawFastHLine(48, 50 + bounce, 20, SSD1306_WHITE);
          o.drawFastHLine(48, 53 + bounce, 14, SSD1306_WHITE);
        }
        // Neutral mouth
        o.drawFastHLine(58, 44 + bounce, 12, SSD1306_WHITE);
        break;
      }

      case ACT_DRINKING_COFFEE: {
        // Eyes half-closed content
        drawEye(o, lEyeX, eyeY + 5, eyeW, eyeH / 2, 0, 0, false);
        drawEye(o, rEyeX, eyeY + 5, eyeW, eyeH / 2, 0, 0, false);
        // Small smile
        o.drawLine(55, 41 + bounce, 59, 44 + bounce, SSD1306_WHITE);
        o.drawFastHLine(59, 44 + bounce, 10, SSD1306_WHITE);
        o.drawLine(69, 44 + bounce, 73, 41 + bounce, SSD1306_WHITE);
        // Coffee cup
        int cf = animFrame % 4;
        o.drawRect(52, 48 + bounce, 24, 14, SSD1306_WHITE);
        o.drawFastHLine(50, 48 + bounce, 28, SSD1306_WHITE);
        // Handle
        o.drawLine(76, 50 + bounce, 80, 50 + bounce, SSD1306_WHITE);
        o.drawLine(80, 50 + bounce, 80, 58 + bounce, SSD1306_WHITE);
        o.drawLine(76, 58 + bounce, 80, 58 + bounce, SSD1306_WHITE);
        // Steam wisps
        if (cf < 2) {
          o.drawPixel(58, 45 + bounce, SSD1306_WHITE);
          o.drawPixel(64, 44 + bounce, SSD1306_WHITE);
          o.drawPixel(70, 45 + bounce, SSD1306_WHITE);
        }
        break;
      }

      case ACT_EATING: {
        // Chewing animation - mouth opens and closes
        int phase = (animFrame / 2) % 3;
        drawEye(o, lEyeX, eyeY, eyeW, eyeH, 0, 0, blinking);
        drawEye(o, rEyeX, eyeY, eyeW, eyeH, 0, 0, blinking);
        if (phase == 0) {
          // Mouth closed with food bulge in cheek
          o.fillRoundRect(52, 40 + bounce, 24, 6, 3, SSD1306_WHITE);
          o.fillCircle(78, 43 + bounce, 4, SSD1306_WHITE); // food bulge
        } else if (phase == 1) {
          // Mouth slightly open
          o.fillRoundRect(52, 40 + bounce, 24, 8, 3, SSD1306_WHITE);
          o.fillRect(54, 42 + bounce, 20, 4, SSD1306_BLACK);
        } else {
          // Mouth wide open with food
          o.fillRoundRect(52, 38 + bounce, 24, 12, 3, SSD1306_WHITE);
          o.fillRect(54, 40 + bounce, 20, 7, SSD1306_BLACK);
          // Food item (small square)
          o.fillRect(58, 28 + bounce, 8, 8, SSD1306_WHITE);
          o.drawRect(58, 28 + bounce, 8, 8, SSD1306_BLACK);
        }
        // Cheeks for enjoying
        o.drawCircle(30, 38 + bounce, 4, SSD1306_WHITE);
        o.drawCircle(98, 38 + bounce, 4, SSD1306_WHITE);
        break;
      }

      case ACT_YAWNING: {
        // Progressive yawn
        int phase = (animFrame / 3) % 6;
        drawEye(o, lEyeX, eyeY + (phase > 2 ? 4 : 0), eyeW, eyeH, 0, 0,
                phase > 2 ? true : blinking);
        drawEye(o, rEyeX, eyeY + (phase > 2 ? 4 : 0), eyeW, eyeH, 0, 0,
                phase > 2 ? true : blinking);
        // Yawn mouth - opens wide
        int mOpen = (phase < 3) ? phase * 4 : (5 - phase) * 4;
        mOpen = constrain(mOpen, 2, 16);
        o.fillRoundRect(52, 42 + bounce, 24, mOpen, 4, SSD1306_WHITE);
        if (mOpen > 8) {
          o.fillRoundRect(54, 44 + bounce, 20, mOpen - 4, 3, SSD1306_BLACK);
        }
        // Hand covering mouth during peak yawn
        if (phase >= 2 && phase <= 4) {
          o.fillRoundRect(46, 44 + bounce, 36, 8, 4, SSD1306_WHITE);
        }
        break;
      }

      case ACT_ROLLING_EYES: {
        // Eyes rolling (pupils move in circle)
        float angle = (animFrame * 0.4f);
        int pdx = (int)(5 * cosf(angle));
        int pdy = (int)(5 * sinf(angle));
        drawEye(o, lEyeX, eyeY, eyeW, eyeH, pdx, pdy, false);
        drawEye(o, rEyeX, eyeY, eyeW, eyeH, pdx, pdy, false);
        // Flat/annoyed mouth
        o.drawFastHLine(55, 43 + bounce, 18, SSD1306_WHITE);
        o.drawFastHLine(55, 44 + bounce, 18, SSD1306_WHITE);
        // Sweat drop
        o.fillTriangle(88, 20 + bounce, 92, 20 + bounce, 90, 26 + bounce, SSD1306_WHITE);
        break;
      }

      case ACT_WHISTLING: {
        // Eyes relaxed, mouth puckered
        drawEye(o, lEyeX, eyeY, eyeW, eyeH - 4, 0, 0, blinking);
        drawEye(o, rEyeX, eyeY, eyeW, eyeH - 4, 0, 0, blinking);
        // Puckered mouth O
        int msize = 5 + (animFrame % 3);
        o.drawCircle(cx, 44 + bounce, msize, SSD1306_WHITE);
        o.drawCircle(cx, 44 + bounce, msize - 1, SSD1306_WHITE);
        // Music notes floating
        if (animFrame % 6 < 3) {
          o.setCursor(90, 15 + bounce);
          o.setTextSize(1);
          o.print("*");
          o.setCursor(100, 22 + bounce);
          o.print("*");
        }
        break;
      }

      case ACT_POUTY: {
        // Pouty sad-ish face 😒
        // Half-closed eyes looking sideways
        drawEye(o, lEyeX, eyeY + 6, eyeW, eyeH / 2 + 2, -3, 0, false);
        drawEye(o, rEyeX, eyeY + 6, eyeW, eyeH / 2 + 2, -3, 0, false);
        // Pouty mouth - bottom lip sticking out
        o.drawLine(52, 44 + bounce, 58, 46 + bounce, SSD1306_WHITE);
        o.drawFastHLine(58, 46 + bounce, 12, SSD1306_WHITE);
        o.drawLine(70, 46 + bounce, 76, 44 + bounce, SSD1306_WHITE);
        o.drawFastHLine(58, 47 + bounce, 12, SSD1306_WHITE);
        // Little eyebrow furrow lines
        o.drawLine(36, eyeY - 4 + bounce, 54, eyeY - 6 + bounce, SSD1306_WHITE);
        o.drawLine(72, eyeY - 6 + bounce, 90, eyeY - 4 + bounce, SSD1306_WHITE);
        break;
      }

      case ACT_BLANK_STARE: {
        // Completely blank stare 😑
        // Flat line eyes
        o.drawFastHLine(lEyeX, eyeY + eyeH / 2, eyeW, SSD1306_WHITE);
        o.drawFastHLine(lEyeX, eyeY + eyeH / 2 + 1, eyeW, SSD1306_WHITE);
        o.drawFastHLine(rEyeX, eyeY + eyeH / 2, eyeW, SSD1306_WHITE);
        o.drawFastHLine(rEyeX, eyeY + eyeH / 2 + 1, eyeW, SSD1306_WHITE);
        // Flat mouth
        o.drawFastHLine(56, 43 + bounce, 16, SSD1306_WHITE);
        break;
      }

      case ACT_BIG_GRIN: {
        // Big happy grin 😁
        // Wide happy eyes
        for (int t = 0; t < 3; t++) {
          o.drawLine(lEyeX, eyeY + eyeH, lEyeX + eyeW / 2, eyeY + t + 4, SSD1306_WHITE);
          o.drawLine(lEyeX + eyeW / 2, eyeY + t + 4, lEyeX + eyeW, eyeY + eyeH, SSD1306_WHITE);
          o.drawLine(rEyeX, eyeY + eyeH, rEyeX + eyeW / 2, eyeY + t + 4, SSD1306_WHITE);
          o.drawLine(rEyeX + eyeW / 2, eyeY + t + 4, rEyeX + eyeW, eyeY + eyeH, SSD1306_WHITE);
        }
        // Big wide mouth showing teeth
        o.fillRoundRect(46, 38 + bounce, 36, 16, 5, SSD1306_WHITE);
        o.fillRect(48, 40 + bounce, 32, 8, SSD1306_BLACK);
        // Teeth
        for (int t = 0; t < 4; t++) {
          o.fillRect(49 + t * 8, 40 + bounce, 6, 5, SSD1306_WHITE);
        }
        // Rosy cheeks
        for (int d = 0; d < 3; d++) {
          o.drawCircle(28, 38 + bounce, d + 2, SSD1306_WHITE);
          o.drawCircle(100, 38 + bounce, d + 2, SSD1306_WHITE);
        }
        break;
      }

      case ACT_THINKING: {
        // Thinking - eyes looking up-right, hand on chin
        drawEye(o, lEyeX, eyeY, eyeW, eyeH, 3, -3, blinking);
        drawEye(o, rEyeX, eyeY, eyeW, eyeH, 3, -3, blinking);
        // Slight smile
        o.drawLine(55, 43 + bounce, 58, 45 + bounce, SSD1306_WHITE);
        o.drawFastHLine(58, 45 + bounce, 12, SSD1306_WHITE);
        // Hand/fist at chin
        o.fillRoundRect(56, 48 + bounce, 20, 10, 3, SSD1306_WHITE);
        // Thought bubble dots
        int tb = (animFrame / 3) % 4;
        for (int i = 0; i <= tb && i < 3; i++) {
          o.fillCircle(85 + i * 8, 12 + bounce - i * 3, 2 + i, SSD1306_WHITE);
        }
        break;
      }

      case ACT_STRETCHING: {
        // Stretching arms wide
        int phase = (animFrame / 4) % 4;
        drawEye(o, lEyeX, eyeY + 3, eyeW, eyeH - 6, 0, 0, true);
        drawEye(o, rEyeX, eyeY + 3, eyeW, eyeH - 6, 0, 0, true);
        // Squished face (eyes closed)
        // Wide mouth
        int mw = 12 + phase * 4;
        o.drawFastHLine(cx - mw / 2, 44 + bounce, mw, SSD1306_WHITE);
        // Arms stretching out
        int armLen = 10 + phase * 8;
        armLen = constrain(armLen, 10, 40);
        o.drawLine(lEyeX - 2, 36 + bounce, lEyeX - 2 - armLen, 30 + bounce, SSD1306_WHITE);
        o.drawLine(rEyeX + eyeW + 2, 36 + bounce, rEyeX + eyeW + 2 + armLen, 30 + bounce, SSD1306_WHITE);
        // Little hand circles
        o.fillCircle(lEyeX - 2 - armLen, 30 + bounce, 3, SSD1306_WHITE);
        o.fillCircle(rEyeX + eyeW + 2 + armLen, 30 + bounce, 3, SSD1306_WHITE);
        break;
      }

      case ACT_DANCING: {
        // Bouncy dance!
        dancePhase += 0.3f;
        int db = (int)(4 * sinf(dancePhase));
        int db2 = (int)(4 * cosf(dancePhase));
        // Alternating eyes grow/shrink slightly
        int ley = eyeY + (db > 0 ? 0 : 2);
        int rey = eyeY + (db > 0 ? 2 : 0);
        drawEye(o, lEyeX + db / 2, ley, eyeW, eyeH, 0, 0, false);
        drawEye(o, rEyeX + db / 2, rey, eyeW, eyeH, 0, 0, false);
        // Happy mouth
        o.drawLine(52, 43 + bounce + db / 2, 57, 47 + bounce + db / 2, SSD1306_WHITE);
        o.drawFastHLine(57, 47 + bounce + db / 2, 14, SSD1306_WHITE);
        o.drawLine(71, 47 + bounce + db / 2, 76, 43 + bounce + db / 2, SSD1306_WHITE);
        // Music notes
        if ((int)(dancePhase * 2) % 4 < 2) {
          o.setCursor(14, 20);
          o.setTextSize(1);
          o.print("~");
          o.setCursor(106, 20);
          o.print("~");
        }
        break;
      }

      case ACT_WINKING: {
        // Wink - left eye normal, right eye closed with curve
        drawEye(o, lEyeX, eyeY, eyeW, eyeH, 0, 0, false);
        // Right eye wink (curved line)
        for (int i = 0; i < 3; i++) {
          o.drawLine(rEyeX, eyeY + 12 + i, rEyeX + eyeW / 2, eyeY + 6 + i, SSD1306_WHITE);
          o.drawLine(rEyeX + eyeW / 2, eyeY + 6 + i, rEyeX + eyeW, eyeY + 12 + i, SSD1306_WHITE);
        }
        // Cheeky smile
        o.drawLine(54, 42 + bounce, 58, 46 + bounce, SSD1306_WHITE);
        o.drawFastHLine(58, 46 + bounce, 12, SSD1306_WHITE);
        o.drawLine(70, 46 + bounce, 76, 42 + bounce, SSD1306_WHITE);
        break;
      }

      // ---- NEGLECT STATES ----

      case ACT_SAD: {
        // Sad droopy eyes
        // Eyes look down with sad brows
        drawEye(o, lEyeX, eyeY + 4, eyeW, eyeH - 4, 0, 3, blinking);
        drawEye(o, rEyeX, eyeY + 4, eyeW, eyeH - 4, 0, 3, blinking);
        // Sad angled brows
        o.drawLine(lEyeX, eyeY - 4 + bounce, lEyeX + eyeW, eyeY + bounce, SSD1306_WHITE);
        o.drawLine(rEyeX, eyeY + bounce, rEyeX + eyeW, eyeY - 4 + bounce, SSD1306_WHITE);
        // Sad frown
        o.drawLine(52, 46 + bounce, 57, 42 + bounce, SSD1306_WHITE);
        o.drawFastHLine(57, 42 + bounce, 14, SSD1306_WHITE);
        o.drawLine(71, 42 + bounce, 76, 46 + bounce, SSD1306_WHITE);
        // "...?" text
        dm->centerText("...", 56, 1);
        break;
      }

      case ACT_CRYING: {
        // Crying face with falling tears
        // Teary squinted eyes
        // Eye squeezed shut wavy
        for (int i = 0; i < 3; i++) {
          o.drawLine(lEyeX + i * 3, eyeY + 10, lEyeX + i * 3 + 2, eyeY + 7, SSD1306_WHITE);
          o.drawLine(rEyeX + i * 3, eyeY + 10, rEyeX + i * 3 + 2, eyeY + 7, SSD1306_WHITE);
        }
        o.drawFastHLine(lEyeX, eyeY + 10, eyeW, SSD1306_WHITE);
        o.drawFastHLine(rEyeX, eyeY + 10, eyeW, SSD1306_WHITE);
        // Sad brows
        o.drawLine(lEyeX, eyeY - 2 + bounce, lEyeX + eyeW, eyeY + 2 + bounce, SSD1306_WHITE);
        o.drawLine(rEyeX, eyeY + 2 + bounce, rEyeX + eyeW, eyeY - 2 + bounce, SSD1306_WHITE);
        // Tears falling
        tearY1 = eyeY + 14 + (animFrame * 2) % 30;
        tearY2 = eyeY + 14 + (animFrame * 2 + 15) % 30;
        o.fillRoundRect(lEyeX + 6, tearY1 + bounce, 4, 6, 2, SSD1306_WHITE);
        o.fillRoundRect(rEyeX + 6, tearY2 + bounce, 4, 6, 2, SSD1306_WHITE);
        // Big frown
        o.drawLine(50, 48 + bounce, 56, 44 + bounce, SSD1306_WHITE);
        o.drawFastHLine(56, 44 + bounce, 16, SSD1306_WHITE);
        o.drawLine(72, 44 + bounce, 78, 48 + bounce, SSD1306_WHITE);
        // Sob text
        if (animFrame % 6 < 3) {
          dm->centerText("(T_T)", 56, 1);
        }
        break;
      }

      case ACT_SLEEPING: {
        // Sleeping with ZZZ
        // Eyes closed as curved lines
        for (int i = 0; i < 3; i++) {
          o.drawLine(lEyeX + i * 3, eyeY + 9 + bounce, lEyeX + 3 + i * 3, eyeY + 6 + bounce, SSD1306_WHITE);
        }
        for (int i = 0; i < 3; i++) {
          o.drawLine(rEyeX + i * 3, eyeY + 9 + bounce, rEyeX + 3 + i * 3, eyeY + 6 + bounce, SSD1306_WHITE);
        }
        o.drawFastHLine(lEyeX, eyeY + 9 + bounce, eyeW, SSD1306_WHITE);
        o.drawFastHLine(rEyeX, eyeY + 9 + bounce, eyeW, SSD1306_WHITE);
        // Sleepy mouth (small)
        o.drawFastHLine(59, 43 + bounce, 10, SSD1306_WHITE);
        // ZZZ floating up
        if (now - lastZMs > 800) {
          lastZMs = now;
          zCount = (zCount + 1) % 4;
        }
        for (int z = 0; z < zCount; z++) {
          int zx = 78 + z * 7;
          int zy = 20 - z * 7;
          o.setCursor(zx, zy + bounce);
          o.setTextSize(1);
          o.print("z");
        }
        break;
      }

      case ACT_LONELY: {
        // Lonely - looking sideways with a sigh
        drawEye(o, lEyeX, eyeY + 3, eyeW, eyeH - 4, -4, 2, blinking);
        drawEye(o, rEyeX, eyeY + 3, eyeW, eyeH - 4, -4, 2, blinking);
        // Flat sad mouth
        o.drawFastHLine(54, 44 + bounce, 20, SSD1306_WHITE);
        // Sigh lines
        if (animFrame % 8 < 4) {
          o.drawLine(30, 30 + bounce, 20, 20 + bounce, SSD1306_WHITE);
          o.drawLine(20, 20 + bounce, 25, 18 + bounce, SSD1306_WHITE);
        }
        dm->centerText("...", 56, 1);
        break;
      }

      // ---- TOUCH REACTIONS ----

      case ACT_TOUCHED_HAPPY: {
        // Very happy from touch! Bouncy stars
        for (int t = 0; t < 3; t++) {
          o.drawLine(lEyeX, eyeY + eyeH, lEyeX + eyeW / 2, eyeY + t + 3, SSD1306_WHITE);
          o.drawLine(lEyeX + eyeW / 2, eyeY + t + 3, lEyeX + eyeW, eyeY + eyeH, SSD1306_WHITE);
          o.drawLine(rEyeX, eyeY + eyeH, rEyeX + eyeW / 2, eyeY + t + 3, SSD1306_WHITE);
          o.drawLine(rEyeX + eyeW / 2, eyeY + t + 3, rEyeX + eyeW, eyeY + eyeH, SSD1306_WHITE);
        }
        // Big smile
        o.drawLine(48, 43 + bounce, 54, 48 + bounce, SSD1306_WHITE);
        o.drawFastHLine(54, 48 + bounce, 20, SSD1306_WHITE);
        o.drawLine(74, 48 + bounce, 80, 43 + bounce, SSD1306_WHITE);
        // Sparkles
        int sp = animFrame % 8;
        if (sp < 4) {
          o.drawLine(20, 10, 24, 10, SSD1306_WHITE);
          o.drawLine(22, 8, 22, 12, SSD1306_WHITE);
          o.drawLine(100, 18, 104, 18, SSD1306_WHITE);
          o.drawLine(102, 16, 102, 20, SSD1306_WHITE);
        }
        dm->centerText("^_^", 54, 1);
        break;
      }

      case ACT_TOUCHED_BLUSH: {
        // Blushing from touch
        drawEye(o, lEyeX, eyeY + 2, eyeW, eyeH - 4, 0, 0, false);
        drawEye(o, rEyeX, eyeY + 2, eyeW, eyeH - 4, 0, 0, false);
        // Small shy smile
        o.drawLine(56, 44 + bounce, 60, 46 + bounce, SSD1306_WHITE);
        o.drawFastHLine(60, 46 + bounce, 8, SSD1306_WHITE);
        o.drawLine(68, 46 + bounce, 72, 44 + bounce, SSD1306_WHITE);
        // Big blush circles
        for (int r = 2; r <= 6; r++) {
          if (r % 2 == 0) {
            o.drawCircle(26, 38 + bounce, r, SSD1306_WHITE);
            o.drawCircle(102, 38 + bounce, r, SSD1306_WHITE);
          }
        }
        // Shy text
        if (animFrame % 6 < 3) {
          dm->centerText(">////<", 56, 1);
        } else {
          dm->centerText("      ", 56, 1);
        }
        break;
      }

      case ACT_TOUCHED_GIGGLE: {
        // Giggling!
        int gb = (animFrame % 2 == 0) ? 2 : -2;
        for (int t = 0; t < 3; t++) {
          o.drawLine(lEyeX, eyeY + eyeH + gb, lEyeX + eyeW / 2, eyeY + t + 5 + gb, SSD1306_WHITE);
          o.drawLine(lEyeX + eyeW / 2, eyeY + t + 5 + gb, lEyeX + eyeW, eyeY + eyeH + gb, SSD1306_WHITE);
          o.drawLine(rEyeX, eyeY + eyeH + gb, rEyeX + eyeW / 2, eyeY + t + 5 + gb, SSD1306_WHITE);
          o.drawLine(rEyeX + eyeW / 2, eyeY + t + 5 + gb, rEyeX + eyeW, eyeY + eyeH + gb, SSD1306_WHITE);
        }
        // Laugh mouth open
        o.fillRoundRect(50, 39 + bounce + gb, 28, 14, 5, SSD1306_WHITE);
        o.fillRect(52, 41 + bounce + gb, 24, 8, SSD1306_BLACK);
        // Hehe text bouncing
        if (animFrame % 4 < 2) {
          dm->centerText("hehe~", 55, 1);
        }
        break;
      }

      default:
        break;
    }

    o.display();
  }
};

/* ============================ WatchApp ============================== */
class WatchApp {
  DisplayManager* dm;
  TimeManager* tm;
  int lastSecond = -1;
public:
  void begin(DisplayManager* d, TimeManager* t) {
    dm = d; tm = t;
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
  enum GState { G_START, G_PLAYING, G_OVER };
  GState gstate = G_START;
private:
  float birdY = 32, birdV = 0;
  static constexpr int BIRD_X = 24, BIRD_R = 3;
  static constexpr float GRAVITY = 0.28f, FLAP_V = -2.9f;
  static constexpr int NPIPES = 3, PIPE_W = 12, GAP_H = 26, PIPE_SPACING = 52;
  int pipeX[NPIPES], gapY[NPIPES];
  bool passed[NPIPES];
  int score = 0, best = 0;
  unsigned long lastFrameMs = 0;
  bool dirty = true;
public:
  void begin(DisplayManager* d) {
    dm = d;
    prefs.begin("flappy", false);
    best = prefs.getInt("best", 0);
  }
  void onEnter() { gstate = G_START; dirty = true; }
  bool isPlaying() const { return gstate == G_PLAYING; }

  void onSingleTap() {
    if (gstate == G_START)        startGame();
    else if (gstate == G_PLAYING) birdV = FLAP_V;
    else                          startGame();
  }

  void startGame() {
    birdY = 32; birdV = 0; score = 0;
    for (int i = 0; i < NPIPES; i++) {
      pipeX[i] = 128 + i * PIPE_SPACING;
      gapY[i]  = random(10, 64 - 10 - GAP_H);
      passed[i] = false;
    }
    gstate = G_PLAYING;
  }

  void update() {
    unsigned long now = millis();
    if (now - lastFrameMs < 33) return;
    lastFrameMs = now;

    if (gstate == G_START)   { if (dirty) { drawStart(); dirty = false; } return; }
    if (gstate == G_OVER)    { if (dirty) { drawGameOver(); dirty = false; } return; }

    birdV += GRAVITY;
    birdY += birdV;

    for (int i = 0; i < NPIPES; i++) {
      pipeX[i] -= 2;
      if (pipeX[i] + PIPE_W < 0) {
        pipeX[i] = pipeX[(i + NPIPES - 1) % NPIPES] + PIPE_SPACING;
        gapY[i]  = random(10, 64 - 10 - GAP_H);
        passed[i] = false;
      }
      if (!passed[i] && pipeX[i] + PIPE_W < BIRD_X - BIRD_R) {
        passed[i] = true;
        score++;
      }
    }

    bool dead = (birdY - BIRD_R <= 0) || (birdY + BIRD_R >= 63);
    for (int i = 0; i < NPIPES && !dead; i++) {
      bool inX = (BIRD_X + BIRD_R > pipeX[i]) && (BIRD_X - BIRD_R < pipeX[i] + PIPE_W);
      bool inGap = (birdY - BIRD_R > gapY[i]) && (birdY + BIRD_R < gapY[i] + GAP_H);
      if (inX && !inGap) dead = true;
    }
    if (dead) {
      if (score > best) { best = score; prefs.putInt("best", best); }
      gstate = G_OVER; dirty = true;
      return;
    }
    drawGame();
  }

  void drawStart() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    dm->centerText("FLAPPY BIRD", 10, 1);
    o.drawRoundRect(34, 28, 60, 16, 4, SSD1306_WHITE);
    dm->centerText("START", 32, 1);
    dm->centerText("tap to play", 54, 1);
    o.display();
  }

  void drawGame() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    for (int i = 0; i < NPIPES; i++) {
      o.fillRect(pipeX[i], 0, PIPE_W, gapY[i], SSD1306_WHITE);
      o.fillRect(pipeX[i], gapY[i] + GAP_H, PIPE_W, 64 - gapY[i] - GAP_H, SSD1306_WHITE);
    }
    o.drawFastHLine(0, 63, 128, SSD1306_WHITE);
    o.fillCircle(BIRD_X, (int)birdY, BIRD_R, SSD1306_WHITE);
    o.drawPixel(BIRD_X + 2, (int)birdY - 1, SSD1306_BLACK);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", score);
    dm->centerText(buf, 0, 1);
    o.display();
  }

  void drawGameOver() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    dm->centerText("GAME OVER", 8, 1);
    char buf[20];
    snprintf(buf, sizeof(buf), "Score: %d", score);
    dm->centerText(buf, 26, 1);
    snprintf(buf, sizeof(buf), "Best:  %d", best);
    dm->centerText(buf, 38, 1);
    dm->centerText("tap to retry", 54, 1);
    o.display();
  }
};

/* ============================ UpdateApp ============================= */
class UpdateApp {
  DisplayManager* dm;
  enum OtaState { OTA_IDLE, OTA_CONNECTING, OTA_CHECKING, OTA_DOWNLOADING, OTA_FAIL, OTA_UP_TO_DATE };
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
        dm->centerText("Connecting Wi-Fi", 25, 1);
        o.fillRect(14, 45, 100, 4, SSD1306_BLACK);
        o.setCursor(60, 42);
        o.print((now / 500) % 2 == 0 ? "..." : " ");
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
            size_t size = stream->available();
            if (size) {
              int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
              Update.write(buff, c);
              written += c;
              o.clearDisplay();
              dm->centerText("FLASHING UPDATE", 5, 1);
              int progressWidth = map(written, 0, contentLength, 0, 100);
              o.drawRect(14, 28, 100, 10, SSD1306_WHITE);
              o.fillRect(14, 28, progressWidth, 10, SSD1306_WHITE);
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

AppMode mode = MODE_MOCHI;
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

  mochiApp.begin(&displayMgr);
  watchApp.begin(&displayMgr, &timeMgr);
  flappyApp.begin(&displayMgr);
  updateApp.begin(&displayMgr);

  lastInteractionMs = millis();
  enterMode(MODE_MOCHI);
}

void loop() {
  unsigned long now = millis();

  timeMgr.update();
  batteryMgr.update();

  TapEvent ev = touchMgr.update();
  if (ev != TAP_NONE) lastInteractionMs = now;

  if (ev == TAP_DOUBLE) {
    enterMode(mode == MODE_MOCHI  ? MODE_WATCH :
              mode == MODE_WATCH  ? MODE_FLAPPY :
              mode == MODE_FLAPPY ? MODE_UPDATE : MODE_MOCHI);
  } else if (ev == TAP_SINGLE) {
    switch (mode) {
      case MODE_MOCHI:  mochiApp.onSingleTap(); break;
      case MODE_WATCH:  break;
      case MODE_FLAPPY: flappyApp.onSingleTap(); break;
      case MODE_UPDATE: break;
    }
  }

  if (mode == MODE_FLAPPY && flappyApp.isPlaying()) lastInteractionMs = now;
  if (mode == MODE_UPDATE) lastInteractionMs = now;

  if (mode != MODE_MOCHI && (now - lastInteractionMs) >= IDLE_TIMEOUT) {
    lastInteractionMs = now;
    enterMode(MODE_MOCHI);
  }

  switch (mode) {
    case MODE_MOCHI:  mochiApp.update();  break;
    case MODE_WATCH:  watchApp.update();  break;
    case MODE_FLAPPY: flappyApp.update(); break;
    case MODE_UPDATE: updateApp.update(); break;
  }
}

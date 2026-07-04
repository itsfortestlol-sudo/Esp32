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

/* Global compatibility macros to ensure original colors function unaltered */
#define SSD1306_WHITE SH110X_WHITE
#define SSD1306_BLACK SH110X_BLACK

/* ============================ USER CONFIG =========================== */
const char* WIFI_SSID      = "Afia Mahir";
const char* WIFI_PASS      = "afiamahir2026";
const long  GMT_OFFSET_SEC = 21600; // Bangladesh GMT+6
const int   DST_OFFSET_SEC = 0;
const char* NTP_SERVER     = "asia.pool.ntp.org";

// OTA Update Configuration
const int   CURRENT_VERSION = 1;
const char* VERSION_URL     = "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/main.ino";
const char* BIN_URL         = "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/firmware.bin";

/* ------------------------------ Pins ------------------------------- */
#define PIN_OLED_SDA   21
#define PIN_OLED_SCL   22
#define PIN_TOUCH      4
#define PIN_VBAT       34

/* ------------------------- Battery calibration --------------------- */
const float VBAT_DIVIDER   = 2.0f;
const float VBAT_CAL       = 1.00f;
const float VBAT_FULL      = 4.20f;
const float VBAT_EMPTY     = 3.30f;

/* --------------------------- Touch timing -------------------------- */
const unsigned long TOUCH_DEBOUNCE_MS    = 30;
const unsigned long DOUBLE_TAP_WINDOW_MS = 300;

/* ------------------------------ Idle ------------------------------- */
const unsigned long IDLE_TIMEOUT = 20000;

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
  void begin() {
    startSync();
  }
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
      if (now - lastRetryMs > 60000) {
        startSync();
      }
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
  enum Expr { EX_NORMAL, EX_LOOK_L, EX_LOOK_R, EX_HAPPY, EX_SURPRISED };
  Expr expr = EX_NORMAL;
  unsigned long nextExprMs = 0, nextBlinkMs = 0, blinkStartMs = 0;
  unsigned long reactionEndMs = 0, lastFrameMs = 0;
  bool blinking = false, reactionHappy = true;
public:
  void begin(DisplayManager* d) {
    dm = d;
    nextExprMs  = millis() + random(3000, 6000);
    nextBlinkMs = millis() + random(2000, 5000);
  }
  void onEnter() { expr = EX_NORMAL; reactionEndMs = 0; }

  void onSingleTap() {
    reactionHappy = random(0, 2);
    reactionEndMs = millis() + 1500;
  }

  void update() {
    unsigned long now = millis();
    if (now - lastFrameMs < 33) return;
    lastFrameMs = now;
    if (now >= nextExprMs && now >= reactionEndMs) {
      Expr pool[] = { EX_NORMAL, EX_NORMAL, EX_LOOK_L, EX_LOOK_R, EX_HAPPY };
      expr = pool[random(0, 5)];
      nextExprMs = now + random(3000, 6000);
    }
    if (!blinking && now >= nextBlinkMs) { blinking = true; blinkStartMs = now; }
    if (blinking && now - blinkStartMs > 180) {
      blinking = false;
      nextBlinkMs = now + random(2000, 5000);
    }
    draw(now);
  }

  void draw(unsigned long now) {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    int bounce = (int)(2.0f * sinf(now / 400.0f));
    int lookX  = 0;
    Expr e = expr;
    if (now < reactionEndMs) e = reactionHappy ? EX_HAPPY : EX_SURPRISED;
    if (e == EX_LOOK_L) lookX = -8;
    if (e == EX_LOOK_R) lookX =  8;
    int eyeY = 18 + bounce;
    int lx = 34 + lookX, rx = 74 + lookX;
    int eyeW = 20, eyeH = 26;
    if (blinking) {
      float ph = (now - blinkStartMs) / 180.0f;
      float k  = 1.0f - sinf(ph * PI);
      eyeH = max(3, (int)(26 * k));
      eyeY = 18 + bounce + (26 - eyeH) / 2;
    }

    if (e == EX_HAPPY) {
      for (int t = 0; t < 3; t++) {
        o.drawLine(lx, eyeY + 16, lx + 10, eyeY + 6 + t, SSD1306_WHITE);
        o.drawLine(lx + 10, eyeY + 6 + t, lx + 20, eyeY + 16, SSD1306_WHITE);
        o.drawLine(rx, eyeY + 16, rx + 10, eyeY + 6 + t, SSD1306_WHITE);
        o.drawLine(rx + 10, eyeY + 6 + t, rx + 20, eyeY + 16, SSD1306_WHITE);
      }
      o.fillCircle(64 + lookX, 50 + bounce, 4, SSD1306_WHITE);
      o.fillRect(58 + lookX, 44 + bounce, 13, 6, SSD1306_BLACK);
    } else if (e == EX_SURPRISED) {
      o.fillCircle(lx + 10, eyeY + 13, 11, SSD1306_WHITE);
      o.fillCircle(rx + 10, eyeY + 13, 11, SSD1306_WHITE);
      o.drawCircle(64 + lookX, 52 + bounce, 4, SSD1306_WHITE);
    } else {
      o.fillRoundRect(lx, eyeY, eyeW, eyeH, 8, SSD1306_WHITE);
      o.fillRoundRect(rx, eyeY, eyeW, eyeH, 8, SSD1306_WHITE);
      o.fillRoundRect(60 + lookX, 52 + bounce, 8, 2, 1, SSD1306_WHITE);
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

    /* --- Big HH:MM, centered --- */
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    dm->centerText(buf, 20, 3);

    /* --- Seconds, small, under the time --- */
    snprintf(buf, sizeof(buf), ":%02d", t.tm_sec);
    o.setTextSize(1);
    o.setCursor(110, 38);
    o.print(buf);

    /* --- Date DD MMM YYYY --- */
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

          while (http.connected() && written < contentLength) {
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

  if (mode == MODE_FLAPPY && flappyApp.isPlaying()) {
    lastInteractionMs = now;
  }
  if (mode == MODE_UPDATE) {
    lastInteractionMs = now;
  }
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

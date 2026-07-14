/***********************************************************************
 * MochiPod - ESP32 Handheld (Mochi AI / Watch / Flappy / OTA)
 * Board : ESP32 DevKit V1 (ESP-WROOM-32)
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

#include "secrets.h"   // WiFi + Gemini credentials (injected by CI)

#define SSD1306_WHITE SH110X_WHITE
#define SSD1306_BLACK SH110X_BLACK

/* ==================== CONFIG ==================== */
static const long  GMT_OFFSET_SEC = 21600;
static const int   DST_OFFSET_SEC = 0;
static const char* NTP_SERVER     = "asia.pool.ntp.org";

/* OTA */
static const int   CURRENT_VERSION = 4;
static const char* VERSION_URL =
  "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/virsion.txt";
static const char* BIN_URL =
  "https://raw.githubusercontent.com/itsfortestlol-sudo/Esp32/main/firmware.bin";

/* Pins */
#define PIN_OLED_SDA 21
#define PIN_OLED_SCL 22
#define PIN_TOUCH    4
#define PIN_VBAT     34

/* Battery */
static const float VBAT_DIVIDER = 2.0f;
static const float VBAT_CAL     = 1.00f;
static const float VBAT_FULL    = 4.20f;
static const float VBAT_EMPTY   = 3.30f;

/* Touch */
static const unsigned long TOUCH_DEBOUNCE_MS    = 25;
static const unsigned long DOUBLE_TAP_WINDOW_MS = 300;

/* Idle */
static const unsigned long IDLE_TIMEOUT = 20000;

/* AI */
static const bool          ENABLE_GEMINI       = true;
static const unsigned long AI_PLAN_PERIOD_MS   = 60000;
static const unsigned long AI_HTTP_TIMEOUT_MS  = 12000;

/* ==================== Enums ==================== */
enum AppMode  { MODE_MOCHI, MODE_WATCH, MODE_FLAPPY, MODE_UPDATE };
enum TapEvent { TAP_NONE, TAP_SINGLE, TAP_DOUBLE };

/* ==================== DisplayManager ==================== */
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

/* ==================== TouchManager ==================== */
class TouchManager {
  bool stableState   = false;
  bool lastRead      = false;
  unsigned long lastChangeMs = 0;
  unsigned long firstTapMs   = 0;
  bool waitingSecondTap = false;
  bool pressedEdge      = false;

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
        pressedEdge = true;
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

  bool consumePressedEdge() {
    if (!pressedEdge) return false;
    pressedEdge = false;
    return true;
  }
};

/* ==================== BatteryManager ==================== */
class BatteryManager {
  unsigned long lastReadMs = 0;
  float voltage = 3.7f;
  int   percent = 50;

public:
  void begin() { analogSetPinAttenuation(PIN_VBAT, ADC_11db); sample(); }
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

/* ==================== TimeManager ==================== */
class TimeManager {
  enum St { CONNECTING, SYNCING, DONE };
  St state = CONNECTING;
  unsigned long startMs     = 0;
  unsigned long lastRetryMs = 0;
  bool synced = false;

  Preferences prefs;
  int lastYday             = -1;
  int sleepStartMinOfDay   = 22 * 60;
  int wakeMinOfDay         = 8  * 60;

public:
  void begin() {
    prefs.begin("mochiSleep", false);
    startSync();
  }

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
        state   = SYNCING;
        startMs = now;
      } else if (now - startMs > 30000) {
        shutdownWifi();
        lastRetryMs = now;
      }
    } else if (state == SYNCING) {
      if (time(nullptr) > 1700000000UL) {
        synced = true;
        shutdownWifi();
        ensureDailySleepWindow();
      } else if (now - startMs > 15000) {
        shutdownWifi();
        lastRetryMs = now;
      }
    } else {
      if (!synced && (now - lastRetryMs > 60000)) startSync();
      if (synced) ensureDailySleepWindow();
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

  int minuteOfDay() {
    if (!synced) return 0;
    struct tm t; getLocal(t);
    return t.tm_hour * 60 + t.tm_min;
  }

  void ensureDailySleepWindow() {
    if (!synced) return;
    struct tm t; getLocal(t);
    if (t.tm_yday == lastYday) return;
    lastYday = t.tm_yday;

    int storedYday = prefs.getInt("yday", -1);
    if (storedYday == t.tm_yday) {
      sleepStartMinOfDay = prefs.getInt("sleepStart", 22 * 60);
      wakeMinOfDay       = prefs.getInt("wake",       8  * 60);
      return;
    }

    sleepStartMinOfDay = 22 * 60 + random(0, 11);
    wakeMinOfDay       = 8  * 60 + random(0, 60);
    prefs.putInt("yday",       t.tm_yday);
    prefs.putInt("sleepStart", sleepStartMinOfDay);
    prefs.putInt("wake",       wakeMinOfDay);
  }

  bool isSleepTime() {
    if (!synced) return false;
    int m = minuteOfDay();
    return (m >= sleepStartMinOfDay) || (m < wakeMinOfDay);
  }
};

/* ==================== GeminiClient ==================== */
class GeminiClient {
public:
  static String extractJsonObject(const String& s) {
    int a = s.indexOf('{');
    int b = s.lastIndexOf('}');
    if (a < 0 || b < 0 || b <= a) return "";
    return s.substring(a, b + 1);
  }

  static bool wifiConnectQuick(unsigned long timeoutMs) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeoutMs) delay(20);
    return WiFi.status() == WL_CONNECTED;
  }

  static void wifiOff() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  bool getNextMinutePlanJson(String& outPlanJson,
                             const String& localTimeStr,
                             int batteryPct,
                             const String& lastActivity,
                             bool touchedRecently) {
    outPlanJson = "";
    if (!ENABLE_GEMINI) return false;
    if (!GEMINI_API_KEY || strlen(GEMINI_API_KEY) < 10) return false;
    if (!wifiConnectQuick(8000)) { wifiOff(); return false; }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(AI_HTTP_TIMEOUT_MS / 1000);

    HTTPClient http;
    String url = String("https://generativelanguage.googleapis.com/v1beta/models/")
                 + GEMINI_MODEL + ":generateContent?key=" + GEMINI_API_KEY;

    if (!http.begin(client, url)) { wifiOff(); return false; }
    http.setTimeout(AI_HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");

    String prompt;
    prompt.reserve(700);
    prompt += "You are Mochi, a tiny OLED pet.\n";
    prompt += "Return ONLY a minified JSON object (no markdown, no extra text).\n";
    prompt += "Schema:\n";
    prompt += "{\"activity\":\"CHILL|BORED|PHONE|COFFEE|KISSY|SHOCKED\",";
    prompt += "\"text\":\"<=24 chars optional\"}\n";
    prompt += "Rules:\n";
    prompt += "- Pick ONE activity for the next 60 seconds.\n";
    prompt += "- text must be short (<=24 chars), or empty string.\n";
    prompt += "- Do not include any other keys.\n\n";
    prompt += "Context:\n";
    prompt += "- time=" + localTimeStr + "\n";
    prompt += "- battery=" + String(batteryPct) + "%\n";
    prompt += "- last=" + lastActivity + "\n";
    prompt += "- touchedRecently=" + String(touchedRecently ? "true" : "false") + "\n";

    StaticJsonDocument<1024> req;
    JsonArray contents = req["contents"].to<JsonArray>();
    JsonObject c0      = contents.add<JsonObject>();
    c0["role"]         = "user";
    JsonArray parts    = c0["parts"].to<JsonArray>();
    JsonObject p0      = parts.add<JsonObject>();
    p0["text"]         = prompt;

    JsonObject gen         = req["generationConfig"].to<JsonObject>();
    gen["temperature"]     = 0.9;
    gen["maxOutputTokens"] = 120;
    gen["responseMimeType"]= "application/json";

    String body;
    serializeJson(req, body);

    int code = http.POST((uint8_t*)body.c_str(), body.length());
    if (code != HTTP_CODE_OK) { http.end(); wifiOff(); return false; }

    String resp = http.getString();
    http.end();
    wifiOff();

    DynamicJsonDocument wrapper(8192);
    if (deserializeJson(wrapper, resp)) return false;

    const char* txt =
      wrapper["candidates"][0]["content"]["parts"][0]["text"] | nullptr;
    if (!txt) return false;

    String extracted = extractJsonObject(String(txt));
    if (extracted.length() < 2) return false;

    StaticJsonDocument<256> plan;
    if (deserializeJson(plan, extracted)) return false;

    const char* act = plan["activity"] | "";
    const char* t   = plan["text"]     | "";

    String A(act);
    if (!(A=="CHILL"||A=="BORED"||A=="PHONE"||A=="COFFEE"||A=="KISSY"||A=="SHOCKED"))
      return false;

    String T(t);
    if (T.length() > 24) T.remove(24);

    StaticJsonDocument<128> clean;
    clean["activity"] = A;
    clean["text"]     = T;
    serializeJson(clean, outPlanJson);
    return true;
  }
};

/* ==================== MochiApp ==================== */
class MochiApp {
  DisplayManager* dm = nullptr;
  TimeManager*    tm = nullptr;
  BatteryManager* bm = nullptr;
  GeminiClient gemini;

  enum Activity {
    ACT_CHILL, ACT_BORED, ACT_PHONE, ACT_COFFEE,
    ACT_KISSY, ACT_SHOCKED, ACT_SLEEPING, ACT_ANNOYED_WAKE
  };

  Activity activity     = ACT_CHILL;
  Activity planned      = ACT_CHILL;
  Activity prevActivity = ACT_CHILL;

  unsigned long lastFrameMs    = 0;
  unsigned long activityEndMs  = 0;
  unsigned long nextAiMs       = 0;

  String aiText              = "";
  String lastPlanActivityStr = "CHILL";
  bool   touchedRecently     = false;
  unsigned long touchedRecentlyUntil = 0;

  bool blinking = false;
  unsigned long blinkStartMs = 0;
  unsigned long nextBlinkMs  = 0;

  float bouncePhase = 0.0f;

  struct ZzzParticle { float x, y, vy; int size; bool active; };
  static const int ZZZ_MAX = 4;
  ZzzParticle zzz[ZZZ_MAX];
  unsigned long nextZzzMs = 0;

  int shakeOffset = 0;

  float coffeeSip = 0.0f;
  bool  sipping   = false;
  unsigned long sipEndMs = 0;

  int  phoneScrollY  = 0;
  unsigned long nextScrollMs = 0;

public:
  void begin(DisplayManager* d, TimeManager* t, BatteryManager* b) {
    dm = d; tm = t; bm = b;
    initZzz();
    nextBlinkMs = millis() + random(2000, 5000);
    nextAiMs    = millis() + 2000;
  }

  void onEnter() {
    activity = ACT_CHILL;
    planned  = ACT_CHILL;
    aiText   = "";
    nextAiMs = millis() + 500;
  }

  void onSingleTap() {
    unsigned long now = millis();
    touchedRecently        = true;
    touchedRecentlyUntil   = now + 60000;
    if (activity == ACT_SLEEPING) {
      setActivity(ACT_ANNOYED_WAKE, 3000);
    } else {
      prevActivity = activity;
      setActivity(ACT_SHOCKED, 900);
    }
  }

  void update() {
    unsigned long now = millis();
    if (now - lastFrameMs < 33) return;
    lastFrameMs = now;

    if (touchedRecently && now > touchedRecentlyUntil) touchedRecently = false;

    if (tm->isSleepTime()) {
      if (activity != ACT_SLEEPING && activity != ACT_ANNOYED_WAKE)
        setActivity(ACT_SLEEPING, 0);
    } else {
      if (activity == ACT_SLEEPING) setActivity(planned, 0);
    }

    if (!tm->isSleepTime() && ENABLE_GEMINI && now >= nextAiMs) {
      nextAiMs = now + AI_PLAN_PERIOD_MS;
      if (tm->isSynced()) {
        struct tm lt; tm->getLocal(lt);
        char tbuf[24];
        strftime(tbuf, sizeof(tbuf), "%H:%M", &lt);
        String planJson;
        if (gemini.getNextMinutePlanJson(planJson, String(tbuf),
              bm->getPercent(), lastPlanActivityStr, touchedRecently))
          applyPlan(planJson);
      }
    }

    if (activityEndMs > 0 && now >= activityEndMs) {
      activityEndMs = 0;
      if (activity == ACT_ANNOYED_WAKE)  setActivity(ACT_SLEEPING, 0);
      else                                setActivity(planned, 0);
    }

    if (activity != ACT_SLEEPING && activity != ACT_ANNOYED_WAKE && activity != ACT_SHOCKED) {
      if (!blinking && now >= nextBlinkMs) { blinking = true; blinkStartMs = now; }
      if (blinking && (now - blinkStartMs) > 160) {
        blinking   = false;
        nextBlinkMs = now + random(2000, 5000);
      }
    } else blinking = false;

    bouncePhase += 0.020f;
    if (bouncePhase > TWO_PI) bouncePhase -= TWO_PI;

    if (activity == ACT_COFFEE) {
      if (!sipping && now >= sipEndMs) { sipping = true; sipEndMs = now + 800; }
      else if (sipping && now >= sipEndMs) {
        sipping   = false;
        sipEndMs  = now + random(1500, 3000);
        coffeeSip = 0.0f;
      }
      if (sipping) {
        float prog = 1.0f - (float)(sipEndMs - now) / 800.0f;
        coffeeSip  = sinf(prog * PI);
      }
    }

    if (activity == ACT_PHONE && now >= nextScrollMs) {
      phoneScrollY  = random(0, 8);
      nextScrollMs  = now + random(600, 1400);
    }

    if (activity == ACT_SLEEPING || activity == ACT_ANNOYED_WAKE) updateZzz(now);
    shakeOffset = (activity == ACT_ANNOYED_WAKE)
                  ? ((random(0,2)==0) ? random(-3,3) : 0) : 0;

    draw(now);
  }

private:
  void applyPlan(const String& planJson) {
    StaticJsonDocument<128> plan;
    if (deserializeJson(plan, planJson)) return;
    String act = plan["activity"] | "CHILL";
    String txt = plan["text"]     | "";
    lastPlanActivityStr = act;
    aiText = txt;
    if (aiText.length() > 24) aiText.remove(24);
    planned = strToActivity(act);
    if (activity != ACT_SLEEPING && activity != ACT_ANNOYED_WAKE && activity != ACT_SHOCKED)
      setActivity(planned, 0);
  }

  Activity strToActivity(const String& s) {
    if (s == "BORED")   return ACT_BORED;
    if (s == "PHONE")   return ACT_PHONE;
    if (s == "COFFEE")  return ACT_COFFEE;
    if (s == "KISSY")   return ACT_KISSY;
    if (s == "SHOCKED") return ACT_SHOCKED;
    return ACT_CHILL;
  }

  void setActivity(Activity a, unsigned long durationMs) {
    activity      = a;
    activityEndMs = (durationMs > 0) ? millis() + durationMs : 0;
    coffeeSip     = 0.0f;
    sipping       = false;
    sipEndMs      = millis() + 1200;
    phoneScrollY  = 0;
    nextScrollMs  = millis() + 600;
    initZzz();
    nextZzzMs = millis();
  }

  void initZzz() {
    for (int i = 0; i < ZZZ_MAX; i++) {
      zzz[i] = {80.0f, 20.0f, -0.4f, 1, false};
    }
  }

  void spawnZzz(unsigned long now) {
    if (now < nextZzzMs) return;
    nextZzzMs = now + 900;
    for (int i = 0; i < ZZZ_MAX; i++) {
      if (!zzz[i].active) {
        zzz[i].active = true;
        zzz[i].x      = 82.0f + random(-3, 4);
        zzz[i].y      = 22.0f;
        zzz[i].vy     = -0.35f - random(0, 3) * 0.05f;
        zzz[i].size   = (i % 2 == 0) ? 1 : 2;
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

  void drawPhone(Adafruit_SH1106G& o, int px, int py) {
    o.drawRoundRect(px, py, 16, 22, 2, SSD1306_WHITE);
    o.fillRect(px+2, py+2, 12, 16, SSD1306_WHITE);
    int lineY = py + 3 + phoneScrollY;
    for (int l = 0; l < 3; l++) {
      int ly = lineY + l * 4;
      if (ly >= py+2 && ly < py+17) o.drawFastHLine(px+3, ly, 10, SSD1306_BLACK);
    }
    o.drawCircle(px+8, py+19, 1, SSD1306_WHITE);
  }

  void drawCoffeeCup(Adafruit_SH1106G& o, int cx, int cy, float sip) {
    int tiltX = (int)(sip * 5.0f);
    int tiltY = (int)(sip * 2.0f);
    o.drawLine(cx,          cy+5-tiltY, cx+2,          cy+13, SSD1306_WHITE);
    o.drawLine(cx+12-tiltX, cy+5-tiltY, cx+10,         cy+13, SSD1306_WHITE);
    o.drawFastHLine(cx+2, cy+13, 8, SSD1306_WHITE);
    o.drawFastHLine(cx,   cy+5-tiltY, 12-tiltX, SSD1306_WHITE);
    o.drawLine(cx+12-tiltX, cy+7-tiltY, cx+14-tiltX, cy+7-tiltY, SSD1306_WHITE);
    o.drawLine(cx+14-tiltX, cy+7-tiltY, cx+14-tiltX, cy+11,      SSD1306_WHITE);
    o.drawLine(cx+14-tiltX, cy+11,      cx+10,        cy+11,      SSD1306_WHITE);
    if (sip < 0.3f) {
      o.drawPixel(cx+4, cy+3, SSD1306_WHITE);
      o.drawPixel(cx+7, cy+2, SSD1306_WHITE);
      o.drawPixel(cx+10,cy+3, SSD1306_WHITE);
    }
  }

  void drawFaceNeutral(Adafruit_SH1106G& o, int lx, int rx, int eyeY, int bounce, int faceX) {
    int eyeW = 20, eyeH = 26;
    if (blinking) {
      float ph = (millis() - blinkStartMs) / 160.0f;
      float k  = 1.0f - sinf(ph * PI);
      eyeH = max(3, (int)(26 * k));
      eyeY = eyeY + (26 - eyeH) / 2;
    }
    o.fillRoundRect(lx, eyeY, eyeW, eyeH, 8, SSD1306_WHITE);
    o.fillRoundRect(rx, eyeY, eyeW, eyeH, 8, SSD1306_WHITE);
    o.fillRect(lx+3, eyeY+3, 4, 3, SSD1306_BLACK);
    o.fillRect(rx+3, eyeY+3, 4, 3, SSD1306_BLACK);
    o.fillRoundRect(faceX-4, 52+bounce, 8, 2, 1, SSD1306_WHITE);
  }

  void drawFaceBored(Adafruit_SH1106G& o, int lx, int rx, int eyeY, int bounce, int faceX) {
    int eyeW = 20, eyeH = 26;
    o.fillRoundRect(lx, eyeY, eyeW, eyeH, 8, SSD1306_WHITE);
    o.fillRoundRect(rx, eyeY, eyeW, eyeH, 8, SSD1306_WHITE);
    o.fillRect(lx, eyeY, eyeW, eyeH/3, SSD1306_BLACK);
    o.fillRect(rx, eyeY, eyeW, eyeH/3, SSD1306_BLACK);
    o.drawFastHLine(lx, eyeY+eyeH/3, eyeW, SSD1306_WHITE);
    o.drawFastHLine(rx, eyeY+eyeH/3, eyeW, SSD1306_WHITE);
    o.drawLine(faceX-6, 53+bounce, faceX-2, 55+bounce, SSD1306_WHITE);
    o.drawLine(faceX-2, 55+bounce, faceX+6, 53+bounce, SSD1306_WHITE);
  }

  void drawFaceKissy(Adafruit_SH1106G& o, int lx, int rx, int eyeY, int bounce, int faceX) {
    for (int t = 0; t < 3; t++) {
      o.drawLine(lx,    eyeY+18, lx+10, eyeY+8+t, SSD1306_WHITE);
      o.drawLine(lx+10, eyeY+8+t, lx+20, eyeY+18, SSD1306_WHITE);
      o.drawLine(rx,    eyeY+18, rx+10, eyeY+8+t, SSD1306_WHITE);
      o.drawLine(rx+10, eyeY+8+t, rx+20, eyeY+18, SSD1306_WHITE);
    }
    o.drawCircle(faceX, 52+bounce, 4, SSD1306_WHITE);
  }

  void drawFaceShocked(Adafruit_SH1106G& o, int lx, int rx, int eyeY, int bounce, int faceX) {
    o.fillCircle(lx+10, eyeY+13, 12, SSD1306_WHITE);
    o.fillCircle(rx+10, eyeY+13, 12, SSD1306_WHITE);
    o.fillCircle(lx+10, eyeY+13,  5, SSD1306_BLACK);
    o.fillCircle(rx+10, eyeY+13,  5, SSD1306_BLACK);
    o.fillCircle(lx+8,  eyeY+11,  2, SSD1306_WHITE);
    o.fillCircle(rx+8,  eyeY+11,  2, SSD1306_WHITE);
    o.fillRoundRect(faceX-8, 49+bounce, 16, 11, 4, SSD1306_WHITE);
    o.fillRoundRect(faceX-5, 51+bounce, 10,  7, 2, SSD1306_BLACK);
  }

  void drawFaceSleeping(Adafruit_SH1106G& o, int lx, int rx, int eyeY, int bounce, int faceX) {
    for (int t = 0; t < 2; t++) {
      o.drawLine(lx,    eyeY+13, lx+10, eyeY+5+t, SSD1306_WHITE);
      o.drawLine(lx+10, eyeY+5+t, lx+20, eyeY+13, SSD1306_WHITE);
      o.drawLine(rx,    eyeY+13, rx+10, eyeY+5+t, SSD1306_WHITE);
      o.drawLine(rx+10, eyeY+5+t, rx+20, eyeY+13, SSD1306_WHITE);
    }
    o.drawLine(faceX-5, 52+bounce, faceX,   50+bounce, SSD1306_WHITE);
    o.drawLine(faceX,   50+bounce, faceX+5, 52+bounce, SSD1306_WHITE);
    o.drawCircle(8, 8, 6, SSD1306_WHITE);
    o.fillCircle(11, 6, 5, SSD1306_BLACK);
  }

  void draw(unsigned long /*now*/) {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();

    int bounce   = (int)(2.0f * sinf(bouncePhase));
    int faceX    = 64 + shakeOffset;
    int eyeBaseY = 18 + bounce;
    int lx       = faceX - 30;
    int rx       = faceX + 10;

    switch (activity) {
      case ACT_SLEEPING:
        drawFaceSleeping(o, lx, rx, eyeBaseY, bounce, faceX);
        drawZzz(o);
        break;
      case ACT_ANNOYED_WAKE:
        drawFaceShocked(o, lx, rx, eyeBaseY, bounce, faceX);
        for (int row = 0; row < 64; row += 4)
          o.drawFastHLine(0, row, 128, SSD1306_BLACK);
        o.setTextSize(2);
        o.setCursor(faceX-10, 2);
        o.print("!!");
        o.setTextSize(1);
        drawZzz(o);
        break;
      case ACT_SHOCKED:
        drawFaceShocked(o, lx, rx, eyeBaseY, bounce, faceX);
        break;
      case ACT_KISSY:
        drawFaceKissy(o, lx, rx, eyeBaseY, bounce, faceX);
        break;
      case ACT_BORED:
        drawFaceBored(o, lx, rx, eyeBaseY, bounce, faceX);
        break;
      default:
        drawFaceNeutral(o, lx, rx, eyeBaseY, bounce, faceX);
        break;
    }

    if (activity == ACT_PHONE)  drawPhone(o, faceX+18, 20+bounce);
    if (activity == ACT_COFFEE) drawCoffeeCup(o, faceX+20, 28+bounce, coffeeSip);

    if (!tm->isSleepTime() && aiText.length() > 0) {
      o.setTextSize(1);
      o.setCursor(2, 56);
      o.print(aiText);
    }

    o.display();
  }
};

/* ==================== WatchApp ==================== */
class WatchApp {
  DisplayManager* dm = nullptr;
  TimeManager*    tm = nullptr;
  BatteryManager* bm = nullptr;
  int lastSecond = -1;

public:
  void begin(DisplayManager* d, TimeManager* t, BatteryManager* b) { dm=d; tm=t; bm=b; }
  void onEnter() { lastSecond = -1; }

  void update() {
    struct tm t; tm->getLocal(t);
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
    o.fillRect(bx+18, by+2, 2, 4, SSD1306_WHITE);
    int fillW = map(pct, 0, 100, 0, 14);
    if (fillW > 0) o.fillRect(bx+2, by+2, fillW, 4, SSD1306_WHITE);
    snprintf(buf, sizeof(buf), "%d%%", pct);
    o.setTextSize(1);
    o.setCursor(bx - (int)(6*strlen(buf)) - 3, by+1);
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

/* ==================== FlappyApp ==================== */
class FlappyApp {
  DisplayManager* dm = nullptr;
  Preferences prefs;

public:
  enum GState { G_START, G_PLAYING, G_OVER };
  GState gstate = G_START;

private:
  float birdY = 32.0f;
  float birdV = 0.0f;

  static constexpr int   BIRD_X      = 22;
  static constexpr int   BIRD_R      = 4;
  static constexpr float GRAVITY     = 0.16f;
  static constexpr float FLAP_V      = -2.35f;
  static constexpr float PIPE_SPEED  = 1.2f;
  static constexpr int   NPIPES      = 3;
  static constexpr int   PIPE_W      = 14;
  static constexpr int   GAP_H       = 44;
  static constexpr int   PIPE_SPACING= 62;

  float pipeX[NPIPES];
  int   gapY[NPIPES];
  bool  passed[NPIPES];

  int  score = 0;
  int  best  = 0;
  bool dirty = true;

  unsigned long lastFrameMs     = 0;
  unsigned long startGraceUntil = 0;
  float wingPhase = 0.0f;

public:
  void begin(DisplayManager* d) {
    dm = d;
    prefs.begin("flappy", false);
    best = prefs.getInt("best", 0);
  }

  void onEnter()        { gstate = G_START; dirty = true; }
  bool isPlaying()      const { return gstate == G_PLAYING; }
  bool onStartScreen()  const { return gstate == G_START;   }

  void onSingleTap() {
    if (gstate == G_START || gstate == G_OVER) startGame(true);
  }

  void onPressFlap() {
    if (gstate == G_PLAYING) { birdV = FLAP_V; wingPhase = 0.0f; }
  }

private:
  void startGame(bool giveInitialFlap) {
    birdY = 32.0f;
    birdV = giveInitialFlap ? (FLAP_V * 0.8f) : 0.0f;
    score = 0; wingPhase = 0.0f;
    for (int i = 0; i < NPIPES; i++) {
      pipeX[i]  = 128.0f + i * PIPE_SPACING;
      int maxStart = max(8, 64 - 8 - GAP_H);
      gapY[i]   = random(8, maxStart + 1);
      passed[i] = false;
    }
    gstate          = G_PLAYING;
    startGraceUntil = millis() + 900;
  }

public:
  void update() {
    unsigned long now = millis();
    if (now - lastFrameMs < 33) return;
    lastFrameMs = now;

    if (gstate == G_START) { if (dirty) { drawStart();    dirty = false; } return; }
    if (gstate == G_OVER)  { if (dirty) { drawGameOver(); dirty = false; } return; }

    birdV      += GRAVITY;
    birdY      += birdV;
    wingPhase  += 0.18f;
    if (wingPhase > TWO_PI) wingPhase -= TWO_PI;

    for (int i = 0; i < NPIPES; i++) {
      pipeX[i] -= PIPE_SPEED;
      if (pipeX[i] + PIPE_W < 0) {
        float maxX = pipeX[0];
        for (int j = 1; j < NPIPES; j++) if (pipeX[j] > maxX) maxX = pipeX[j];
        pipeX[i]  = maxX + PIPE_SPACING;
        int maxStart = max(8, 64 - 8 - GAP_H);
        gapY[i]   = random(8, maxStart + 1);
        passed[i] = false;
      }
      if (!passed[i] && pipeX[i] + PIPE_W < BIRD_X - BIRD_R) {
        passed[i] = true; score++;
      }
    }

    bool dead = false;
    if (millis() > startGraceUntil) {
      dead = (birdY - BIRD_R <= 1) || (birdY + BIRD_R >= 62);
      for (int i = 0; i < NPIPES && !dead; i++) {
        bool inX  = ((float)(BIRD_X+BIRD_R) > pipeX[i]) &&
                    ((float)(BIRD_X-BIRD_R) < pipeX[i]+PIPE_W);
        bool inGap= ((birdY-BIRD_R) > gapY[i]) &&
                    ((birdY+BIRD_R) < gapY[i]+GAP_H);
        if (inX && !inGap) dead = true;
      }
    }

    if (dead) {
      if (score > best) { best = score; prefs.putInt("best", best); }
      gstate = G_OVER; dirty = true; return;
    }

    drawGame();
  }

private:
  void drawPipe(Adafruit_SH1106G& o, int px, int topH, int botY) {
    if (topH > 4) o.fillRect(px+1, 0, PIPE_W-2, topH-4, SSD1306_WHITE);
    o.fillRect(px-1, topH-4, PIPE_W+2, 4, SSD1306_WHITE);
    o.drawFastVLine(px+2, 0, max(0, topH-4), SSD1306_BLACK);
    o.fillRect(px-1, botY, PIPE_W+2, 4, SSD1306_WHITE);
    int bodyH = 63-(botY+4);
    if (bodyH > 0) o.fillRect(px+1, botY+4, PIPE_W-2, bodyH, SSD1306_WHITE);
    o.drawFastVLine(px+2, botY+4, max(0,bodyH), SSD1306_BLACK);
  }

  void drawBird(Adafruit_SH1106G& o, int bx, int by) {
    o.fillCircle(bx, by, BIRD_R, SSD1306_WHITE);
    int wingOff = (int)(2.0f * sinf(wingPhase));
    o.fillRoundRect(bx-BIRD_R-2, by-1+wingOff, 6, 3, 1, SSD1306_WHITE);
    o.fillTriangle(bx+BIRD_R-1, by-1, bx+BIRD_R+3, by, bx+BIRD_R-1, by+2, SSD1306_WHITE);
    o.drawPixel(bx+2, by-2, SSD1306_BLACK);
    o.drawLine(bx-BIRD_R, by+1, bx-BIRD_R-3, by-1, SSD1306_WHITE);
  }

  void drawStart() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    o.fillRoundRect(10, 4, 108, 16, 4, SSD1306_WHITE);
    o.setTextColor(SSD1306_BLACK);
    dm->centerText("FLAPPY MOCHI", 8, 1);
    o.setTextColor(SSD1306_WHITE);
    o.fillRect(0,   0, 6, 20, SSD1306_WHITE);
    o.fillRect(122, 0, 6, 20, SSD1306_WHITE);
    o.fillRect(0,  48, 6, 16, SSD1306_WHITE);
    o.fillRect(122,48, 6, 16, SSD1306_WHITE);
    drawBird(o, 64, 38);
    o.drawFastHLine(0, 62, 128, SSD1306_WHITE);
    o.drawFastHLine(0, 63, 128, SSD1306_WHITE);
    dm->centerText("1 Tap: Start",  48, 1);
    dm->centerText("2 Tap: Switch", 56, 1);
    o.display();
  }

  void drawGame() {
    Adafruit_SH1106G& o = dm->oled;
    o.clearDisplay();
    for (int i = 0; i < NPIPES; i++)
      drawPipe(o, (int)pipeX[i], gapY[i], gapY[i]+GAP_H);
    o.drawFastHLine(0, 62, 128, SSD1306_WHITE);
    o.drawFastHLine(0, 63, 128, SSD1306_WHITE);
    for (int x = 0; x < 128; x += 4) o.drawPixel(x, 61, SSD1306_WHITE);
    drawBird(o, BIRD_X, (int)birdY);
    char buf[8]; snprintf(buf, sizeof(buf), "%d", score);
    int sw = (int)(strlen(buf)*6+6);
    o.drawRoundRect((128-sw)/2, 1, sw, 10, 2, SSD1306_WHITE);
    dm->centerText(buf, 3, 1);
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
    o.drawFastHLine(20, 34, 88, SSD1306_WHITE);
    dm->centerText("Tap to retry", 52, 1);
    o.display();
  }
};

/* ==================== UpdateApp ==================== */
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
        if (WiFi.status() == WL_CONNECTED)       state = OTA_CHECKING;
        else if (now - statusTimer > 20000)       fail("Wi-Fi Timeout");
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
        dm->centerText("Dbl Tap to Exit",  50, 1);
        o.display();
        break;

      case OTA_UP_TO_DATE:
        o.clearDisplay();
        dm->centerText("SYSTEM OK", 8, 1);
        o.drawFastHLine(10, 20, 108, SSD1306_WHITE);
        dm->centerText("No New Version",   28, 1);
        dm->centerText("Dbl Tap to Exit",  50, 1);
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
            dm->centerText("SUCCESS!",     10, 1);
            dm->centerText("Rebooting...", 35, 1);
            o.display();
            delay(2000);
            ESP.restart();
          } else fail("Flash Error");
        } else fail("No Space");
      } else fail("Bin HTTP Err: " + String(code));
      http.end();
    } else fail("Bin Link Fail");
  }
};

/* ==================== Globals ==================== */
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

static AppMode nextModeCircular(AppMode m) {
  return (m == MODE_MOCHI)  ? MODE_WATCH  :
         (m == MODE_WATCH)  ? MODE_FLAPPY :
         (m == MODE_FLAPPY) ? MODE_UPDATE :
         MODE_MOCHI;
}

/* ==================== Setup ==================== */
void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  if (!displayMgr.begin())
    Serial.println("SH1106 init failed");

  touchMgr.begin();
  batteryMgr.begin();
  timeMgr.begin();

  mochiApp.begin(&displayMgr, &timeMgr, &batteryMgr);
  watchApp.begin(&displayMgr, &timeMgr, &batteryMgr);
  flappyApp.begin(&displayMgr);
  updateApp.begin(&displayMgr);

  lastInteractionMs = millis();
  enterMode(MODE_MOCHI);
}

/* ==================== Loop ==================== */
void loop() {
  unsigned long now = millis();

  timeMgr.update();
  batteryMgr.update();

  TapEvent ev        = touchMgr.update();
  bool     pressEdge = touchMgr.consumePressedEdge();

  if (mode == MODE_FLAPPY && flappyApp.isPlaying() && pressEdge) {
    flappyApp.onPressFlap();
    lastInteractionMs = now;
  }

  if (ev != TAP_NONE) lastInteractionMs = now;

  if (ev == TAP_DOUBLE) {
    if (!(mode == MODE_FLAPPY && !flappyApp.onStartScreen()))
      enterMode(nextModeCircular(mode));
  } else if (ev == TAP_SINGLE) {
    switch (mode) {
      case MODE_MOCHI:  mochiApp.onSingleTap();  break;
      case MODE_FLAPPY: flappyApp.onSingleTap(); break;
      default: break;
    }
  }

  if (mode == MODE_FLAPPY && flappyApp.isPlaying()) lastInteractionMs = now;
  if (mode == MODE_UPDATE)                          lastInteractionMs = now;

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

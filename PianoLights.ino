/*
 * ==================================================================================================
 *  PianoLights.ino, a BLE/MIDI bridge between Synthesia and a LED strip
 * ==================================================================================================
 *  Target     : ESP32 WROOM ("ESP32 Dev Module")
 *  Role       : BLE/MIDI peripheral (seen as a MIDI output/input by Synthesia on Windows)
 *  Control    : Incoming notes drive a WS2812B LED strip to show which keys to play
 *  Feedback   : An INMP441 mic listens to an acoustic piano and reports the notes actually played
 *  Config     : Embedded web page (WiFi STA with automatic AP fallback), preferences stored in NVS
 *
 *  Libraries required under the Arduino IDE:
 *    - MIDI Library        (Francois Best)
 *    - BLE-MIDI            (lathoub, must be patched)
 *    - NimBLE-Arduino      (h2zero)
 *    - FastLED             (Daniel Garcia)
 *    - ArduinoJson         (Benoit Blanchon)
 *    - ESP Async WebServer (ESP32Async)
 *    - Async TCP           (ESP32Async)
 *
 *  Partition Scheme : "Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)"
 * ==================================================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <FastLED.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <Update.h>

#include <BLEMIDI_Transport.h>
#include <hardware/BLEMIDI_ESP32_NimBLE.h>

#include "PianoLights.h"
#include "PianoLightsMic.h"

// ---------
// Constants
// ---------
#define FW_VERSION              "1.3"
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_RETRY_INTERVAL_MS  30000
#define WIFI_AP_SSID            "Piano-Lights-AP"   // AP-mode SSID
#define MDNS_HOSTNAME           "pianolights"       // access via http://pianolights.local
#define BLE_DEVICE_NAME         "Piano-Lights"      // name shown under Windows
#define LED_MAX_COUNT           300                 // max allocated buffer
#define LED_FRAME_INTERVAL_MS   15                  // ~60 fps max

// Allowed pins for the LED strip and the microphone
static const uint8_t LED_PIN_WHITELIST[] = {5, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
static const uint8_t MIC_PIN_WHITELIST[] = {34, 35, 36, 39};

// -----------
// Preferences
// -----------
struct Config {
  // Geometry
  uint8_t  keyCount    = 88;       // number of keyboard keys
  uint8_t  firstNote   = 21;       // MIDI note of the 1st key (21 = A0)
  float    ledsPerKey  = 2.0f;     // LEDs-per-key ratio
  int16_t  ledOffset   = 0;        // index of the 1st usable LED
  bool     reversed    = false;    // strip direction
  uint8_t  ledPin      = 16;       // LED data (DOUT)
  // Colors
  uint32_t colorLeft   = 0x00A0FF; // left hand
  uint32_t colorRight  = 0xFF4000; // right hand
  uint32_t colorOther  = 0x00FF60; // any other channel
  uint8_t  chLeft      = 1;        // left-hand MIDI channel (1-16)
  uint8_t  chRight     = 2;        // right-hand MIDI channel (1-16)
  uint8_t  brightness  = 100;      // global brightness (5-255)
  // Microphone
  bool     micEnabled  = false;
  uint8_t  micGain     = 16;       // digital gain (1-64)
  uint8_t  micThreshold= 40;       // detection sensitivity (1-100)
  uint8_t  micPinSck   = 21;       // I2S bit clock (SCK/BCLK)
  uint8_t  micPinCh    = 5;        // I2S word select (WS/LRCLK)
  uint8_t  micPinData  = 26;       // I2S data (SD/DOUT)
  // WiFi
  char     ssid[33]    = "";
  char     pass[65]    = "";
};

Config       cfg;
Preferences  prefs;

// ------------
// Global state
// ------------
BLEMIDI_CREATE_INSTANCE(BLE_DEVICE_NAME, MIDI)

CRGB              leds[LED_MAX_COUNT];
AsyncWebServer    server(80);

volatile uint8_t  noteChan[128] = {0};    // 0 = off, otherwise MIDI channel 1-16
volatile bool     ledsDirty     = true;
volatile bool     bleConnected  = false;
volatile bool     otaInProgress = false;  // freezes LED rendering while writing to flash

bool      apMode        = false;
uint32_t  rebootAt      = 0;              // != 0 -> scheduled reboot
uint32_t  lastWifiRetry = 0;
uint32_t  lastShow      = 0;

// -------------
// Storage (NVS)
// -------------
void loadConfig() {
  prefs.begin("pianolights", true);
  cfg.keyCount    = prefs.getUChar("kc",   cfg.keyCount);
  cfg.firstNote   = prefs.getUChar("fn",   cfg.firstNote);
  cfg.ledsPerKey  = prefs.getFloat("lpn",  cfg.ledsPerKey);
  cfg.ledOffset   = prefs.getShort("off",  cfg.ledOffset);
  cfg.reversed    = prefs.getBool("rev",   cfg.reversed);
  cfg.ledPin      = prefs.getUChar("pin",  cfg.ledPin);
  cfg.colorLeft   = prefs.getUInt("cl",    cfg.colorLeft);
  cfg.colorRight  = prefs.getUInt("cr",    cfg.colorRight);
  cfg.colorOther  = prefs.getUInt("co",    cfg.colorOther);
  cfg.chLeft      = prefs.getUChar("chl",  cfg.chLeft);
  cfg.chRight     = prefs.getUChar("chr",  cfg.chRight);
  cfg.brightness  = prefs.getUChar("bri",  cfg.brightness);
  cfg.micEnabled  = prefs.getBool ("men",  cfg.micEnabled);
  cfg.micGain     = prefs.getUChar("mgn",  cfg.micGain);
  cfg.micThreshold= prefs.getUChar("mth",  cfg.micThreshold);
  cfg.micPinSck   = prefs.getUChar("msck", cfg.micPinSck);
  cfg.micPinCh    = prefs.getUChar("mws",  cfg.micPinCh);
  cfg.micPinData  = prefs.getUChar("msd",  cfg.micPinData);
  prefs.getString("ssid", cfg.ssid, sizeof(cfg.ssid));
  prefs.getString("pass", cfg.pass, sizeof(cfg.pass));
  prefs.end();
}

void saveConfig() {
  prefs.begin("pianolights", false);
  prefs.putUChar("kc",    cfg.keyCount);
  prefs.putUChar("fn",    cfg.firstNote);
  prefs.putFloat("lpn",   cfg.ledsPerKey);
  prefs.putShort("off",   cfg.ledOffset);
  prefs.putBool ("rev",   cfg.reversed);
  prefs.putUChar("pin",   cfg.ledPin);
  prefs.putUInt ("cl",    cfg.colorLeft);
  prefs.putUInt ("cr",    cfg.colorRight);
  prefs.putUInt ("co",    cfg.colorOther);
  prefs.putUChar("chl",   cfg.chLeft);
  prefs.putUChar("chr",   cfg.chRight);
  prefs.putUChar("bri",   cfg.brightness);
  prefs.putBool ("men",   cfg.micEnabled);
  prefs.putUChar("mgn",   cfg.micGain);
  prefs.putUChar("mth",   cfg.micThreshold);
  prefs.putUChar("msck",  cfg.micPinSck);
  prefs.putUChar("mws",   cfg.micPinCh);
  prefs.putUChar("msd",   cfg.micPinData);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.end();
}

// ------------
// LED handling
// ------------
bool isValidLedPin(uint8_t pin) {
  for (uint8_t p : LED_PIN_WHITELIST)
    if (p == pin)
      return true;
  return false;
}

uint16_t activeNumLeds() {
  long n = lroundf(cfg.keyCount * cfg.ledsPerKey) + max((int16_t)0, cfg.ledOffset);
  return (uint16_t)constrain(n, 1L, (long)LED_MAX_COUNT);
}

// FastLED needs the pin fixed at compile time, so we instantiate every possible pin and select it at boot
#define LED_PIN_CASE(p) case p: FastLED.addLeds<WS2812B, p, GRB>(leds, LED_MAX_COUNT); break;
void setupLeds() {
  if (!isValidLedPin(cfg.ledPin)) cfg.ledPin = 16;
  switch (cfg.ledPin) {
    LED_PIN_CASE(16) LED_PIN_CASE(17) LED_PIN_CASE(18) LED_PIN_CASE(19)
    LED_PIN_CASE(21) LED_PIN_CASE(22) LED_PIN_CASE(23) LED_PIN_CASE(25)
    LED_PIN_CASE(26) LED_PIN_CASE(27) LED_PIN_CASE(32) LED_PIN_CASE(33)
  }
  FastLED.setBrightness(cfg.brightness);
  // Power cap
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 2000);
}

CRGB colorForChannel(uint8_t ch) {
  if (ch == cfg.chLeft)  return CRGB(cfg.colorLeft);
  if (ch == cfg.chRight) return CRGB(cfg.colorRight);
  return CRGB(cfg.colorOther);
}

bool isBlackKey(uint8_t note) {
  switch (note % 12) {
    case 1:
    case 3:
    case 6:
    case 8:
    case 10:
        return true;
    default:
        return false;
  }
}

// Syncs the strip state with the note state
void renderLeds() {
  const uint16_t n = activeNumLeds();
  fill_solid(leds, LED_MAX_COUNT, CRGB::Black);

  const int lastNote = min(127, cfg.firstNote + cfg.keyCount - 1);
  for (int note = cfg.firstNote; note <= lastNote; note++) {
    const uint8_t ch = noteChan[note];
    if (!ch) continue;

    const int   idx   = note - cfg.firstNote;
    int         start = (int)lroundf(idx * cfg.ledsPerKey);
    int         end   = (int)lroundf((idx + 1) * cfg.ledsPerKey);
    if (end <= start) end = start + 1;  // at least 1 LED per key

    if (isBlackKey(note)) {
      int half = (end - start) / 2;
      if (half < 1) half = 1;
      if (idx < cfg.keyCount / 2)
        start = end - half;
      else
        end = start + half;
    }

    const CRGB c = colorForChannel(ch);
    for (int i = start; i < end; i++) {
      int led = i + cfg.ledOffset;
      if (led < 0 || led >= (int)n) continue;
      if (cfg.reversed) led = n - 1 - led;
      leds[led] = c;
    }
  }
}

void bootAnimation() {
  const int n = min((int)lroundf(cfg.keyCount * cfg.ledsPerKey), LED_MAX_COUNT);
  fill_solid(leds, LED_MAX_COUNT, CRGB::Black);

  for (int h = 0; h < n; h++) {
    fadeToBlackBy(leds, n, 64);
    if (h < n) {
      uint8_t hue = map(h, 0, n, 160, 0);
      leds[h] = CHSV(hue, 255, 255);
    }
    FastLED.show();
    delay(10);
  }

  for (int f = 0; f < 16; f++) {
    fadeToBlackBy(leds, n, 40);
    FastLED.show();
    delay(12);
  }

  fill_solid(leds, LED_MAX_COUNT, CRGB::Black);
  FastLED.show();
}

// -------------
// MIDI handling
// -------------
static const char *NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
String midiNoteName(int n) {
  if (n < 0 || n > 127) return "-";
  return String(NOTE_NAMES[n % 12]) + String(n / 12 - 1);
}

void onNoteOn(byte channel, byte note, byte velocity) {
  Serial.printf("[MIDI] NoteOn  ch=%u note=%u vel=%u\n", channel, note, velocity);
  noteChan[note] = (velocity == 0 ? 0 : channel);
  ledsDirty = true;
}

void onNoteOff(byte channel, byte note, byte velocity) {
  Serial.printf("[MIDI] NoteOff ch=%u note=%u\n", channel, note);
  (void)channel; (void)velocity;
  noteChan[note] = 0;
  ledsDirty = true;
}

void onControlChange(byte channel, byte number, byte value) {
  (void)channel; (void)value;
  // CC 120 (all sound off) / 123 (all notes off)
  if (number == 120 || number == 123) {
    memset((void*)noteChan, 0, sizeof(noteChan));
    ledsDirty = true;
  }
}

void setupBleMidi() {
  BLEMIDI.setHandleConnected([]()    { bleConnected = true;  Serial.printf("[BLE] Device \"%s\" connected\n",   BLE_DEVICE_NAME); });
  BLEMIDI.setHandleDisconnected([]() { bleConnected = false; Serial.printf("[BLE] Device \"%s\" disconnected\n", BLE_DEVICE_NAME); });
  MIDI.setHandleNoteOn(onNoteOn);
  MIDI.setHandleNoteOff(onNoteOff);
  MIDI.setHandleControlChange(onControlChange);
  MIDI.begin(MIDI_CHANNEL_OMNI);
  Serial.printf("[BLE] Device \"%s\" waiting for pairing\n", BLE_DEVICE_NAME);
}

// -------------------
// Microphone handling
// -------------------
bool isValidMicPin(uint8_t pin) {
  if (isValidLedPin(pin)) return true;
  for (uint8_t p : MIC_PIN_WHITELIST)
    if (p == pin)
      return true;
  return false;
}

bool areValidMicPins() {
  if (!isValidMicPin(cfg.micPinSck) || !isValidMicPin(cfg.micPinCh) || !isValidMicPin(cfg.micPinData))
    return false;
  if (cfg.micPinSck == cfg.micPinCh || cfg.micPinSck == cfg.micPinData || cfg.micPinCh == cfg.micPinData)
    return false;
  if (cfg.micPinSck == cfg.ledPin || cfg.micPinCh == cfg.ledPin || cfg.micPinData == cfg.ledPin)
    return false;
  return true;
}

void setupMic() {
  if (!cfg.micEnabled) return;
  if (!areValidMicPins()) {
    Serial.println("[MIC] Invalid GPIO configuration (conflict or forbidden pin): mic disabled");
    return;
  }
  if (micBegin(cfg.micPinSck, cfg.micPinCh, cfg.micPinData, noteChan)) {
    micSetGain(cfg.micGain);
    micSetThreshold(cfg.micThreshold);
    Serial.printf("[MIC] Listening (SCK=%u WS=%u SD=%u, gain=%u, threshold=%u)\n", cfg.micPinSck, cfg.micPinCh, cfg.micPinData, cfg.micGain, cfg.micThreshold);
  } else {
    Serial.println("[MIC] Initialization failed");
  }
}

void pumpMicEvents() {
  if (!micRunning() || !micEvents()) return;
  MicEvent ev;
  while (xQueueReceive(micEvents(), &ev, 0) == pdTRUE) {
    if (!bleConnected) continue;
    if (ev.on) {
      Serial.printf("[MIC] Detected note %u (vel=%u) -> NoteOn\n", ev.note, ev.velocity);
      MIDI.sendNoteOn(ev.note, ev.velocity, 1);
    } else {
      MIDI.sendNoteOff(ev.note, 0, 1);
    }
  }
}

// -------------
// WiFi handling
// -------------
void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID);       // open AP (for configuration)
  apMode = true;
  Serial.printf("[WiFi] AP mode: SSID \"%s\", IP %s\n", WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
}

void setupWifi() {
  WiFi.persistent(false);          // credentials are already managed
  WiFi.setAutoReconnect(false);    // reconnection handled in the main loop

  if (strlen(cfg.ssid) == 0) {     // first boot: nothing saved
    startAccessPoint();
  }
  else {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MDNS_HOSTNAME);
    WiFi.begin(cfg.ssid, cfg.pass);
    Serial.printf("[WiFi] Connecting to \"%s\"...\n", cfg.ssid);
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) {
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      apMode = false;
      Serial.printf("[WiFi] Connected: IP %s\n", WiFi.localIP().toString().c_str());
    }
    else {
      Serial.println("[WiFi] STA failed -> falling back to access point");
      startAccessPoint();
    }
  }

  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mDNS] http://" MDNS_HOSTNAME ".local");
  }
}

void maintainWifi() {
  // Reconnect only in STA mode (never in an aggressive loop, so as not to disturb BLE)
  if (apMode || WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiRetry < WIFI_RETRY_INTERVAL_MS) return;
  lastWifiRetry = millis();
  Serial.println("[WiFi] Attempting to reconnect...");
  WiFi.reconnect();
}

// --------------------
// JSON / color helpers
// --------------------
String colorToHex(uint32_t c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%06X", (unsigned)(c & 0xFFFFFF));
  return String(buf);
}

uint32_t hexToColor(const char *s) {
  if (s && *s == '#') s++;
  return s ? (uint32_t)strtoul(s, nullptr, 16) : 0;
}

// ---------------------
// Update handling (OTA)
// ---------------------
void handleOtaUpload(AsyncWebServerRequest *req, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    Serial.printf("[OTA] Start: %s\n", filename.c_str());
    otaInProgress = true;
    fill_solid(leds, LED_MAX_COUNT, CRGB::Black);
    FastLED.show();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Serial.print("[OTA] ");
      Update.printError(Serial);
    }
  }

  if (Update.isRunning() && Update.write(data, len) != len) {
    Serial.print("[OTA] ");
    Update.printError(Serial);
  }

  if (final) {
    if (Update.end(true)) {
      Serial.printf("[OTA] Done: %u bytes\n", (unsigned)(index + len));
    } else {
      Serial.print("[OTA] ");
      Update.printError(Serial);
    }
  }
}

void handleOtaResult(AsyncWebServerRequest *req) {
  const bool ok = !Update.hasError();
  AsyncWebServerResponse *res = req->beginResponse(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  res->addHeader("Connection", "close");
  req->send(res);
  if (ok) rebootAt = millis() + 1500; // give the HTTP response time to be sent
  otaInProgress = false;
}

// -------------------
// Web server handling
// -------------------
void sendConfigJson(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["keyCount"]    = cfg.keyCount;
  doc["firstNote"]   = cfg.firstNote;
  doc["ledsPerKey"]  = serialized(String(cfg.ledsPerKey, 3));
  doc["ledOffset"]   = cfg.ledOffset;
  doc["reversed"]    = cfg.reversed;
  doc["ledPin"]      = cfg.ledPin;
  doc["colorLeft"]   = colorToHex(cfg.colorLeft);
  doc["colorRight"]  = colorToHex(cfg.colorRight);
  doc["colorOther"]  = colorToHex(cfg.colorOther);
  doc["chLeft"]      = cfg.chLeft;
  doc["chRight"]     = cfg.chRight;
  doc["brightness"]  = cfg.brightness;
  doc["micEnabled"]  = cfg.micEnabled;
  doc["micGain"]     = cfg.micGain;
  doc["micThreshold"]= cfg.micThreshold;
  doc["micSck"]      = cfg.micPinSck;
  doc["micWs"]       = cfg.micPinCh;
  doc["micSd"]       = cfg.micPinData;
  doc["ssid"]        = cfg.ssid;  // the password is never returned

  JsonObject st  = doc["status"].to<JsonObject>();
  st["mode"]     = apMode ? "ap" : "sta";
  st["ip"]       = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  st["ble"]      = bleConnected;
  st["bleName"]  = BLE_DEVICE_NAME;
  st["numLeds"]  = activeNumLeds();
  st["mic"]      = micRunning();
  st["heap"]     = ESP.getFreeHeap();
  st["version"]  = FW_VERSION;

  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

void handleConfigPost(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonObject o = json.as<JsonObject>();
  const uint8_t oldLed    = cfg.ledPin;
  const bool    oldMicEn  = cfg.micEnabled;
  const uint8_t oldMicSck = cfg.micPinSck;
  const uint8_t oldMicWs  = cfg.micPinCh;
  const uint8_t oldMicSd  = cfg.micPinData;

  if (o["colorLeft"].is<const char*>())   cfg.colorLeft   = hexToColor(o["colorLeft"]);
  if (o["colorRight"].is<const char*>())  cfg.colorRight  = hexToColor(o["colorRight"]);
  if (o["colorOther"].is<const char*>())  cfg.colorOther  = hexToColor(o["colorOther"]);
  if (o["keyCount"].is<int>())            cfg.keyCount    = constrain((int)o["keyCount"], 1, 108);
  if (o["firstNote"].is<int>())           cfg.firstNote   = constrain((int)o["firstNote"], 0, 120);
  if (o["ledsPerKey"].is<float>())        cfg.ledsPerKey  = constrain((float)o["ledsPerKey"], 0.1f, 10.0f);
  if (o["ledOffset"].is<int>())           cfg.ledOffset   = constrain((int)o["ledOffset"], -50, 100);
  if (o["chLeft"].is<int>())              cfg.chLeft      = constrain((int)o["chLeft"], 1, 16);
  if (o["chRight"].is<int>())             cfg.chRight     = constrain((int)o["chRight"], 1, 16);
  if (o["brightness"].is<int>())          cfg.brightness  = constrain((int)o["brightness"], 5, 255);
  if (o["micGain"].is<int>())             cfg.micGain     = constrain((int)o["micGain"], 1, 64);
  if (o["micThreshold"].is<int>())        cfg.micThreshold= constrain((int)o["micThreshold"], 1, 100);
  if (o["reversed"].is<bool>())           cfg.reversed    = o["reversed"];
  if (o["micEnabled"].is<bool>())         cfg.micEnabled  = o["micEnabled"];
  if (o["ledPin"].is<int>() && isValidLedPin(o["ledPin"])) cfg.ledPin     = o["ledPin"];
  if (o["micSck"].is<int>() && isValidMicPin(o["micSck"])) cfg.micPinSck  = o["micSck"];
  if (o["micWs"].is<int>()  && isValidMicPin(o["micWs"]))  cfg.micPinCh   = o["micWs"];
  if (o["micSd"].is<int>()  && isValidMicPin(o["micSd"]))  cfg.micPinData = o["micSd"];

  saveConfig();
  ledsDirty = true;
  FastLED.setBrightness(cfg.brightness);
  if (micRunning()) {
    micSetGain(cfg.micGain);
    micSetThreshold(cfg.micThreshold);
    Serial.printf("[MIC] New parameters applied (gain=%u, threshold=%u)\n", cfg.micGain, cfg.micThreshold);
  }

  JsonDocument res;
  res["ok"]          = true;
  res["needsReboot"] = (cfg.ledPin != oldLed) || (cfg.micEnabled != oldMicEn) || (cfg.micPinSck != oldMicSck) || (cfg.micPinCh != oldMicWs) || (cfg.micPinData != oldMicSd);
  res["micPinsOk"]   = areValidMicPins();
  res["numLeds"]     = activeNumLeds();
  String out;
  serializeJson(res, out);
  req->send(200, "application/json", out);
}

void handleTestPost(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonObject o = json.as<JsonObject>();
  const int note = o["note"] | -1;
  const bool on  = o["on"]   | false;
  const int ch   = constrain((int)(o["ch"] | 1), 1, 16);
  if (note >= 0 && note < 128) {
    noteChan[note] = on ? ch : 0;
    ledsDirty = true;
  }
  req->send(200, "application/json", "{\"ok\":true}");
}

void handleWifiPost(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonObject o = json.as<JsonObject>();
  if (o["ssid"].is<const char*>()) {
    strlcpy(cfg.ssid, o["ssid"], sizeof(cfg.ssid));
    strlcpy(cfg.pass, o["pass"] | "", sizeof(cfg.pass));
    saveConfig();
    rebootAt = millis() + 1500; // give the HTTP response time to be sent
    req->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
  } else {
    req->send(400, "application/json", "{\"ok\":false}");
  }
}

void sendMicStatusJson(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["running"] = micRunning();
  if (micRunning()) {
    MicStatus s;
    micGetStatus(s);
    doc["level"]    = serialized(String(s.levelDb, 1));
    doc["noise"]    = serialized(String(s.noiseDb, 1));
    doc["note"]     = s.lastNote;
    doc["name"]     = midiNoteName(s.lastNote);
    doc["freq"]     = serialized(String(s.lastFreq, 2));
    doc["cents"]    = serialized(String(s.lastCents, 1));
    doc["age"]      = s.lastMs ? (uint32_t)(millis() - s.lastMs) : 0;
    doc["calCount"] = micCalCount();

    JsonObject cal  = doc["cal"].to<JsonObject>();
    cal["armed"]    = s.calArmed;
    cal["note"]     = s.calNote;
    cal["done"]     = s.calDone;
    cal["freq"]     = serialized(String(s.calFreq, 2));
    cal["cents"]    = serialized(String(s.calCents, 1));
  }
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

void handleMicCalPost(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonObject o = json.as<JsonObject>();
  if (!micRunning()) {
    req->send(409, "application/json", "{\"ok\":false,\"error\":\"mic not running\"}");
    return;
  }
  if (o["clear"].is<bool>() && (bool)o["clear"]) {
    // {"clear": true} erases the table
    micClearCal();
    Serial.println("[MIC] Calibration table cleared");
  }
  else if (o["note"].is<int>()) {
    // {"note": n} arms a capture on note n
    // {"note": -1} disarms
    micArmCal((int)o["note"]);
  }
  req->send(200, "application/json", "{\"ok\":true}");
}

void sendMicCalJson(AsyncWebServerRequest *req) {
  JsonDocument doc;
  JsonArray arr = doc["cal"].to<JsonArray>();
  for (int n = MIC_NOTE_MIN; n <= MIC_NOTE_MAX; n++) {
    const float f = micCalFreq(n);
    if (f <= 0.0f) continue;
    JsonObject e = arr.add<JsonObject>();
    e["note"]  = n;
    e["name"]  = midiNoteName(n);
    e["freq"]  = serialized(String(f, 2));
    e["cents"] = serialized(String(1200.0f * log2f(f / (440.0f * powf(2.0f, (n - 69) / 12.0f))), 1));
  }
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/config",     HTTP_GET, sendConfigJson);
  server.on("/api/mic/status", HTTP_GET, sendMicStatusJson);
  server.on("/api/mic/cal",    HTTP_GET, sendMicCalJson);
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/config",  handleConfigPost));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/mic/cal", handleMicCalPost));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/test",    handleTestPost));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/wifi",    handleWifiPost));

  server.on("/api/alloff", HTTP_POST, [](AsyncWebServerRequest *req) {
    memset((void*)noteChan, 0, sizeof(noteChan));
    ledsDirty = true;
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
    rebootAt = millis() + 500;
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/update", HTTP_POST, handleOtaResult, handleOtaUpload);

  server.onNotFound([](AsyncWebServerRequest *req) {
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("[HTTP] Server started on port 80");
}

// ----------------
// Peripheral setup
// ----------------
void setup() {
  Serial.begin(115200);
  Serial.println("\nPianoLights " FW_VERSION);

  loadConfig();
  setupLeds();        // WS2812B strip
  setupMic();         // I2S microphone
  bootAnimation();

  setupWifi();        // STA (blocking, 15 s max) then possible AP fallback
  setupWebServer();
  setupBleMidi();     // BLE advertising
}

// ---------
// Main loop
// ---------
void loop() {
  // MIDI messages
  MIDI.read();

  // Microphone listening
  pumpMicEvents();

  // WiFi messages
  maintainWifi();

  // LED rendering
  if (!otaInProgress && ledsDirty && millis() - lastShow >= LED_FRAME_INTERVAL_MS) {
    ledsDirty = false;
    renderLeds();
    FastLED.show();
    lastShow = millis();
  }

  // Pending reboot
  if (rebootAt && millis() > rebootAt) {
    Serial.println("[SYS] Rebooting...");
    delay(100);
    ESP.restart();
  }
}

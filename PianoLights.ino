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
#include <esp_wifi.h>
#include <esp_coexist.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <FastLED.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <Update.h>

#include <BLEMIDI_Transport.h>
#include <hardware/BLEMIDI_ESP32_NimBLE.h>

#include "PianoLightsGzip.h"
#include "PianoLightsMic.h"

// ---------
// Constants
// ---------
#define FW_VERSION              "1.11"
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_RETRY_INTERVAL_MS  30000
#define WIFI_AP_CHANNEL         6                   // AP-mode channel
#define WIFI_AP_MAX_CLIENTS     4
#define WIFI_AP_SSID            "Piano-Lights-AP"   // AP-mode SSID
#define MDNS_HOSTNAME           "pianolights"       // access via http://pianolights.local
#define BLE_DEVICE_NAME         "Piano-Lights"      // default name shown under Windows
#define BLE_ADV_INTERVAL_MIN    160                 // advertising interval, units of 0.625 ms (160 = 100 ms)
#define BLE_ADV_INTERVAL_MAX    320                 // (320 = 200 ms): leaves the radio to WiFi (coexistence)
#define BLE_CONN_ITVL_MIN       6                   // requested connection interval, units of 1.25 ms (6 = 7.5 ms)
#define BLE_CONN_ITVL_MAX       12                  // (12 = 15 ms), per the BLE-MIDI recommendation
#define BLE_TUNE_DELAY_MS       3000                // delay after connection before reading/tuning the link
#define MIC_TX_SPACING_MS       3                   // minimum spacing between two mic-originated MIDI messages
#define LED_MAX_COUNT           300                 // max allocated buffer
#define LED_FRAME_INTERVAL_MS   15                  // ~60 fps max

// Allowed pins for the LED strip (strapping, flash and input-only pins are excluded), the microphone and the power relay
static const uint8_t LED_PIN_WHITELIST[] = {16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
static const uint8_t MIC_PIN_WHITELIST[] = {5, 34, 35, 36, 39};
static const int8_t RELAY_PIN_WHITELIST[] = {-1, 12, 13, 14};

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
  uint8_t  ledPin      = 16;       // data GPIO
  int8_t   relayPin    = -1;       // power relay GPIO
  // Colors
  uint32_t colorLeft   = 0x0000FF; // left hand
  uint32_t colorRight  = 0x00FF00; // right hand
  uint32_t colorOther  = 0xFFFFFF; // any other channel
  uint8_t  chLeft      = 2;        // left-hand MIDI channel (1-16)
  uint8_t  chRight     = 1;        // right-hand MIDI channel (1-16)
  uint8_t  brightness  = 100;      // global brightness (5-255)
  // Visual effects
  bool     fxLeft      = false;    // visual effects for left hand
  bool     fxRight     = false;    // visual effects for right hand
  bool     fxOther     = false;    // visual effects for any other channel
  // Microphone
  bool     micEnabled  = false;
  uint8_t  micGain     = 20;       // digital gain (1-64)
  uint8_t  micThreshold= 50;       // detection sensitivity (1-100)
  uint8_t  micPinSck   = 21;       // I2S bit clock (SCK/BCLK)
  uint8_t  micPinCh    = 5;        // I2S word select (WS/LRCLK)
  uint8_t  micPinData  = 26;       // I2S data (SD/DOUT)
  // WiFi
  char     ssid[33]    = "";
  char     pass[65]    = "";
  // Bluetooth
  char     bleName[24] = BLE_DEVICE_NAME;
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
bool              fxAnimating   = false;  // true while at least one FX note is lit
volatile bool     bleConnected  = false;
volatile bool     powerOn       = true;   // true even if no power relay is configured
volatile bool     otaInProgress = false;  // freezes LED rendering while writing to flash
bool              apMode        = false;
volatile uint32_t rebootAt      = 0;      // != 0 -> scheduled restart
volatile uint32_t cometAt       = 0;      // != 0 -> scheduled animation
uint32_t          lastWifiRetry = 0;
uint32_t          lastShow      = 0;
uint32_t          lastMicTx     = 0;      // last mic-originated MIDI message
uint32_t          micDropSeen   = 0;      // last reported mic queue drop count
volatile uint32_t bleTuneAt     = 0;      // != 0 -> scheduled BLE link check
uint8_t           bleTunePhase  = 0;      // 0 = measure and request, 1 = report result

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
  cfg.relayPin    = prefs.getChar("rpin",  cfg.relayPin);
  cfg.colorLeft   = prefs.getUInt("cl",    cfg.colorLeft);
  cfg.colorRight  = prefs.getUInt("cr",    cfg.colorRight);
  cfg.colorOther  = prefs.getUInt("co",    cfg.colorOther);
  cfg.chLeft      = prefs.getUChar("chl",  cfg.chLeft);
  cfg.chRight     = prefs.getUChar("chr",  cfg.chRight);
  cfg.brightness  = prefs.getUChar("bri",  cfg.brightness);
  cfg.fxLeft      = prefs.getBool("fxl",   cfg.fxLeft);
  cfg.fxRight     = prefs.getBool("fxr",   cfg.fxRight);
  cfg.fxOther     = prefs.getBool("fxo",   cfg.fxOther);
  cfg.micEnabled  = prefs.getBool ("men",  cfg.micEnabled);
  cfg.micGain     = prefs.getUChar("mgn",  cfg.micGain);
  cfg.micThreshold= prefs.getUChar("mth",  cfg.micThreshold);
  cfg.micPinSck   = prefs.getUChar("msck", cfg.micPinSck);
  cfg.micPinCh    = prefs.getUChar("mws",  cfg.micPinCh);
  cfg.micPinData  = prefs.getUChar("msd",  cfg.micPinData);
  prefs.getString("ssid", cfg.ssid, sizeof(cfg.ssid));
  prefs.getString("pass", cfg.pass, sizeof(cfg.pass));
  prefs.getString("bnam", cfg.bleName, sizeof(cfg.bleName));
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
  prefs.putChar ("rpin",  cfg.relayPin);
  prefs.putUInt ("cl",    cfg.colorLeft);
  prefs.putUInt ("cr",    cfg.colorRight);
  prefs.putUInt ("co",    cfg.colorOther);
  prefs.putUChar("chl",   cfg.chLeft);
  prefs.putUChar("chr",   cfg.chRight);
  prefs.putUChar("bri",   cfg.brightness);
  prefs.putBool ("fxl",   cfg.fxLeft);
  prefs.putBool ("fxr",   cfg.fxRight);
  prefs.putBool ("fxo",   cfg.fxOther);
  prefs.putBool ("men",   cfg.micEnabled);
  prefs.putUChar("mgn",   cfg.micGain);
  prefs.putUChar("mth",   cfg.micThreshold);
  prefs.putUChar("msck",  cfg.micPinSck);
  prefs.putUChar("mws",   cfg.micPinCh);
  prefs.putUChar("msd",   cfg.micPinData);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("bnam", cfg.bleName);
  prefs.end();
}

// ------------
// LED handling
// ------------
bool isValidRelayPin(int8_t pin) {
  for (int8_t p : RELAY_PIN_WHITELIST)
    if (p == pin)
      return true;
  return false;
}

void setupPower() {
  if (!isValidRelayPin(cfg.relayPin)) cfg.relayPin = -1;
  if (cfg.relayPin < 0) return;
  digitalWrite(cfg.relayPin, LOW);
  pinMode(cfg.relayPin, OUTPUT);
  digitalWrite(cfg.relayPin, HIGH);
}

void togglePower(bool on) {
  if (cfg.relayPin < 0 || powerOn == on) return;
  if (on) {
    digitalWrite(cfg.relayPin, HIGH);
    powerOn = true;
  } else {
    digitalWrite(cfg.relayPin, LOW);
    powerOn = false;
  }
}

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

bool fxForChannel(uint8_t ch) {
  if (ch == cfg.chLeft)  return cfg.fxLeft;
  if (ch == cfg.chRight) return cfg.fxRight;
  return cfg.fxOther;
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

// Shimmering glow at the note position and symmetric gradient spilling over the adjacent keys
void plotFxNote(uint8_t note, int start, int end, const CRGB &c, uint16_t n, uint32_t t) {
  const CHSV  base   = rgb2hsv_approximate(c);
  const float center = (start + end - 1) * 0.5f;
  const float halo   = max(1.5f, cfg.ledsPerKey * 1.5f);  // reaches the adjacent keys

  const int lo = (int)floorf(center - halo);
  const int hi = (int)ceilf(center + halo);
  for (int i = lo; i <= hi; i++) {
    int led = i + cfg.ledOffset;
    if (led < 0 || led >= (int)n) continue;

    float d = fabsf((float)i - center) / (halo + 0.5f);
    if (d >= 1.0f) continue;
    const uint8_t fall = (uint8_t)(255.0f * (1.0f - d) * (1.0f - d));

    const uint8_t sh  = sin8((uint8_t)(t / 3) + (uint8_t)(i * 37) + (uint8_t)(note * 11));
    const uint8_t val = scale8(fall, 140 + scale8(sh, 115));

    const int8_t hueShift = (int8_t)(((int)sin8((uint8_t)(t / 9) + (uint8_t)(i * 23)) - 128) / 8);

    uint8_t sat = base.s;
    if (d < 0.25f) sat = scale8(sat, 200);

    CHSV hsv((uint8_t)(base.h + hueShift), sat, val);
    if (cfg.reversed) led = n - 1 - led;
    leds[led] += CRGB(hsv);  // saturating add blends overlapping halos
  }
}

// Syncs the strip state with the note state.
// Returns true if at least one animated (FX) note is lit, so the caller keeps rendering.
bool renderLeds() {
  const uint16_t n = activeNumLeds();
  fill_solid(leds, LED_MAX_COUNT, CRGB::Black);

  bool           animating = false;
  const uint32_t t         = millis();
  const int      lastNote  = min(127, cfg.firstNote + cfg.keyCount - 1);

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

    if (fxForChannel(ch)) {
      animating = true;
      plotFxNote(note, start, end, c, n, t);
      continue;
    }

    for (int i = start; i < end; i++) {
      int led = i + cfg.ledOffset;
      if (led < 0 || led >= (int)n) continue;
      if (cfg.reversed) led = n - 1 - led;
      leds[led] = c;
    }
  }
  return animating;
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
  BLEMIDI.setName(cfg.bleName);
  BLEMIDI.setHandleConnected([]() {
    bleConnected = true;
    bleTuneAt    = millis() + BLE_TUNE_DELAY_MS;   // let the central settle its parameters first
    bleTunePhase = 0;
    Serial.printf("[BLE] Device \"%s\" connected\n", cfg.bleName);
  });
  BLEMIDI.setHandleDisconnected([]() {
    bleConnected = false;
    bleTuneAt    = 0;
    Serial.printf("[BLE] Device \"%s\" disconnected\n", cfg.bleName);
  });
  MIDI.setHandleNoteOn(onNoteOn);
  MIDI.setHandleNoteOff(onNoteOff);
  MIDI.setHandleControlChange(onControlChange);
  MIDI.begin(MIDI_CHANNEL_OMNI);

  // Slow down the advertising started by BLE-MIDI: NimBLE defaults to short intervals that
  // starve the WiFi AP under core 3.x coexistence (HTTP stalls while BLE advertises)
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  if (adv) {
    adv->stop();
    adv->setMinInterval(BLE_ADV_INTERVAL_MIN);
    adv->setMaxInterval(BLE_ADV_INTERVAL_MAX);
    adv->start();
  }
  Serial.printf("[BLE] Device \"%s\" waiting for pairing\n", cfg.bleName);
}

// Reads the negotiated connection interval a few seconds after connection and, if the central
// settled on a slow one (Windows often picks 30-60 ms), asks for the 7.5-15 ms recommended by the
// BLE-MIDI specification. A slow interval caps the number of MIDI messages per second the link
// can carry; beyond that, NimBLE's buffers fill up and notifications get lost
void tuneBleLink() {
  if (!bleTuneAt || !bleConnected || (int32_t)(millis() - bleTuneAt) < 0) return;
  bleTuneAt = 0;

  NimBLEServer *srv = NimBLEDevice::getServer();
  if (!srv || srv->getConnectedCount() == 0) return;
  NimBLEConnInfo info = srv->getPeerInfo(0);
  const uint16_t itvl = info.getConnInterval();   // units of 1.25 ms
  Serial.printf("[BLE] Connection interval %.2f ms, latency %u, timeout %u ms%s\n",
                itvl * 1.25f, info.getConnLatency(), info.getConnTimeout() * 10,
                bleTunePhase ? " (after request)" : "");

  if (bleTunePhase == 0 && itvl > BLE_CONN_ITVL_MAX) {
    Serial.println("[BLE] Requesting a 7.5-15 ms connection interval");
    srv->updateConnParams(info.getConnHandle(), BLE_CONN_ITVL_MIN, BLE_CONN_ITVL_MAX, 0, 400);
    bleTunePhase = 1;
    bleTuneAt    = millis() + BLE_TUNE_DELAY_MS;   // report what the central actually granted
  }
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
  if (!cfg.micEnabled) return;
  if (!micRunning() || !micEvents()) return;
  MicEvent ev;

  // Nobody listening: discard everything, nothing to pace
  if (!bleConnected) {
    while (xQueueReceive(micEvents(), &ev, 0) == pdTRUE) {}
    return;
  }

  // Report events lost upstream (mic queue full), should never happen in normal use
  const uint32_t dropped = micDroppedEvents();
  if (dropped != micDropSeen) {
    Serial.printf("[MIC] WARNING: %u event%s dropped (queue full)\n", (unsigned)(dropped - micDropSeen), dropped - micDropSeen > 1 ? "s" : "");
    micDropSeen = dropped;
  }

  // One MIDI message per pass, spaced out: each message is one BLE notification, and a burst
  // of them in the same millisecond is what exhausts NimBLE's buffers and loses NoteOffs
  if (millis() - lastMicTx < MIC_TX_SPACING_MS) return;
  if (xQueueReceive(micEvents(), &ev, 0) != pdTRUE) return;

  if (ev.on) {
    Serial.printf("[MIC] Note %u (vel=%u) -> NoteOn\n", ev.note, ev.velocity);
    MIDI.sendNoteOn(ev.note, ev.velocity, 1);
  } else {
    Serial.printf("[MIC] Note %u -> NoteOff\n", ev.note);
    MIDI.sendNoteOff(ev.note, 0, 1);
  }
  lastMicTx = millis();
}

// -------------
// WiFi handling
// -------------
// Diagnostic trace of the WiFi driver events (STA disconnect reasons, AP client association, DHCP)
void onWifiEvent(WiFiEvent_t ev, WiFiEventInfo_t info) {
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WiFi] STA associated");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      // 201 = AP not found, 202 = auth fail, 15 = 4-way handshake timeout (bad password)
      Serial.printf("[WiFi] STA disconnected, reason=%u\n", info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.println("[WiFi] AP started");
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.println("[WiFi] AP: client associated");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.printf("[WiFi] AP: client left, reason=%u\n", info.wifi_ap_stadisconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      Serial.println("[WiFi] AP: DHCP lease assigned");
      break;
    default:
      break;
  }
}

void startAccessPoint() {
  // Fully tear down any pending STA attempt before switching modes
  // On core 3.x a half-connecting STA interface left behind makes the AP visible (beacons) but unable to accept associations
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(WIFI_AP_SSID, nullptr, WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CLIENTS)) {   // open AP (for configuration)
    Serial.println("[WiFi] softAP() failed");
  }
  apMode = true;
  Serial.printf("[WiFi] AP mode: SSID \"%s\", channel %u, IP %s\n", WIFI_AP_SSID, WIFI_AP_CHANNEL, WiFi.softAPIP().toString().c_str());
}

void setupWifi() {
  WiFi.onEvent(onWifiEvent);
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
  doc["relayPin"]    = cfg.relayPin;    // -1 = no relay
  doc["colorLeft"]   = colorToHex(cfg.colorLeft);
  doc["colorRight"]  = colorToHex(cfg.colorRight);
  doc["colorOther"]  = colorToHex(cfg.colorOther);
  doc["fxLeft"]      = cfg.fxLeft;
  doc["fxRight"]     = cfg.fxRight;
  doc["fxOther"]     = cfg.fxOther;
  doc["chLeft"]      = cfg.chLeft;
  doc["chRight"]     = cfg.chRight;
  doc["brightness"]  = cfg.brightness;
  doc["micEnabled"]  = cfg.micEnabled;
  doc["micGain"]     = cfg.micGain;
  doc["micThreshold"]= cfg.micThreshold;
  doc["micSck"]      = cfg.micPinSck;
  doc["micWs"]       = cfg.micPinCh;
  doc["micSd"]       = cfg.micPinData;
  doc["ssid"]        = cfg.ssid;        // ssid only (the password is never returned)
  doc["bleName"]     = cfg.bleName;

  JsonObject st  = doc["status"].to<JsonObject>();
  st["mode"]     = apMode ? "ap" : "sta";
  st["ip"]       = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  st["ble"]      = bleConnected;
  st["bleName"]  = cfg.bleName;
  st["numLeds"]  = activeNumLeds();
  st["power"]    = (cfg.relayPin < 0) ? -1 : (powerOn ? 1 : 0);
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
  const int8_t  oldRelay  = cfg.relayPin;
  const bool    oldMicEn  = cfg.micEnabled;
  const uint8_t oldMicSck = cfg.micPinSck;
  const uint8_t oldMicWs  = cfg.micPinCh;
  const uint8_t oldMicSd  = cfg.micPinData;
  char oldName[24]; strlcpy(oldName, cfg.bleName, sizeof(oldName));

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
  if (o["fxLeft"].is<bool>())             cfg.fxLeft      = o["fxLeft"];
  if (o["fxRight"].is<bool>())            cfg.fxRight     = o["fxRight"];
  if (o["fxOther"].is<bool>())            cfg.fxOther     = o["fxOther"];
  if (o["micEnabled"].is<bool>())         cfg.micEnabled  = o["micEnabled"];
  if (o["relayPin"].is<int>() && isValidRelayPin(o["relayPin"])) cfg.relayPin = o["relayPin"];
  if (o["ledPin"].is<int>() && isValidLedPin(o["ledPin"])) cfg.ledPin     = o["ledPin"];
  if (o["micSck"].is<int>() && isValidMicPin(o["micSck"])) cfg.micPinSck  = o["micSck"];
  if (o["micWs"].is<int>()  && isValidMicPin(o["micWs"]))  cfg.micPinCh   = o["micWs"];
  if (o["micSd"].is<int>()  && isValidMicPin(o["micSd"]))  cfg.micPinData = o["micSd"];
  if (o["bleName"].is<const char*>()) { const char *s = o["bleName"]; if (*s) strlcpy(cfg.bleName, s, sizeof(cfg.bleName)); }

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
  res["needsReboot"] = (cfg.ledPin != oldLed) || (cfg.relayPin != oldRelay) || (cfg.micEnabled != oldMicEn) || (cfg.micPinSck != oldMicSck) || (cfg.micPinCh != oldMicWs) || (cfg.micPinData != oldMicSd) || (strcmp(cfg.bleName, oldName) != 0);
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
    doc["dropped"]  = micDroppedEvents();

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
  // The page is served gzipped (~8 KB instead of ~30 KB): with BLE + WiFi + async server the free heap is tight
  // A long uncompressed transfer starves the TCP buffers and stalls mid-way
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    Serial.printf("[HTTP] GET / from %s (heap=%u)\n", req->client()->remoteIP().toString().c_str(), ESP.getFreeHeap());
    AsyncWebServerResponse *res = req->beginResponse(200, "text/html", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
    res->addHeader("Content-Encoding", "gzip");
    req->send(res);
  });

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *req) {
    Serial.printf("[HTTP] GET /api/config from %s (heap=%u)\n", req->client()->remoteIP().toString().c_str(), ESP.getFreeHeap());
    sendConfigJson(req);
  });
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

  server.on("/api/power", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (cfg.relayPin < 0) {
      req->send(409, "application/json", "{\"ok\":false,\"err\":\"no power relay configured\"}");
      return;
    }
    togglePower(!powerOn);
    cometAt = powerOn ? millis() + 100 : 0;
    req->send(200, "application/json", powerOn ? "{\"ok\":true,\"power\":1}" : "{\"ok\":true,\"power\":0}");
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
  setupPower();
  setupLeds();        // WS2812B LED strip
  setupMic();         // I2S microphone
  bootAnimation();    // comet-like streak animation

  // WiFi/BLE coexistence: favor WiFi (BLE-MIDI needs very little airtime)
  // Must be set before either radio starts
  esp_coex_preference_set(ESP_COEX_PREFER_WIFI);

  setupBleMidi();     // BLE advertising, started first so that coexistence is initialized by the BT side
  Serial.printf("[SYS] Free heap after BLE init: %u\n", ESP.getFreeHeap());
  setupWifi();        // STA (blocking, 15 s max) then possible AP fallback
  setupWebServer();
  Serial.printf("[SYS] Free heap after setup: %u\n", ESP.getFreeHeap());
}

// ---------
// Main loop
// ---------
void loop() {
  // MIDI messages
  MIDI.read();

  // Microphone listening
  pumpMicEvents();

  // BLE link check after connection
  tuneBleLink();

  // WiFi messages
  maintainWifi();

  // LED rendering
  if (!otaInProgress && !cometAt && powerOn && (ledsDirty || fxAnimating) && millis() - lastShow >= LED_FRAME_INTERVAL_MS) {
    ledsDirty = false;
    fxAnimating = renderLeds();
    FastLED.show();
    lastShow = millis();
  }

  // Pending power animation (comet)
  if (!otaInProgress && cometAt && millis() > cometAt) {
    cometAt = 0;
    bootAnimation();
    ledsDirty = true;
  }

  // Pending reboot
  if (rebootAt && millis() > rebootAt) {
    Serial.println("[SYS] Rebooting...");
    delay(100);
    ESP.restart();
  }
}

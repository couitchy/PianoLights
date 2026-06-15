/*
 * ==================================================================================================
 *  PianoLights.ino, bridge BLE/MIDI entre Synthesia et un ruban LED
 * ==================================================================================================
 *  Cible      : ESP32 WROOM ("ESP32 Dev Module")
 *  Rôle       : Périphérique BLE MIDI (vu comme sortie MIDI par Synthesia sous Windows)
 *  Contrôle   : Les notes reçues pilotent un ruban LED WS2812B pour indiquer les touches à jouer
 *  Config     : Page web embarquée (WiFi STA avec repli AP automatique), préférences stockées en NVS
 *
 *  Bibliothèques requises sous Arduino IDE :
 *    - MIDI Library        (Francois Best)
 *    - BLE-MIDI            (lathoub, mais à patcher)
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

// ----------
// Constantes
// ----------
#define FW_VERSION              "1.1"
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_RETRY_INTERVAL_MS  30000
#define WIFI_AP_SSID            "Piano-Lights-AP"   // SSID du mode AP
#define MDNS_HOSTNAME           "pianolights"       // accès via http://pianolights.local
#define BLE_DEVICE_NAME         "Piano-Lights"      // nom visible sous Windows
#define LED_MAX_COUNT           300                 // buffer max alloué
#define LED_FRAME_INTERVAL_MS   15                  // ~60 fps max

// Pins autorisées pour le ruban lumineux (on exclut notamment les pins de strapping, flash et input-only)
static const uint8_t LED_PIN_WHITELIST[] = {16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};

// -----------
// Préférences
// -----------
struct Config {
  // Géométrie
  uint8_t  keyCount    = 88;       // nombre de touches du clavier
  uint8_t  firstNote   = 21;       // note MIDI de la 1ère touche (21 = A0)
  float    ledsPerKey  = 2.0f;     // ratio LEDs/touche (flottant, affinable)
  int16_t  ledOffset   = 0;        // index de la 1ère LED utile
  bool     reversed    = false;    // sens du ruban
  uint8_t  ledPin      = 16;       // GPIO data (redémarrage requis)
  // Couleurs
  uint32_t colorLeft   = 0x00A0FF; // main gauche
  uint32_t colorRight  = 0xFF4000; // main droite
  uint32_t colorOther  = 0x00FF60; // tout autre canal
  uint8_t  chLeft      = 1;        // canal MIDI main gauche (1-16)
  uint8_t  chRight     = 2;        // canal MIDI main droite (1-16)
  uint8_t  brightness  = 100;      // luminosité globale (5-255)
  // WiFi
  char     ssid[33]    = "";
  char     pass[65]    = "";
};

Config       cfg;
Preferences  prefs;

// -----------
// État global
// -----------
BLEMIDI_CREATE_INSTANCE(BLE_DEVICE_NAME, MIDI)

CRGB              leds[LED_MAX_COUNT];
AsyncWebServer    server(80);

volatile uint8_t  noteChan[128] = {0};    // 0 = éteinte, sinon canal MIDI 1-16
volatile bool     ledsDirty     = true;
volatile bool     bleConnected  = false;
volatile bool     otaInProgress = false;  // fige le rendu LED pendant l'écriture dans la flash

bool      apMode        = false;
uint32_t  rebootAt      = 0;              // != 0 -> redémarrage planifié
uint32_t  lastWifiRetry = 0;
uint32_t  lastShow      = 0;

// --------------
// Stockage (NVS)
// --------------
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
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.end();
}

// ------------
// Gestion LEDs
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

// FastLED a besoin d'avoir la pin figée dès la compilation : on instancie donc chaque pin potentielle et on sélectionne au boot
#define LED_PIN_CASE(p) case p: FastLED.addLeds<WS2812B, p, GRB>(leds, LED_MAX_COUNT); break;
void setupLeds() {
  if (!isValidLedPin(cfg.ledPin)) cfg.ledPin = 16;
  switch (cfg.ledPin) {
    LED_PIN_CASE(16) LED_PIN_CASE(17) LED_PIN_CASE(18) LED_PIN_CASE(19)
    LED_PIN_CASE(21) LED_PIN_CASE(22) LED_PIN_CASE(23) LED_PIN_CASE(25)
    LED_PIN_CASE(26) LED_PIN_CASE(27) LED_PIN_CASE(32) LED_PIN_CASE(33)
  }
  FastLED.setBrightness(cfg.brightness);
  // Limite conso
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 2000);
}

CRGB colorForChannel(uint8_t ch) {
  if (ch == cfg.chLeft)  return CRGB(cfg.colorLeft);
  if (ch == cfg.chRight) return CRGB(cfg.colorRight);
  return CRGB(cfg.colorOther);
}

// Synchronise l'état du ruban avec celui des notes
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
    if (end <= start) end = start + 1;  // au moins 1 LED par touche

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

// ------------
// Gestion MIDI
// ------------
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
  BLEMIDI.setHandleConnected([]()    { bleConnected = true;  Serial.printf("[BLE] Périphérique \"%s\" connecté\n",   BLE_DEVICE_NAME); });
  BLEMIDI.setHandleDisconnected([]() { bleConnected = false; Serial.printf("[BLE] Périphérique \"%s\" déconnecté\n", BLE_DEVICE_NAME); });
  MIDI.setHandleNoteOn(onNoteOn);
  MIDI.setHandleNoteOff(onNoteOff);
  MIDI.setHandleControlChange(onControlChange);
  MIDI.begin(MIDI_CHANNEL_OMNI);
  Serial.printf("[BLE] Périphérique \"%s\" en attente d'appairage\n", BLE_DEVICE_NAME);
}

// ------------
// Gestion WiFi
// ------------
void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID);       // AP ouvert (pour la configuration)
  apMode = true;
  Serial.printf("[WiFi] Mode AP — SSID \"%s\", IP %s\n", WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
}

void setupWifi() {
  WiFi.persistent(false);          // les identifiants sont déjà gérés ici
  WiFi.setAutoReconnect(false);    // reconnexion gérée dans la boucle principale

  if (strlen(cfg.ssid) == 0) {     // premier démarrage : rien d'enregistré
    startAccessPoint();
  }
  else {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MDNS_HOSTNAME);
    WiFi.begin(cfg.ssid, cfg.pass);
    Serial.printf("[WiFi] Connexion à \"%s\"...\n", cfg.ssid);
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) {
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      apMode = false;
      Serial.printf("[WiFi] Connecté — IP %s\n", WiFi.localIP().toString().c_str());
    }
    else {
      Serial.println("[WiFi] Échec STA -> repli en point d'accès");
      startAccessPoint();
    }
  }

  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mDNS] http://" MDNS_HOSTNAME ".local");
  }
}

void maintainWifi() {
  // Reconnexion uniquement en mode STA (jamais en boucle agressive pour ne pas perturber le BLE)
  if (apMode || WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiRetry < WIFI_RETRY_INTERVAL_MS) return;
  lastWifiRetry = millis();
  Serial.println("[WiFi] Tentative de reconnexion...");
  WiFi.reconnect();
}

// -----------------------
// Helpers JSON / couleurs
// -----------------------
String colorToHex(uint32_t c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%06X", (unsigned)(c & 0xFFFFFF));
  return String(buf);
}

uint32_t hexToColor(const char *s) {
  if (s && *s == '#') s++;
  return s ? (uint32_t)strtoul(s, nullptr, 16) : 0;
}

// -------------------------
// Gestion mise à jour (OTA)
// -------------------------
void handleOtaUpload(AsyncWebServerRequest *req, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    Serial.printf("[OTA] Début : %s\n", filename.c_str());
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
      Serial.printf("[OTA] Terminée : %u octets\n", (unsigned)(index + len));
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
  if (ok) rebootAt = millis() + 1500; // laisse le temps à la réponse HTTP de partir
  otaInProgress = false;
}

// -------------------
// Gestion serveur web
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
  doc["ssid"]        = cfg.ssid;  // le mot de passe n'est jamais renvoyé

  JsonObject st  = doc["status"].to<JsonObject>();
  st["mode"]     = apMode ? "ap" : "sta";
  st["ip"]       = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  st["ble"]      = bleConnected;
  st["bleName"]  = BLE_DEVICE_NAME;
  st["numLeds"]  = activeNumLeds();
  st["heap"]     = ESP.getFreeHeap();
  st["version"]  = FW_VERSION;

  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

void handleConfigPost(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonObject o = json.as<JsonObject>();
  const uint8_t oldPin = cfg.ledPin;

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
  if (o["reversed"].is<bool>())           cfg.reversed    = o["reversed"];
  if (o["ledPin"].is<int>() && isValidLedPin(o["ledPin"])) cfg.ledPin = o["ledPin"];

  saveConfig();
  FastLED.setBrightness(cfg.brightness);
  ledsDirty = true;

  JsonDocument res;
  res["ok"]          = true;
  res["needsReboot"] = (cfg.ledPin != oldPin);
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
    rebootAt = millis() + 1500; // laisse le temps à la réponse HTTP de partir
    req->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
  } else {
    req->send(400, "application/json", "{\"ok\":false}");
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/config", HTTP_GET, sendConfigJson);
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/config", handleConfigPost));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/test",   handleTestPost));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/wifi",   handleWifiPost));

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
  Serial.println("[HTTP] Serveur démarré sur le port 80");
}

// -----------------------
// Setup des périphériques
// -----------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\nPianoLights " FW_VERSION);

  loadConfig();
  setupLeds();
  bootAnimation();

  setupWifi();         // STA (bloquant 15 s max) puis repli AP éventuel
  setupWebServer();
  setupBleMidi();      // advertising BLE
}

// -----------------
// Boucle principale
// -----------------
void loop() {
  // Messages MIDI
  MIDI.read();

  // Messages WiFi
  maintainWifi();

  // Rendu LEDs
  if (!otaInProgress && ledsDirty && millis() - lastShow >= LED_FRAME_INTERVAL_MS) {
    ledsDirty = false;
    renderLeds();
    FastLED.show();
    lastShow = millis();
  }

  // Cas d'un redémarrage différé
  if (rebootAt && millis() > rebootAt) {
    Serial.println("[SYS] Redémarrage...");
    delay(100);
    ESP.restart();
  }
}

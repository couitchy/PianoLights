/*
 * ==================================================================================================
 *  PianoLightsMic.h, acoustic note detection through an I2S MEMS microphone (INMP441)
 * ==================================================================================================
 *
 *    1. Audio is captured with an I2S MEMS microphone (DMA driven, no CPU cost for acquisition).
 *    2. An onset detector spots key strikes (short-term RMS above a fixed absolute threshold,
 *       post-gain: the gain slider therefore sets the acoustic sensitivity).
 *    3. For each expected note (lit by Synthesia), the analyzer measures the energy concentration
 *       of the window on its first 3 partials (Goertzel). If it exceeds the threshold set by the
 *       threshold slider, the note is considered played. This is exactly the same measure as the
 *       one used by calibration: simple, level independent (it is a ratio) and frequency
 *       independent (works from A0 to C8).
 *    4. Confirmed notes are pushed to a FreeRTOS queue; the main loop forwards them over BLE/MIDI.
 *
 *  There is deliberately NO disambiguation guard (octave, sub-harmonic, etc.): an expected note
 *  whose partials are excited by another genuinely played note will be confirmed. This is an
 *  accepted trade-off: never miss a played note, at the cost of accepting impostors.
 *
 *  The only two detection controls are the exposed sliders (micSetGain / micSetThreshold).
 *
 *  The per-note calibration (real measured fundamental, "stretched" tuning of acoustic pianos)
 *  is persisted in NVS (namespace "plmic").
 *
 *  Public API: micBegin / micRunning / micEvents / micSetGain / micSetThreshold / micGetStatus /
 *  micArmCal / micClearCal / micCalFreq / micCalCount.
 * ==================================================================================================
 */
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <driver/i2s_std.h>
#include <math.h>

// Structural sizing
#define MIC_I2S_PORT        I2S_NUM_0
#define MIC_SAMPLE_RATE     16000     // 16 kHz is enough since piano fundamentals top out around 4.2
#define MIC_HOP             128       // samples per DMA read (8 ms)
#define MIC_RING            4096      // ring buffer length (power of two)
#define MIC_WIN             1600      // analysis window: 100 ms
#define MIC_NOTE_MIN        21        // A0
#define MIC_NOTE_MAX        108       // C8
#define MIC_NOTEOFF_MS      200       // automatic note-off delay after a detection
#define MIC_RETRIG_MS       250       // per-note retrigger lockout (MIDI hygiene)
#define MIC_ONSET_GAP_MS    60        // minimum delay between two onsets
#define MIC_ABS_FLOOR_DB    -30.0f    // onset threshold (post-gain dBFS): the gain sets sensitivity
#define MIC_DC_ALPHA        0.001f    // INMP441 DC offset removal (~2.5 Hz high-pass)
#define MIC_ENV_RELEASE     0.02f     // displayed level envelope
#define MIC_AMB_ALPHA       0.0005f   // slow ambient level tracker
#define MIC_ACTIVE_SLOTS    16        // pending note-off slots

struct MicEvent {
  uint8_t note;
  uint8_t velocity;
  bool    on;
};

struct MicStatus {
  float    levelDb;    // level envelope: instant attack, ~400 ms release (dBFS, post-gain)
  float    noiseDb;    // slow ambient level estimate (dBFS)
  int      lastNote;   // last detected MIDI note, -1 if none yet
  float    lastFreq;   // measured/used fundamental of the last detection (Hz)
  float    lastCents;  // deviation of lastFreq vs A440 equal temperament (cents)
  uint32_t lastMs;     // millis() timestamp of the last detection
  bool     calArmed;   // a calibration capture is pending
  int      calNote;    // target note of the pending/last capture
  bool     calDone;    // one-shot: last capture completed (cleared on next arm)
  float    calFreq;    // measured fundamental of the last capture (Hz)
  float    calCents;   // deviation of the last capture vs equal temperament (cents)
};

// ------------
// Module state
// ------------
static TaskHandle_t      s_micTask   = nullptr;
static QueueHandle_t     s_micQueue  = nullptr;
static i2s_chan_handle_t s_rxChan    = nullptr;
static float            *s_ring      = nullptr;   // MIC_RING samples, normalized floats
static float            *s_win       = nullptr;   // MIC_WIN Hann-windowed samples
static uint32_t          s_total     = 0;         // absolute sample counter
static volatile float    s_gain      = 16.0f;     // digital gain (1..64)
static volatile float    s_thr       = 0.1f;      // concentration threshold ider (default 50 -> 0.1)
static volatile float    s_levelDb   = -90.0f;
static volatile float    s_noiseDb   = -70.0f;
static float             s_dc        = 0.0f;      // running DC offset of the (post-gain) samples
static volatile bool     s_running   = false;

static volatile int      s_calArmedNote = -1;     // -1 = disarmed
static volatile bool     s_calDone      = false;
static volatile float    s_calFreq      = 0.0f;
static volatile float    s_calCents     = 0.0f;
static int               s_calLastNote  = -1;

static volatile int      s_lastNote  = -1;
static volatile float    s_lastFreq  = 0.0f;
static volatile float    s_lastCents = 0.0f;
static volatile uint32_t s_lastMs    = 0;

static const volatile uint8_t *s_expected = nullptr;  // pointer to noteChan[128] in the sketch
static float    s_calFreqTable[MIC_NOTE_MAX - MIC_NOTE_MIN + 1];  // 0 = not calibrated
static uint32_t s_lastSentMs[128] = {0};
static uint32_t s_lastOnsetMs = 0;

struct MicActive { uint8_t note; uint32_t offAt; bool used; };
static MicActive s_active[MIC_ACTIVE_SLOTS];

static Preferences s_micPrefs;

// -------
// Helpers
// -------
static inline float micEqualTemp(int note) {
  return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

static inline float micCentsVsEq(int note, float freq) {
  return 1200.0f * log2f(freq / micEqualTemp(note));
}

// Fundamental to use for a note: calibrated value if available, equal temperament otherwise
static inline float micNoteFreq(int note) {
  if (note >= MIC_NOTE_MIN && note <= MIC_NOTE_MAX) {
    const float f = s_calFreqTable[note - MIC_NOTE_MIN];
    if (f > 0.0f) return f;
  }
  return micEqualTemp(note);
}

// Goertzel power of the (already windowed) analysis buffer at an arbitrary frequency
static float micGoertzel(const float *x, int n, float freq) {
  const float w     = 2.0f * (float)M_PI * freq / MIC_SAMPLE_RATE;
  const float coeff = 2.0f * cosf(w);
  float s1 = 0.0f, s2 = 0.0f;
  for (int i = 0; i < n; i++) {
    const float s0 = x[i] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

// Energy concentration of the window on the first 3 partials of f0, bounded in (0,1).
// A real piano note packs most of its energy into its first partials; a broadband transient
// (click, knock) spreads it across the whole band. Single measure shared by both detection
// and calibration.
static float micConcentration(float f0, float energy) {
  const float nyq = MIC_SAMPLE_RATE * 0.47f;
  float p = micGoertzel(s_win, MIC_WIN, f0);
  if (f0 * 2.0f < nyq) p += micGoertzel(s_win, MIC_WIN, f0 * 2.0f);
  if (f0 * 3.0f < nyq) p += micGoertzel(s_win, MIC_WIN, f0 * 3.0f);
  return p / (energy * (float)MIC_WIN * (float)MIC_WIN + 1e-12f);
}

// -----------------------
// Calibration persistence
// -----------------------
static void micLoadCal() {
  memset(s_calFreqTable, 0, sizeof(s_calFreqTable));
  s_micPrefs.begin("plmic", true);
  if (s_micPrefs.isKey("freq"))
    s_micPrefs.getBytes("freq", s_calFreqTable, sizeof(s_calFreqTable));
  s_micPrefs.end();
}

static void micSaveCal() {
  s_micPrefs.begin("plmic", false);
  s_micPrefs.putBytes("freq", s_calFreqTable, sizeof(s_calFreqTable));
  s_micPrefs.end();
}

// -------------------------------------------------
// Analysis of one window (runs inside the mic task)
// -------------------------------------------------
static void micPushEvent(uint8_t note, uint8_t vel, bool on) {
  MicEvent ev = { note, vel, on };
  xQueueSend(s_micQueue, &ev, 0);
}

static void micScheduleOff(uint8_t note, uint32_t now) {
  for (auto &a : s_active) {                    // refresh if already pending
    if (a.used && a.note == note) { a.offAt = now + MIC_NOTEOFF_MS; return; }
  }
  for (auto &a : s_active) {
    if (!a.used) { a.used = true; a.note = note; a.offAt = now + MIC_NOTEOFF_MS; return; }
  }
  micPushEvent(note, 0, false);  // no free slot: immediate note-off rather than a stuck note
}

static void micAnalyzeWindow(float onsetPeakDb) {
  // Copy the last MIC_WIN samples out of the ring and apply a Hann window
  const uint32_t start = s_total - MIC_WIN;
  float energy = 0.0f;
  for (int i = 0; i < MIC_WIN; i++) {
    const float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (MIC_WIN - 1)));
    const float s = s_ring[(start + i) & (MIC_RING - 1)] * w;
    s_win[i] = s;
    energy  += s * s;
  }
  energy /= (float)MIC_WIN;  // mean square of the windowed signal
  if (energy < 1e-10f) return;

  const uint32_t now = millis();

  // Calibration capture: fine scan ±60 cents around the theoretical fundamental
  const int calNote = s_calArmedNote;
  if (calNote >= MIC_NOTE_MIN && calNote <= MIC_NOTE_MAX) {
    const float fRef   = micEqualTemp(calNote);
    const int   nSteps = 31;                       // -60..+60 cents, 4-cent steps
    float bestP = -1.0f;
    int   bestI = 0;
    float pAll[nSteps];
    for (int i = 0; i < nSteps; i++) {
      const float cents = -60.0f + i * 4.0f;
      pAll[i] = micGoertzel(s_win, MIC_WIN, fRef * powf(2.0f, cents / 1200.0f));
      if (pAll[i] > bestP) { bestP = pAll[i]; bestI = i; }
    }
    // Parabolic interpolation on the log-power of the peak and its neighbors
    float cents = -60.0f + bestI * 4.0f;
    if (bestI > 0 && bestI < nSteps - 1) {
      const float pPrev = logf(pAll[bestI - 1] + 1e-12f);
      const float pBest = logf(pAll[bestI]     + 1e-12f);
      const float pNext = logf(pAll[bestI + 1] + 1e-12f);
      const float den   = pPrev - 2.0f * pBest + pNext;
      if (fabsf(den) > 1e-9f)
        cents += 4.0f * 0.5f * (pPrev - pNext) / den;
    }

    // Validity gate: accept the capture only if the window's energy is actually concentrated
    // on the target note's partials (rejects clicks and knocks, which trigger the onset but
    // spread their energy across the whole band). A peak at the edge of the scan means the
    // real maximum lies outside ±60 cents. On rejection we simply stay armed (the web page
    // enforces its own timeout).
    const float fCand = fRef * powf(2.0f, cents / 1200.0f);
    const bool  ok    = (bestI > 0) && (bestI < nSteps - 1)
                     && (micConcentration(fCand, energy) > 0.1f);
    if (!ok)
      return;  // not a clean strike of the target note: keep the capture armed

    s_calFreq  = fCand;
    s_calCents = cents;
    s_calFreqTable[calNote - MIC_NOTE_MIN] = s_calFreq;
    micSaveCal();
    s_calLastNote  = calNote;
    s_calArmedNote = -1;
    s_calDone      = true;
    return;  // a calibration strike is never forwarded as MIDI
  }

  // Targeted detection: for each expected note (lit by Synthesia), the note is deemed played
  // if the energy concentration on its partials exceeds the slider threshold. No
  // disambiguation: an expected note excited by the harmonics of another played note will be
  // confirmed (accepted trade-off: zero missed notes rather than zero impostors).
  if (!s_expected) return;

  const float   thr = s_thr;
  const uint8_t vel = (uint8_t)constrain((int)((onsetPeakDb + 50.0f) * 2.5f) + 20, 20, 127);
  int   bestNote = -1;
  float bestScore = 0.0f, bestF = 0.0f;

  for (int n = MIC_NOTE_MIN; n <= MIC_NOTE_MAX; n++) {
    if (!s_expected[n]) continue;
    const float f  = micNoteFreq(n);
    const float sc = micConcentration(f, energy);
    if (sc < thr) continue;
    if (sc > bestScore) { bestScore = sc; bestNote = n; bestF = f; }
    if (now - s_lastSentMs[n] <= MIC_RETRIG_MS) continue;  // retrigger lockout
    s_lastSentMs[n] = now;
    micPushEvent(n, vel, true);
    micScheduleOff(n, now);
  }

  if (bestNote >= 0) {
    s_lastNote  = bestNote;
    s_lastFreq  = bestF;
    s_lastCents = micCentsVsEq(bestNote, bestF);
    s_lastMs    = now;
  }
}

// --------
// Mic task
// --------
static void micTaskFn(void *) {
  enum { IDLE, ARMED } state = IDLE;
  uint32_t onsetTotal  = 0;
  float    onsetPeakDb = -90.0f;
  int32_t  raw[MIC_HOP];

  for (;;) {
    size_t br = 0;
    if (i2s_channel_read(s_rxChan, raw, sizeof(raw), &br, portMAX_DELAY) != ESP_OK || br == 0)
      continue;

    // Ingest: 24-bit samples left-justified in 32-bit frames -> normalized floats.
    // The INMP441 has a noticeable DC offset; a one-pole tracker removes it (~2.5 Hz high-pass)
    // so that neither the RMS level nor the onset detector is polluted by a constant component.
    const int   n = br / 4;
    const float g = s_gain / 8388608.0f;
    float dc    = s_dc;
    float sumSq = 0.0f;
    for (int i = 0; i < n; i++) {
      const float x = (float)(raw[i] >> 8) * g;
      dc += (x - dc) * MIC_DC_ALPHA;
      const float s = x - dc;
      s_ring[s_total & (MIC_RING - 1)] = s;
      s_total++;
      sumSq += s * s;
    }
    s_dc = dc;
    const float rms = sqrtf(sumSq / (float)n);
    const float db  = 20.0f * log10f(rms + 1e-9f);   // instantaneous 8 ms level, used for onsets

    // Displayed level: envelope with instant attack and slow release, so that a key strike
    // stays visible on a UI polled every few hundred ms instead of falling between two polls
    if (db > s_levelDb) s_levelDb = db;
    else                s_levelDb += (db - s_levelDb) * MIC_ENV_RELEASE;

    // Slow ambient level estimate
    s_noiseDb += (db - s_noiseDb) * MIC_AMB_ALPHA;

    const uint32_t now = millis();

    // Pending automatic note-offs
    for (auto &a : s_active) {
      if (a.used && now >= a.offAt) {
        micPushEvent(a.note, 0, false);
        a.used = false;
      }
    }

    // Onset detection then deferred full-window analysis. Plain absolute threshold: the gain
    // slider directly sets the acoustic sensitivity. While the level stays above it, analyses
    // simply repeat (the per-note retrigger lockout prevents any MIDI spam).
    if (state == IDLE) {
      if (db > MIC_ABS_FLOOR_DB && now - s_lastOnsetMs > MIC_ONSET_GAP_MS) {
        state         = ARMED;
        onsetTotal    = s_total;
        onsetPeakDb   = db;
        s_lastOnsetMs = now;
      }
    }
    else {  // ARMED: wait until a full analysis window has elapsed since the onset
      if (db > onsetPeakDb) onsetPeakDb = db;
      if (s_total - onsetTotal >= MIC_WIN) {
        micAnalyzeWindow(onsetPeakDb);
        state = IDLE;
      }
    }
  }
}

// ----------
// Public API
// ----------
bool micBegin(uint8_t pinSck, uint8_t pinWs, uint8_t pinSd, const volatile uint8_t *expectedByNote) {
  if (s_running) return true;

  s_ring = (float *)malloc(MIC_RING * sizeof(float));
  s_win  = (float *)malloc(MIC_WIN  * sizeof(float));
  if (!s_ring || !s_win) {
    Serial.println("[MIC] Buffer allocation failed");
    free(s_ring); free(s_win);
    s_ring = nullptr; s_win = nullptr;
    return false;
  }
  memset(s_ring, 0, MIC_RING * sizeof(float));
  memset(s_active, 0, sizeof(s_active));

  s_expected = expectedByNote;
  micLoadCal();

  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num  = 6;
  chanCfg.dma_frame_num = MIC_HOP;

  if (i2s_new_channel(&chanCfg, nullptr, &s_rxChan) != ESP_OK) {
    Serial.println("[MIC] i2s_new_channel failed");
    free(s_ring); free(s_win);
    s_ring = nullptr; s_win = nullptr;
    return false;
  }

  i2s_std_config_t stdCfg = {};
  stdCfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE);
  stdCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
  stdCfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  stdCfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  stdCfg.gpio_cfg.bclk = (gpio_num_t)pinSck;
  stdCfg.gpio_cfg.ws   = (gpio_num_t)pinWs;
  stdCfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  stdCfg.gpio_cfg.din  = (gpio_num_t)pinSd;

  if (i2s_channel_init_std_mode(s_rxChan, &stdCfg) != ESP_OK ||
      i2s_channel_enable(s_rxChan) != ESP_OK) {
    Serial.println("[MIC] i2s std init/enable failed");
    i2s_del_channel(s_rxChan);
    s_rxChan = nullptr;
    free(s_ring); free(s_win);
    s_ring = nullptr; s_win = nullptr;
    return false;
  }

  s_micQueue = xQueueCreate(32, sizeof(MicEvent));
  // Core 0 hosts the radio stacks but the DSP load is light (a few ms per key strike)
  xTaskCreatePinnedToCore(micTaskFn, "mic", 6144, nullptr, 2, &s_micTask, 0);

  s_running = true;
  return true;
}

bool micRunning()               { return s_running; }
QueueHandle_t micEvents()       { return s_micQueue; }

void micSetGain(uint8_t g)      { s_gain = (float)constrain((int)g, 1, 64); }
void micSetThreshold(uint8_t t) { s_thr = constrain((int)t, 1, 100) * 0.002f; }  // default 40 -> 0.08

void micGetStatus(MicStatus &o) {
  o.levelDb  = s_levelDb;
  o.noiseDb  = s_noiseDb;
  o.lastNote = s_lastNote;
  o.lastFreq = s_lastFreq;
  o.lastCents= s_lastCents;
  o.lastMs   = s_lastMs;
  o.calArmed = (s_calArmedNote >= 0);
  o.calNote  = (s_calArmedNote >= 0) ? s_calArmedNote : s_calLastNote;
  o.calDone  = s_calDone;
  o.calFreq  = s_calFreq;
  o.calCents = s_calCents;
}

void micArmCal(int note) {
  s_calDone = false;
  s_calArmedNote = (note >= MIC_NOTE_MIN && note <= MIC_NOTE_MAX) ? note : -1;
}

void micClearCal() {
  s_calArmedNote = -1;
  s_calDone      = false;
  memset(s_calFreqTable, 0, sizeof(s_calFreqTable));
  micSaveCal();
}

float micCalFreq(int note) {
  if (note < MIC_NOTE_MIN || note > MIC_NOTE_MAX) return 0.0f;
  return s_calFreqTable[note - MIC_NOTE_MIN];
}

int micCalCount() {
  int c = 0;
  for (float f : s_calFreqTable) if (f > 0.0f) c++;
  return c;
}

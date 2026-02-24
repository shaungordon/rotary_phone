/*
 * Rotary Phone Interface for XIAO ESP32S3
 * with Adafruit I2S MAX98357A Amplifier
 *
 * Generates authentic US telephone tones from the 1970s.
 * Special numbers connect to bird characters that hold an interactive
 * conversation using VAD (energy + ZCR) to detect when the human speaks.
 *
 * Easter egg: dial 867-5309 for Jenny.
 *
 * Remote debug/config via BLE UART (NimBLE-Arduino required):
 *   Device name: "RotaryPhone"
 *   Compatible apps:  Bluefruit Connect (iOS)  /  Serial Bluetooth Terminal (Android)
 *   Commands:  status | get <key> | set <key> <val> | reset
 */

#include <Arduino.h>
#include <driver/i2s.h>
#include <FFat.h>
#include "config.h"
#include "ble_debug.h"

#define FILESYSTEM FFat

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
void initI2S();
void writeSample(int16_t sample);
void writeSilenceMs(uint32_t ms);
bool playWavFile(const char* path);
void playClickSound();
void playAnswerClick();
void playHangupClick();
bool writeSilenceChecked(uint32_t ms);
int countSquawks(const char* birdName);
uint32_t lcgRand();
bool playBirdSquawks(const char* birdName, int squawkCount, int numSquawks);
void vadWarmup();
void vadSampleChunk();
bool vadIsSpeech();
bool runBirdConversation(const char* birdName, int squawkCount);
bool playRingingPreamble();
bool playRingingAndConnect(const char* birdName, int squawkCount);
bool playJennyEasterEgg();
String lookupBirdName(String number);
void processCompletedNumber();
void handleOnHook();
void handleDialTone();
void handleDialing();
void playDisconnectTone();
void handleDisconnect();
void handleOffHookWarning();

// ============================================================================
// GLOBAL INSTANCES
// ============================================================================
ConfigManager cfg;
BleDebug      bleDebug;

// ============================================================================
// DLOG — drop-in replacement for Serial.print/println.
// Writes to both Serial (USB) and BLE UART simultaneously.
// Usage:  DLOG("value=%d\n", x);   DLOG("simple message\n");
// ============================================================================
#define DLOG(...) do { Serial.printf(__VA_ARGS__); bleDebug.printf(__VA_ARGS__); } while(0)

// ============================================================================
// PIN DEFINITIONS
// ============================================================================
#define PIN_IN_USE    1   // D1 - LOW when dialing
#define PIN_PULSE     2   // D2 - Pulses HIGH for each digit
#define PIN_HOOK      3   // D3 - LOW when receiver off hook
#define PIN_MIC_ADC   4   // A4 - Carbon mic analog input (for VAD)

#define PIN_I2S_BCLK  7  // D8
#define PIN_I2S_LRC   8  // D9
#define PIN_I2S_DOUT  9  // D10

// ============================================================================
// CONSTANTS
// ============================================================================
#define SAMPLE_RATE          8000   // POTS standard — authentic 1970s phone sound
#define DEBOUNCE_MS          20     // Debounce time for all pins
#define DIGITS_IN_PHONE_NUM  7      // 7-digit phone numbers
#define IDLE_TIMEOUT_MS      10000  // 10 seconds before off-hook warning
#define NUM_RINGS            2      // Number of rings before "answering"

// ============================================================================
// VAD CONSTANTS (for bird conversation mic input)
// ============================================================================
// These are fallback #defines used only before cfg.load() completes.
// At runtime the Config struct values (loaded from NVS) take precedence.
// Calibrated for carbon handset mic, 12-bit ADC, ADC_SCALE=2048:
//   Silence p-p ~600 counts  → AC-RMS ≈ 0.104 normalised
//   Voice p-p  ~1680 counts  → AC-RMS ≈ 0.290 normalised
#define VAD_CHUNK_SIZE            240     // 30ms at 8kHz
#define VAD_SMOOTHING             0.3f    // EMA factor for energy & ZCR
#define VAD_DC_BIAS_ALPHA         0.003f  // Slow EMA for DC tracking (~10s tau)
#define ADC_SCALE                 2048.0f // Half of 12-bit range

// Bird conversation timing (fixed - not adjustable via BLE)
#define BIRD_PAUSE_TRIGGER_MS     800     // Silence after speech → bird responds
#define BIRD_LOCKOUT_MS           2000    // Don't listen while bird is squawking
#define BIRD_INITIAL_SQUAWK_MS    1200    // Pause before bird's opening squawk
#define BIRD_PAUSE_BEFORE_MS      200     // Pause before squawk sequence
#define BIRD_PAUSE_BETWEEN_MS     100     // Pause between squawks in a sequence
#define BIRD_PAUSE_AFTER_MS       400     // Pause after squawk sequence
#define MAX_SQUAWKS_PER_BIRD      16      // Maximum squawk files to probe for

// ============================================================================
// PHONE STATE MACHINE
// ============================================================================
enum PhoneState {
  STATE_ON_HOOK,        // Receiver on hook, idle
  STATE_DIAL_TONE,      // Receiver off hook, playing dial tone
  STATE_DIALING,        // User is dialing a number
  STATE_CALL_CONNECTED, // In a bird conversation (or just finished)
  STATE_DISCONNECT,     // Bird hung up: dead silence (with hiss)
  STATE_ERROR,          // Invalid number or can't connect
  STATE_OFF_HOOK_WARN   // Off-hook warning tone (Howler)
};

PhoneState currentState = STATE_ON_HOOK;
bool offHookWarningInitialized = false;
bool dialToneInitialized = false;
bool interceptRecordingPlayed = false;
bool disconnectTonePlayed = false;    // Track whether disconnect tone has run
unsigned long disconnectStartMs = 0; // When we entered STATE_DISCONNECT

// ============================================================================
// DIALING STATE
// ============================================================================
String dialedNumber = "";
int currentDigit = 0;
unsigned long lastActivityTime = 0;

// ============================================================================
// PIN DEBOUNCING
// ============================================================================
struct DebounceState {
  bool lastState;
  bool currentState;
  unsigned long lastChangeTime;
};

DebounceState hookDebounce  = {HIGH, HIGH, 0};
DebounceState inUseDebounce = {HIGH, HIGH, 0};
DebounceState pulseDebounce = {LOW,  LOW,  0};

// ============================================================================
// SPECIAL PHONE NUMBERS
// ============================================================================
struct PhoneNumber {
  String number;
  String birdName;  // e.g. "osprey" → squawk files: /audio/squawk_osprey_1.wav ...
};

PhoneNumber specialNumbers[] = {
  {"5551234", "osprey"},
  {"3218273", "magpie"},
  {"9253162", "cockatoo"},
  // Add more special numbers as needed
};

const int numSpecialNumbers = sizeof(specialNumbers) / sizeof(specialNumbers[0]);

// ============================================================================
// TONE GENERATION
// ============================================================================
class ToneGenerator {
private:
  float   phases[4];
  float   frequencies[4];
  int     numFrequencies;
  int16_t amplitude;

public:
  ToneGenerator() : numFrequencies(0), amplitude(8000) {
    for (int i = 0; i < 4; i++) { phases[i] = 0; frequencies[i] = 0; }
  }

  void setTone(float f1, float f2 = 0, float f3 = 0, float f4 = 0) {
    frequencies[0] = f1; frequencies[1] = f2;
    frequencies[2] = f3; frequencies[3] = f4;
    numFrequencies = (f1>0) + (f2>0) + (f3>0) + (f4>0);
    for (int i = 0; i < 4; i++) phases[i] = 0;
  }

  void setAmplitude(int16_t amp) { amplitude = amp; }

  int16_t getSample() {
    if (numFrequencies == 0) return 0;
    float mixed = 0;
    for (int i = 0; i < numFrequencies; i++) {
      mixed += sin(phases[i]);
      phases[i] += 2.0f * PI * frequencies[i] / SAMPLE_RATE;
      if (phases[i] >= 2.0f * PI) phases[i] -= 2.0f * PI;
    }
    return (int16_t)(mixed * amplitude / numFrequencies);
  }

  void stop() { numFrequencies = 0; }
};

ToneGenerator toneGen;

// ============================================================================
// CADENCED TONE
// ============================================================================
class CadencedTone {
private:
  unsigned long onDuration, offDuration, lastToggleTime;
  bool isOn;

public:
  CadencedTone() : onDuration(0), offDuration(0), lastToggleTime(0), isOn(false) {}

  void setCadence(unsigned long onMs, unsigned long offMs) {
    onDuration = onMs; offDuration = offMs;
    lastToggleTime = millis(); isOn = true;
  }

  bool isActive() {
    if (onDuration == 0) return true;
    unsigned long now = millis();
    unsigned long elapsed = now - lastToggleTime;
    if (isOn) {
      if (elapsed >= onDuration) {
        isOn = false;
        lastToggleTime = now;
      }
    } else {
      if (elapsed >= offDuration) {
        isOn = true;
        lastToggleTime = now;
      }
    }
    return isOn;
  }

  void stop() { onDuration = 0; offDuration = 0; isOn = false; }
};

CadencedTone cadence;

// true while a call is connected (bird conversation or Jenny easter egg).
// writeSample uses full hiss amplitude (±20) during connected call, half (±10) elsewhere.
static bool g_callConnected = false;

// ============================================================================
// I2S AUDIO OUTPUT
// ============================================================================
void initI2S() {
  i2s_config_t icfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = 0,
    .dma_buf_count        = 8,
    .dma_buf_len          = 64,
    .use_apll             = false,
    .tx_desc_auto_clear   = true,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = PIN_I2S_BCLK,
    .ws_io_num    = PIN_I2S_LRC,
    .data_out_num = PIN_I2S_DOUT,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_0, &icfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
}

// Write one stereo sample pair with volume scaling applied.
// Adds subtle "comfort noise" (line hiss) whenever the phone is off-hook.
void writeSample(int16_t sample) {
  // Add background hiss if off-hook.
  // Full amplitude (±20) during connected call; half (±10) everywhere else.
  if (digitalRead(PIN_HOOK) == LOW) {
    int16_t amp = g_callConnected ? 20 : 10;
    sample += (int16_t)((rand() % (2 * amp + 1)) - amp);
  }

  int16_t scaled = (int16_t)(sample * cfg.current.volume);
  int16_t stereo[2] = {scaled, scaled};
  size_t written;
  i2s_write(I2S_NUM_0, stereo, sizeof(stereo), &written, portMAX_DELAY);
}

// Write silence for a given number of milliseconds.
// Comfort noise is added automatically by writeSample().
void writeSilenceMs(uint32_t ms) {
  uint32_t samples = (uint32_t)SAMPLE_RATE * ms / 1000;
  for (uint32_t i = 0; i < samples; i++) {
    // If the phone is on-hook during silence, stop hiss immediately
    writeSample(0);
  }
}

// ============================================================================
// DEBOUNCE
// ============================================================================
bool debounceRead(int pin, DebounceState &state) {
  bool reading = digitalRead(pin);
  unsigned long now = millis();
  if (reading != state.lastState) state.lastChangeTime = now;
  if ((now - state.lastChangeTime) > DEBOUNCE_MS && reading != state.currentState)
    state.currentState = reading;
  state.lastState = reading;
  return state.currentState;
}

// ============================================================================
// WAV PLAYBACK
// Returns true  = playback completed normally.
// Returns false = interrupted (phone hung up).
// ============================================================================
bool playWavFile(const char* path) {
  DLOG("[Audio] Opening: %s\n", path);

  if (!FILESYSTEM.exists(path)) {
    DLOG("[Audio] ERROR: File not found: %s\n", path);
    return true;  // Not interrupted, just missing
  }

  File file = FILESYSTEM.open(path, "r");
  if (!file || file.size() < 44) {
    DLOG("[Audio] ERROR: Cannot open or too small\n");
    if (file) file.close();
    return true;
  }

  uint8_t header[44];
  if (file.read(header, 44) != 44 ||
      header[0]!='R' || header[1]!='I' || header[2]!='F' || header[3]!='F' ||
      header[8]!='W' || header[9]!='A' || header[10]!='V'|| header[11]!='E') {
    DLOG("[Audio] ERROR: Invalid WAV header\n");
    file.close(); return true;
  }

  uint16_t audioFormat    = header[20] | (header[21] << 8);
  uint16_t numChannels    = header[22] | (header[23] << 8);
  uint32_t fileSampleRate = header[24] | (header[25]<<8) | (header[26]<<16) | (header[27]<<24);
  uint16_t bitsPerSample  = header[34] | (header[35] << 8);

  if (audioFormat != 1) {
    DLOG("[Audio] ERROR: Only PCM WAV supported\n");
    file.close(); return true;
  }

  // Find data chunk
  uint32_t dataSize = 0;
  bool dataFound = false;
  file.seek(36);
  uint8_t chunkHdr[8];
  while (file.available() >= 8) {
    file.read(chunkHdr, 8);
    if (chunkHdr[0]=='d' && chunkHdr[1]=='a' && chunkHdr[2]=='t' && chunkHdr[3]=='a') {
      dataSize = chunkHdr[4] | (chunkHdr[5]<<8) | (chunkHdr[6]<<16) | (chunkHdr[7]<<24);
      dataFound = true; break;
    }
    uint32_t skip = chunkHdr[4] | (chunkHdr[5]<<8) | (chunkHdr[6]<<16) | (chunkHdr[7]<<24);
    file.seek(file.position() + skip);
  }

  if (!dataFound) {
    DLOG("[Audio] ERROR: No data chunk\n");
    file.close(); return true;
  }

  // Stream with resampling and hook interrupt check
  const size_t BUF = 512;
  uint8_t buf[BUF];
  uint32_t bytesPerSample = (bitsPerSample / 8) * numChannels;
  uint32_t bytesRead = 0;
  float resampleAccum = 0.0f;
  float resampleStep  = (float)fileSampleRate / (float)SAMPLE_RATE;
  bool interrupted = false;

  while (bytesRead < dataSize && file.available()) {
    if (digitalRead(PIN_HOOK) == HIGH) {
      DLOG("[Audio] Interrupted: on hook\n");
      interrupted = true; break;
    }
    size_t toRead = min((uint32_t)BUF, dataSize - bytesRead);
    toRead = (toRead / bytesPerSample) * bytesPerSample;
    if (toRead == 0) break;
    size_t got = file.read(buf, toRead);
    bytesRead += got;

    for (size_t i = 0; i + bytesPerSample <= got; i += bytesPerSample) {
      int16_t sample = 0;
      if      (bitsPerSample == 16) sample = (int16_t)(buf[i] | (buf[i+1] << 8));
      else if (bitsPerSample == 8)  sample = ((int16_t)buf[i] - 128) << 8;
      resampleAccum += 1.0f;
      while (resampleAccum >= resampleStep) {
        writeSample(sample);
        resampleAccum -= resampleStep;
      }
    }
  }

  file.close();
  return !interrupted;
}

// ============================================================================
// SYNTHESIZED SOUNDS
// ============================================================================
void playClickSound() {
  toneGen.setAmplitude(3000);
  toneGen.setTone(2000, 3500);
  for (int i = 0; i < SAMPLE_RATE * 2 / 1000; i++) writeSample(toneGen.getSample());
  toneGen.setTone(800);
  for (int i = 0; i < SAMPLE_RATE * 3 / 1000; i++) {
    int16_t s = toneGen.getSample();
    s = (int16_t)(s * (1.0f - (float)i / (SAMPLE_RATE * 3 / 1000)));
    writeSample(s);
  }
  toneGen.stop();
}

// Answer click: someone on the other end picks up their handset.
// Sharp electrical click (circuit closes) followed by a brief low thud
// (handset leaving cradle), then silence while they raise it to their ear.
// Research: 250-550ms from pick-up to first word; 400ms is natural mid-point.
void playAnswerClick() {
  // --- Sharp electrical click: dominant event, circuit snapping closed ---
  toneGen.setAmplitude(3200);
  toneGen.setTone(1800, 3600);
  int click = SAMPLE_RATE * 4 / 1000;  // 4ms - short and sharp
  for (int i = 0; i < click; i++) {
    float env = 1.0f - (float)i / click;
    writeSample((int16_t)(toneGen.getSample() * env));
  }
  toneGen.stop();

  // --- Low thud: handset leaving cradle ---
  toneGen.setTone(80, 200);
  toneGen.setAmplitude(20000);  // High pre-scale; after VOLUME = prominent thud
  int thud = SAMPLE_RATE * 12 / 1000;  // 12ms
  for (int i = 0; i < thud; i++) {
    float env = 1.0f - (float)i / thud; env *= env;  // Squared = faster dropoff
    writeSample((int16_t)(toneGen.getSample() * env));
  }
  toneGen.stop();

  // --- Silence: raising handset to ear (400ms) ---
  writeSilenceMs(400);
}

// Simulated hang-up click from the bird's end.
void playHangupClick() {
  // --- Contact bounce: 3 tiny high-freq rebounds ---
  int16_t bounceAmps[3]  = {2200, 1400, 800};
  int     bounceLenMs[3] = {3,    2,    1};
  int     gapMs          = 1;

  for (int b = 0; b < 3; b++) {
    toneGen.setTone(3500, 6000);
    toneGen.setAmplitude(bounceAmps[b]);
    int len = SAMPLE_RATE * bounceLenMs[b] / 1000;
    for (int i = 0; i < len; i++) {
      float env = 1.0f - (float)i / len;
      writeSample((int16_t)(toneGen.getSample() * env));
    }
    int gap = SAMPLE_RATE * gapMs / 1000;
    toneGen.stop();
    for (int i = 0; i < gap; i++) writeSample(0);
  }

  // --- Electrical pop: circuit opening transient ---
  toneGen.setTone(2800, 4800);
  toneGen.setAmplitude(3000);
  int pop = SAMPLE_RATE * 5 / 1000;
  for (int i = 0; i < pop; i++) {
    float t   = (float)i / pop;
    float env = expf(-4.0f * t);
    writeSample((int16_t)(toneGen.getSample() * env));
  }

  // --- Cradle thud: handset weight hitting cradle ---
  toneGen.setTone(80, 160);
  toneGen.setAmplitude(28000);
  int thud = SAMPLE_RATE * 55 / 1000;
  for (int i = 0; i < thud; i++) {
    float t   = (float)i / thud;
    float env = expf(-5.5f * t);
    writeSample((int16_t)(toneGen.getSample() * env));
  }

  toneGen.stop();
}

// ============================================================================
// HOOK-INTERRUPTIBLE SILENCE
// Returns false immediately if the phone is hung up during the wait.
// ============================================================================
bool writeSilenceChecked(uint32_t ms) {
  uint32_t samples = (uint32_t)SAMPLE_RATE * ms / 1000;
  for (uint32_t i = 0; i < samples; i++) {
    if (digitalRead(PIN_HOOK) == HIGH) return false;
    // Ensure loop yields to system/BLE tasks
    if (i % 500 == 0) yield();
    // writeSample handles comfort noise automatically
    writeSample(0);
  }
  return true;
}

// ============================================================================
// SQUAWK FILE POOL DISCOVERY
// Probes /audio/squawk_<birdName>_1.wav, _2.wav, ... until one is missing.
// Returns the count found (0 if none).
// ============================================================================
int countSquawks(const char* birdName) {
  int count = 0;
  for (int n = 1; n <= MAX_SQUAWKS_PER_BIRD; n++) {
    char path[64];
    snprintf(path, sizeof(path), "/audio/squawk_%s_%d.wav", birdName, n);
    if (!FILESYSTEM.exists(path)) break;
    count++;
  }
  DLOG("[Bird] Squawk pool for '%s': %d file(s)\n", birdName, count);
  return count;
}

// ============================================================================
// RANDOM (simple LCG, seeded in setup)
// ============================================================================
uint32_t lcgState = 42;
uint32_t lcgRand() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState;
}

// ============================================================================
// BIRD SQUAWK PLAYBACK
// Plays 1-3 random squawks from the bird's pool.
// Returns false if interrupted by on-hook at any point.
// ============================================================================
bool playBirdSquawks(const char* birdName, int squawkCount, int numSquawks) {
  if (squawkCount == 0) {
    DLOG("[Bird] No squawk files found - skipping\n");
    return true;
  }

  DLOG("[Bird] Playing %d squawk(s)\n", numSquawks);

  // Initial silence (with hiss)
  if (!writeSilenceChecked(BIRD_PAUSE_BEFORE_MS)) return false;

  for (int i = 0; i < numSquawks; i++) {
    int idx = 1 + (int)(lcgRand() % squawkCount);
    char path[64];
    snprintf(path, sizeof(path), "/audio/squawk_%s_%d.wav", birdName, idx);
    DLOG("[Bird]   %s\n", path);
    
    if (!playWavFile(path)) return false;

    // Small extra silence buffer to ensure hiss continues immediately after file closes
    if (!writeSilenceChecked(50)) return false;

    if (i < numSquawks - 1) {
      if (!writeSilenceChecked(BIRD_PAUSE_BETWEEN_MS)) return false;
    }
  }

  if (!writeSilenceChecked(BIRD_PAUSE_AFTER_MS)) return false;
  return true;
}

// ============================================================================
// VAD GLOBALS (used only inside runBirdConversation)
// ============================================================================
float vadMidpoint       = 900.0f;
float vadSmoothedEnergy = 0.0f;
float vadSmoothedZCR    = 0.0f;

void vadWarmup() {
  // Read ADC to establish DC midpoint bias, while keeping DMA fed to avoid silence during warmup.
  // Interleave short bursts of I2S writes to prevent DMA buffer drain.
  for (int i = 0; i < 200; i++) {
    if (i % 50 == 0) writeSilenceMs(5);  // refill DMA every 50 ADC reads (~5ms)
    analogRead(PIN_MIC_ADC);
    delayMicroseconds(100);
  }
  int32_t sum = 0;
  for (int i = 0; i < 100; i++) {
    if (i % 50 == 0) writeSilenceMs(5);  // refill DMA every 50 ADC reads (~5ms)
    sum += analogRead(PIN_MIC_ADC);
    delayMicroseconds(100);
  }
  vadMidpoint = (float)sum / 100.0f;
  vadSmoothedEnergy = 0.0f;
  vadSmoothedZCR    = 0.0f;
  DLOG("[VAD] Warmup done. Initial midpoint=%.1f\n", vadMidpoint);
}

// Sample one 30ms chunk, update vadSmoothedEnergy and vadSmoothedZCR.
void vadSampleChunk() {
  float sumSq = 0.0f, chunkSum = 0.0f;
  int crossings = 0;
  bool prevAbove = false, first = true;

  for (int i = 0; i < VAD_CHUNK_SIZE; i++) {
    int raw = analogRead(PIN_MIC_ADC);
    chunkSum += raw;
    float norm = (raw - vadMidpoint) / ADC_SCALE;
    sumSq += norm * norm;
    bool above = (raw > vadMidpoint);
    if (!first && above != prevAbove) crossings++;
    prevAbove = above; first = false;
  }

  float mean = chunkSum / VAD_CHUNK_SIZE;
  vadMidpoint = (1.0f - VAD_DC_BIAS_ALPHA) * vadMidpoint + VAD_DC_BIAS_ALPHA * mean;

  float rms = sqrtf(sumSq / VAD_CHUNK_SIZE);
  vadSmoothedEnergy = VAD_SMOOTHING * vadSmoothedEnergy + (1.0f - VAD_SMOOTHING) * rms;
  vadSmoothedZCR    = VAD_SMOOTHING * vadSmoothedZCR    + (1.0f - VAD_SMOOTHING) * (float)crossings;
}

bool vadIsSpeech() {
  return (vadSmoothedEnergy > cfg.current.vadEnergyThreshold) &&
         (vadSmoothedZCR >= (float)cfg.current.vadZcrThreshold);
}

// ============================================================================
// BIRD CONVERSATION
// Called after ringing + answer click. Runs the full interactive loop.
// Returns true  = bird hung up (play disconnect sequence).
// Returns false = human hung up (reset immediately, already handled by loop()).
// ============================================================================
bool runBirdConversation(const char* birdName, int squawkCount) {
  DLOG("[Conv] Starting conversation with %s\n", birdName);

  // --- Opening squawk after a natural pause ---
  if (!writeSilenceChecked(BIRD_INITIAL_SQUAWK_MS)) return false;
  int openingSquawks = 1 + (int)(lcgRand() % 2);  // 1 or 2 to open
  if (!playBirdSquawks(birdName, squawkCount, openingSquawks)) return false;

  // --- VAD setup ---
  vadWarmup();
  writeSilenceMs(64);   // pre-fill DMA to capacity before VAD loop

  enum ConvVADState { CSIL, CSPEAK, CPAUSE, CLOCKOUT };
  ConvVADState cvs = CSIL;
  unsigned long speechStartMs  = 0;
  unsigned long pauseStartMs   = 0;
  unsigned long lockoutStartMs = 0;
  unsigned long lastSpeechMs   = millis();

  DLOG("[Conv] Listening...\n");

  while (true) {
    if (digitalRead(PIN_HOOK) == HIGH) {
      DLOG("[Conv] Human hung up\n");
      return false;
    }

    unsigned long now = millis();
    vadSampleChunk();           // ADC first — drains buffer ~12-16 samples at 8kHz
    writeSilenceMs(30);         // refills deficit immediately, then rate-limited
    bool speech = vadIsSpeech();

    // Periodic readout for threshold tuning (~every 300ms).
    // Shows live energy/ZCR vs current thresholds so you can calibrate.
    static uint8_t vadLogTick = 0;
    if (++vadLogTick >= 10) {
      vadLogTick = 0;
      DLOG("[VAD] e=%.4f(>%.4f) z=%.1f(>=%d) %s\n",
           vadSmoothedEnergy, cfg.current.vadEnergyThreshold,
           vadSmoothedZCR,    cfg.current.vadZcrThreshold,
           speech ? "SPEECH" : "sil");
    }

    // Idle timeout: no speech detected for birdIdleTimeoutMs
    if (cvs == CSIL && (now - lastSpeechMs) >= cfg.current.birdIdleTimeoutMs) {
      DLOG("[Conv] Idle timeout - bird hanging up\n");

      playBirdSquawks(birdName, squawkCount, 2);
      if (digitalRead(PIN_HOOK) == HIGH) return false;

      if (!writeSilenceChecked(1500)) return false;

      // Returning true signals the main loop to handle the disconnect sequence (including hangup click)
      return true;
    }

    switch (cvs) {
      case CSIL:
        if (speech) {
          speechStartMs = now;
          lastSpeechMs  = now;
          cvs = CSPEAK;
          DLOG("[VAD] SILENCE -> SPEAKING\n");
        }
        break;

      case CSPEAK:
        if (speech) lastSpeechMs = now;
        if (!speech) {
          pauseStartMs = now;
          cvs = CPAUSE;
          DLOG("[VAD] SPEAKING -> PAUSE\n");
        }
        break;

      case CPAUSE:
        if (speech) {
          lastSpeechMs = now;
          cvs = CSPEAK;
          DLOG("[VAD] PAUSE -> SPEAKING (resumed)\n");
        } else if (now - pauseStartMs >= BIRD_PAUSE_TRIGGER_MS) {
          unsigned long dur = pauseStartMs - speechStartMs;
          if (dur >= cfg.current.birdMinSpeechMs) {
            int numSq = 1 + (int)(lcgRand() % 3);
            DLOG("[Conv] Responding with %d squawk(s)\n", numSq);
            if (!playBirdSquawks(birdName, squawkCount, numSq)) return false;
            lockoutStartMs = millis();
            cvs = CLOCKOUT;
          } else {
            DLOG("[VAD] PAUSE -> SILENCE (too short)\n");
            cvs = CSIL;
          }
        }
        break;

      case CLOCKOUT:
        if (now - lockoutStartMs >= BIRD_LOCKOUT_MS) {
          vadSmoothedEnergy = 0.0f;
          vadSmoothedZCR    = 0.0f;
          cvs = CSIL;
          DLOG("[VAD] LOCKOUT -> SILENCE\n");
        }
        break;
    }
  }
}

// ============================================================================
// RINGING PREAMBLE (shared by normal calls and easter egg)
// Plays NUM_RINGS rings, then a short gap, then the answer click.
// Returns true  = connected (hook still down after click).
// Returns false = interrupted (human hung up).
// ============================================================================
bool playRingingPreamble() {
  for (int ring = 0; ring < NUM_RINGS; ring++) {
    bool isLast = (ring == NUM_RINGS - 1);

    toneGen.setTone(440, 480);
    toneGen.setAmplitude(8000);
    unsigned long t = millis();
    while (millis() - t < 2000) {
      if (digitalRead(PIN_HOOK) == HIGH) { toneGen.stop(); return false; }
      writeSample(toneGen.getSample());
    }
    toneGen.stop();

    if (!isLast) {
      unsigned long s = millis();
      while (millis() - s < 4000) {
        if (digitalRead(PIN_HOOK) == HIGH) return false;
        writeSample(0);
      }
    }
  }

  // Short gap between last ring and answer click (prevents them blending)
  if (!writeSilenceChecked(250)) return false;

  if (digitalRead(PIN_HOOK) == HIGH) return false;
  playAnswerClick();

  return (digitalRead(PIN_HOOK) == LOW);
}

// ============================================================================
// RINGING + CONNECT (normal bird call)
// Returns true if bird hung up (caller should enter STATE_DISCONNECT),
// false if human hung up (loop() hook detection handles reset).
// ============================================================================
bool playRingingAndConnect(const char* birdName, int squawkCount) {
  DLOG("[Audio] Ringing...\n");
  if (!playRingingPreamble()) return false;
  g_callConnected = true;   // full hiss from answer click through conversation
  bool birdHungUp = runBirdConversation(birdName, squawkCount);
  g_callConnected = false;  // back to half hiss after bird conversation ends
  if (birdHungUp) lastActivityTime = millis(); // Reset timer for silence delay
  return birdHungUp;
}

// ============================================================================
// EASTER EGG: 867-5309 (Jenny)
// Plays jenny.wav, then an answering machine beep, then a trailing beep,
// and finally transitions to the authentic disconnect sequence.
// Returns true (machine hung up).
// ============================================================================
bool playJennyEasterEgg() {
  DLOG("[Easter] 867-5309 - Jenny!\n");

  // Play jenny.wav (hook-interruptible)
  if (!playWavFile("/audio/jenny.wav")) return false;

  // Brief pause before answering machine beep
  if (!writeSilenceChecked(150)) return false;

  // Classic 1980s answering machine start beep: 1400Hz, ~500ms
  DLOG("[Easter] Playing machine start beep\n");
  toneGen.setTone(1400);
  toneGen.setAmplitude(6000);
  int beepSamples = SAMPLE_RATE * 500 / 1000;
  for (int i = 0; i < beepSamples; i++) {
    if (digitalRead(PIN_HOOK) == HIGH) { toneGen.stop(); return false; }
    writeSample(toneGen.getSample());
  }
  toneGen.stop();

  // Wait 30 seconds (standard mid-80s answering machine message window)
  if (!writeSilenceChecked(30000)) return false;

  // Ending beep: same as start beep
  DLOG("[Easter] Playing machine end beep\n");
  toneGen.setTone(1400);
  toneGen.setAmplitude(6000);
  for (int i = 0; i < beepSamples; i++) {
    if (digitalRead(PIN_HOOK) == HIGH) { toneGen.stop(); return false; }
    writeSample(toneGen.getSample());
  }
  toneGen.stop();

  // Brief pause before the mechanical hangup click
  if (!writeSilenceChecked(500)) return false;

  lastActivityTime = millis(); // Reset timer for silence delay
  return true; // Machine hung up
}

// ============================================================================
// PHONE NUMBER PROCESSING
// ============================================================================
String lookupBirdName(String number) {
  for (int i = 0; i < numSpecialNumbers; i++) {
    if (specialNumbers[i].number == number)
      return specialNumbers[i].birdName;
  }
  return "";
}

void processCompletedNumber() {
  DLOG("[Call] Number dialed: %s\n", dialedNumber.c_str());

    // Operator
    if (dialedNumber == "0") {
        DLOG("[Call] Connecting to operator\n");
        currentState = STATE_CALL_CONNECTED;
        if (playWavFile("/audio/operator.wav") && digitalRead(PIN_HOOK) == LOW) {
          DLOG("[Call] Operator hung up - entering disconnect sequence\n");
          playHangupClick();
          currentState = STATE_DISCONNECT;
          disconnectTonePlayed = false;
          disconnectStartMs = millis();
        } else {
          lastActivityTime = millis();
        }
        return;
    }

  // Easter egg: 867-5309 (Jenny)
  if (dialedNumber == "8675309") {
    DLOG("[Call] Easter egg: 867-5309\n");
    currentState = STATE_CALL_CONNECTED;
    if (playRingingPreamble()) {
      g_callConnected = true;
      bool done = playJennyEasterEgg();
      g_callConnected = false;
      if (done && digitalRead(PIN_HOOK) == LOW) {
        DLOG("[Call] Machine hung up - entering disconnect sequence\n");
        playHangupClick();
        currentState = STATE_DISCONNECT;
        disconnectTonePlayed = false;
        disconnectStartMs = millis();
      } else {
        lastActivityTime = millis();
      }
    } else {
      lastActivityTime = millis();
    }
    return;
  }

  if (dialedNumber.length() == DIGITS_IN_PHONE_NUM) {
    String birdName = lookupBirdName(dialedNumber);

    if (birdName != "") {
      DLOG("[Call] Bird: %s\n", birdName.c_str());
      currentState = STATE_CALL_CONNECTED;
      int squawkCount = countSquawks(birdName.c_str());
      bool birdHungUp = playRingingAndConnect(birdName.c_str(), squawkCount);
      if (birdHungUp && digitalRead(PIN_HOOK) == LOW) {
        DLOG("[Call] Bird hung up - entering disconnect sequence\n");
        
        // 1970s authenticity: play the physical hangup click before silence
        playHangupClick();
        
        currentState = STATE_DISCONNECT;
        disconnectTonePlayed = false;
        disconnectStartMs = millis();
      } else {
        lastActivityTime = millis();
      }
    } else {
      DLOG("[Call] Number not recognized\n");
      currentState = STATE_ERROR;
      playWavFile("/audio/cannot_connect.wav");
      lastActivityTime = millis();
    }
  }
}

// ============================================================================
// STATE MACHINE HANDLERS
// ============================================================================
void handleOnHook()   { writeSample(0); }

void handleDialTone() {
  if (!dialToneInitialized) {
    toneGen.setTone(350, 440);
    toneGen.setAmplitude(8000);
    dialToneInitialized = true;
  }
  writeSample(toneGen.getSample());
  if (millis() - lastActivityTime > IDLE_TIMEOUT_MS) {
    DLOG("[Timeout] Dial tone -> off-hook warning\n");
    currentState = STATE_OFF_HOOK_WARN;
    toneGen.stop(); dialToneInitialized = false;
  }
}

void handleDialing() {
  writeSample(0);
  if (millis() - lastActivityTime > IDLE_TIMEOUT_MS) {
    DLOG("[Timeout] Dialing -> off-hook warning\n");
    currentState = STATE_OFF_HOOK_WARN;
    dialedNumber = "";
  }
}

void playDisconnectTone() {
  DLOG("[Audio] Disconnect tone (2 cycles)\n");
  for (int cycle = 0; cycle < 2; cycle++) {
    toneGen.setTone(480, 620);
    toneGen.setAmplitude(7000);
    unsigned long t = millis();
    while (millis() - t < 500) {
      if (digitalRead(PIN_HOOK) == HIGH) { toneGen.stop(); return; }
      writeSample(toneGen.getSample());
    }
    toneGen.stop();
    unsigned long s = millis();
    while (millis() - s < 500) {
      if (digitalRead(PIN_HOOK) == HIGH) return;
      writeSample(0);
    }
  }
}

void handleDisconnect() {
  // 1970s Network Authenticity: After the bird hangs up, the line stays 
  // silent (with comfort noise) until the off-hook timeout triggers the howler.
  // No immediate beeps, no automatic return of dial tone.
  
  if (!disconnectTonePlayed) {
    // We already played the hangup click before entering this state.
    disconnectTonePlayed = true;
    disconnectStartMs = millis();
    DLOG("[Disconnect] Line is now silent. Waiting for off-hook timeout.\n");
  }

  writeSample(0); // writeSample adds comfort noise automatically

  if (millis() - lastActivityTime > IDLE_TIMEOUT_MS) {
    DLOG("[Timeout] Post-disconnect silence -> off-hook warning\n");
    currentState = STATE_OFF_HOOK_WARN;
    offHookWarningInitialized = false;
  }
}

void handleOffHookWarning() {
  if (!offHookWarningInitialized) {
    if (!interceptRecordingPlayed) {
      playWavFile("/audio/intercept_offhook.wav");
      interceptRecordingPlayed = true;
    }
    // High-volume multi-frequency ROH (Receiver Off Hook) Howl
    toneGen.setTone(1400, 2060, 2450, 2600);
    toneGen.setAmplitude(16000);
    // Authentic ROH Howl pulses: 0.1s on, 0.1s off
    cadence.setCadence(100, 100);
    offHookWarningInitialized = true;
    DLOG("[Audio] Off-hook warning (ROH Howler) started\n");
  }

  // Debug: Log cadence state every 100ms
  static unsigned long lastLog = 0;
  bool active = cadence.isActive();
  if (millis() - lastLog > 100) {
    // DLOG("[Howl] active=%d\n", active); // Uncomment if still failing
    lastLog = millis();
  }

  writeSample(active ? toneGen.getSample() : 0);
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Seed random generator for authentic hiss and bird logic
  srand(analogRead(PIN_MIC_ADC) ^ millis());

  // Load config from NVS (or defaults if first boot)
  cfg.load();

  Serial.println("\n\n=================================");
  Serial.println("Rotary Phone Interface");
  Serial.printf("Volume: %d%%\n", (int)(cfg.current.volume * 100));
  Serial.println("=================================\n");

  pinMode(PIN_HOOK,    INPUT_PULLUP);
  pinMode(PIN_IN_USE,  INPUT_PULLUP);
  pinMode(PIN_PULSE,   INPUT_PULLUP);
  pinMode(PIN_MIC_ADC, INPUT);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  initI2S();
  Serial.println("[Init] I2S initialized");

  if (!FILESYSTEM.begin(true)) {
    Serial.println("[Init] LittleFS mount failed!");
  } else {
    Serial.println("[Init] LittleFS mounted");
    for (int i = 0; i < numSpecialNumbers; i++) {
      countSquawks(specialNumbers[i].birdName.c_str());
    }
  }

  lcgState = (uint32_t)analogRead(PIN_MIC_ADC) ^ (uint32_t)millis();

  // Start BLE advertising (after LittleFS so Serial is fully up)
  bleDebug.begin("RotaryPhone");

  // Log current config over both Serial and BLE ring buffer
  DLOG("[Config] volume=%.2f vadEnergy=%.3f vadZcr=%d minSpeech=%u idleTimeout=%u\n",
       cfg.current.volume,
       cfg.current.vadEnergyThreshold,
       cfg.current.vadZcrThreshold,
       cfg.current.birdMinSpeechMs,
       cfg.current.birdIdleTimeoutMs);

  Serial.println("[Ready] Waiting for phone activity...\n");
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  // Process any pending BLE commands (set/get/status/reset)
  bleDebug.loop();

  bool hookOffHook = !debounceRead(PIN_HOOK,    hookDebounce);
  bool inUseActive = !debounceRead(PIN_IN_USE,  inUseDebounce);
  bool pulseHigh   =  debounceRead(PIN_PULSE,   pulseDebounce);

  static bool lastHookState = false;

  if (hookOffHook && !lastHookState) {
    DLOG("\n[Event] Phone OFF HOOK\n");
    currentState = STATE_DIAL_TONE;
    dialedNumber = ""; currentDigit = 0;
    lastActivityTime = millis();
    dialToneInitialized = false;
    offHookWarningInitialized = false;
    interceptRecordingPlayed = false;
    toneGen.setTone(350, 440);
  } else if (!hookOffHook && lastHookState) {
    DLOG("\n[Event] Phone ON HOOK - Resetting\n");
    currentState = STATE_ON_HOOK;
    dialedNumber = ""; currentDigit = 0;
    dialToneInitialized = false;
    offHookWarningInitialized = false;
    interceptRecordingPlayed = false;
    disconnectTonePlayed = false;
    toneGen.stop(); cadence.stop();
  }
  lastHookState = hookOffHook;

  if (hookOffHook && (currentState == STATE_DIAL_TONE || currentState == STATE_DIALING)) {
    static bool lastInUseState = false;
    if (inUseActive && !lastInUseState) {
      DLOG("[Event] Started dialing\n");
      if (currentState == STATE_DIAL_TONE) {
        currentState = STATE_DIALING;
        toneGen.stop(); dialToneInitialized = false;
      }
      currentDigit = 0; lastActivityTime = millis();
    }
    lastInUseState = inUseActive;

    static bool lastPulseState = false;
    if (inUseActive && pulseHigh && !lastPulseState) {
      currentDigit++;
      playClickSound();
      DLOG("  [Pulse] %d\n", currentDigit);
      lastActivityTime = millis();
    }
    lastPulseState = pulseHigh;

    static bool wasDialing = false;
    if (!inUseActive && wasDialing && currentDigit > 0) {
      int digit = (currentDigit == 10) ? 0 : currentDigit;
      dialedNumber += String(digit);
      DLOG("[Digit] %d  (so far: %s)\n", digit, dialedNumber.c_str());
      currentDigit = 0; lastActivityTime = millis();

      if (dialedNumber == "0") {
        processCompletedNumber();
      } else if (dialedNumber.length() >= DIGITS_IN_PHONE_NUM) {
        processCompletedNumber();
      }
    }
    wasDialing = inUseActive;
  }

  // Timeout from CALL_CONNECTED or ERROR → off-hook warning
  if (hookOffHook &&
      (currentState == STATE_CALL_CONNECTED || currentState == STATE_ERROR) &&
      millis() - lastActivityTime > IDLE_TIMEOUT_MS) {
    DLOG("[Timeout] Call ended, phone still off hook\n");
    currentState = STATE_OFF_HOOK_WARN;
    toneGen.stop(); cadence.stop();
    offHookWarningInitialized = false;
  }

  switch (currentState) {
    case STATE_ON_HOOK:        handleOnHook();         break;
    case STATE_DIAL_TONE:      handleDialTone();        break;
    case STATE_DIALING:        handleDialing();         break;
    case STATE_CALL_CONNECTED:
    case STATE_ERROR:          writeSample(0);          break;
    case STATE_DISCONNECT:     handleDisconnect();      break;
    case STATE_OFF_HOOK_WARN:  handleOffHookWarning();  break;
  }
}

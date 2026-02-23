/*
 * Bird Conversation - VAD Standalone POC
 * XIAO ESP32S3 with MAX9814 microphone + MAX98357A I2S amplifier
 *
 * Listens for speech via the MAX9814 ADC microphone, then responds
 * with a random sequence of bird squawks played through I2S audio.
 * Implements the same state machine as vad_prototype.py:
 *   SILENCE → SPEAKING → PAUSE → LOCKOUT → SILENCE
 *
 * Pin assignments chosen to avoid conflicts with rotary_phone sketch:
 *   rotary_phone uses: GPIO 1, 2, 3 (dial), GPIO 7, 8, 9 (I2S out)
 *
 * Hardware connections:
 *   MAX9814 OUT  → GPIO 4 (A4) - ADC microphone input
 *   MAX98357A BCLK → GPIO 7
 *   MAX98357A LRC  → GPIO 8
 *   MAX98357A DIN  → GPIO 9
 */

#include <driver/i2s.h>
#include <LittleFS.h>

// ============================================================================
// PIN DEFINITIONS
// ============================================================================
// MAX9814 microphone - analog output to ADC
// GPIO 4 is ADC-capable and free from rotary_phone sketch
#define PIN_MIC_ADC   4   // A4 - MAX9814 analog output

// I2S audio output - same hardware as rotary_phone, fine for standalone POC
#define PIN_I2S_BCLK  7   // D8
#define PIN_I2S_LRC   8   // D9
#define PIN_I2S_DOUT  9   // D10

// ============================================================================
// AUDIO CONFIGURATION
// ============================================================================
#define SAMPLE_RATE         16000   // Hz
#define CHUNK_DURATION_MS   30      // ms per VAD processing window
#define CHUNK_SIZE          (SAMPLE_RATE * CHUNK_DURATION_MS / 1000)  // samples

// ADC configuration
// Carbon mic via resistor divider sits at ~900/4095 at silence.
// We track DC bias with a very slow exponential moving average (tau ~10s)
// that self-corrects at startup without needing a silent calibration window.
// ADC_SCALE normalises the AC signal into roughly [-1.0, 1.0].
#define ADC_SCALE           2048.0f   // half of 12-bit range

// DC bias tracker - initialised to measured carbon mic idle point.
// Updated every chunk via a very slow EMA (see DC_BIAS_ALPHA).
// Alpha = 1 - exp(-chunk_duration / tau): for tau=10s, chunk=30ms -> ~0.003
#define DC_BIAS_ALPHA       0.003f
float adcMidpoint = 900.0f;  // Reasonable starting point for carbon mic

// Playback buffer size for WAV streaming
#define WAV_BUF_SIZE        512

// ============================================================================
// VAD TUNABLE CONSTANTS
// (Ported directly from vad_prototype.py)
// ============================================================================
// Calibrated from carbon handset mic measurements (12-bit ADC, ADC_SCALE=2048):
// Silence p-p ~600 counts  → AC-RMS ≈ 0.104 normalized
// Voice p-p  ~1680 counts  → AC-RMS ≈ 0.290 normalized
// Threshold sits midway at ~0.18
#define ENERGY_THRESHOLD          0.18f   // RMS amplitude - tune for environment
#define MIN_SPEECH_DURATION_MS    50      // Ignore sounds shorter than this (ms)
                                          // 50ms catches "hello"; rejects handset
                                          // pickup thumps which are single transients
#define PAUSE_DURATION_MS         800     // Silence before triggering response
#define POST_RESPONSE_LOCKOUT_MS  2000    // Don't listen while bird is talking
#define ENERGY_SMOOTHING_FACTOR   0.3f    // 0=no smoothing, 1=max smoothing

// Zero Crossing Rate (ZCR) - counts midpoint crossings per chunk.
// Speech: ~10-30 crossings per 30ms chunk (voiced consonants/vowels).
// Mechanical thump (handset pickup): 0-2 crossings - single slow impulse.
// Threshold set to require at least 4 crossings to count as speech-like.
// Combined with energy: BOTH must be true to transition SILENCE → SPEAKING.
#define ZCR_THRESHOLD             4       // Min crossings per chunk for speech

// ============================================================================
// BIRD RESPONSE CONFIGURATION
// ============================================================================
#define MIN_SQUAWKS_PER_RESPONSE  1
#define MAX_SQUAWKS_PER_RESPONSE  3
#define PAUSE_BETWEEN_SQUAWKS_MS  300
#define PAUSE_BEFORE_RESPONSE_MS  200
#define PAUSE_AFTER_RESPONSE_MS   400

// Squawk files stored in LittleFS under /audio/
const char* SQUAWK_FILES[] = {
  "/audio/squawk_1.wav",
  "/audio/squawk_2.wav",
  "/audio/squawk_3.wav",
  "/audio/squawk_4.wav",
};
const int NUM_SQUAWK_FILES = sizeof(SQUAWK_FILES) / sizeof(SQUAWK_FILES[0]);

// ============================================================================
// VAD STATE MACHINE
// ============================================================================
enum VADState {
  VAD_SILENCE,   // Waiting for speech to start
  VAD_SPEAKING,  // User is talking
  VAD_PAUSE,     // User stopped - waiting to see if they continue
  VAD_LOCKOUT    // Bird is responding, don't listen
};

VADState vadState = VAD_SILENCE;
unsigned long speechStartMs = 0;
unsigned long pauseStartMs  = 0;
unsigned long lockoutStartMs = 0;
float smoothedEnergy = 0.0f;
float smoothedZCR = 0.0f;
int responseCount = 0;

// ============================================================================
// I2S AUDIO OUTPUT
// ============================================================================
void initI2S() {
  i2s_config_t cfg = {
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

  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
}

void writeSilence(uint32_t durationMs) {
  uint32_t samples = (uint32_t)SAMPLE_RATE * durationMs / 1000;
  int16_t buf[2] = {0, 0};
  size_t written;
  for (uint32_t i = 0; i < samples; i++) {
    i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, portMAX_DELAY);
  }
}

// ============================================================================
// WAV PLAYBACK (LittleFS)
// ============================================================================
bool playWavFile(const char* path) {
  Serial.print("[Audio] playWavFile: ");
  Serial.println(path);
  if (!LittleFS.exists(path)) {
    Serial.print("[Audio] File not found: ");
    Serial.println(path);
    return false;
  }

  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.print("[Audio] Cannot open: ");
    Serial.println(path);
    return false;
  }

  if (f.size() < 44) {
    Serial.println("[Audio] File too small for WAV header");
    f.close();
    return false;
  }

  // Read and validate WAV header
  uint8_t header[44];
  if (f.read(header, 44) != 44) {
    Serial.println("[Audio] Failed to read WAV header");
    f.close();
    return false;
  }

  if (header[0] != 'R' || header[1] != 'I' || header[2] != 'F' || header[3] != 'F' ||
      header[8] != 'W' || header[9] != 'A' || header[10] != 'V' || header[11] != 'E') {
    Serial.println("[Audio] Invalid RIFF/WAVE header");
    f.close();
    return false;
  }

  uint16_t audioFormat   = header[20] | (header[21] << 8);
  uint16_t numChannels   = header[22] | (header[23] << 8);
  uint32_t fileSampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
  uint16_t bitsPerSample = header[34] | (header[35] << 8);

  if (audioFormat != 1) {
    Serial.println("[Audio] Only PCM WAV supported");
    f.close();
    return false;
  }

  // Find data chunk
  uint32_t dataSize = 0;
  bool dataFound = false;
  f.seek(36);
  uint8_t chunkHdr[8];
  while (f.available() >= 8) {
    f.read(chunkHdr, 8);
    if (chunkHdr[0] == 'd' && chunkHdr[1] == 'a' &&
        chunkHdr[2] == 't' && chunkHdr[3] == 'a') {
      dataSize = chunkHdr[4] | (chunkHdr[5] << 8) | (chunkHdr[6] << 16) | (chunkHdr[7] << 24);
      dataFound = true;
      break;
    }
    uint32_t skip = chunkHdr[4] | (chunkHdr[5] << 8) | (chunkHdr[6] << 16) | (chunkHdr[7] << 24);
    f.seek(f.position() + skip);
  }

  if (!dataFound) {
    Serial.println("[Audio] No data chunk in WAV");
    f.close();
    return false;
  }

  // Stream audio data through I2S
  uint8_t buf[WAV_BUF_SIZE];
  uint32_t bytesPerSample = (bitsPerSample / 8) * numChannels;
  uint32_t bytesRead = 0;
  float resampleAccum = 0.0f;
  float resampleStep = (float)fileSampleRate / (float)SAMPLE_RATE;

  while (bytesRead < dataSize && f.available()) {
    size_t toRead = min((uint32_t)WAV_BUF_SIZE, dataSize - bytesRead);
    toRead = (toRead / bytesPerSample) * bytesPerSample;
    if (toRead == 0) break;

    size_t actualRead = f.read(buf, toRead);
    bytesRead += actualRead;

    for (size_t i = 0; i + bytesPerSample <= actualRead; i += bytesPerSample) {
      int16_t sample = 0;
      if (bitsPerSample == 16) {
        sample = (int16_t)(buf[i] | (buf[i + 1] << 8));
      } else if (bitsPerSample == 8) {
        sample = ((int16_t)buf[i] - 128) << 8;
      }

      resampleAccum += 1.0f;
      while (resampleAccum >= resampleStep) {
        int16_t stereo[2] = {sample, sample};
        size_t written;
        i2s_write(I2S_NUM_0, stereo, sizeof(stereo), &written, portMAX_DELAY);
        resampleAccum -= resampleStep;
      }
    }
  }

  f.close();
  return true;
}

// ============================================================================
// BIRD RESPONSE
// ============================================================================
// Simple deterministic shuffle: Knuth/Fisher-Yates with a trivial LCG seed
uint32_t lcgState = 42;
uint32_t lcgRand() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState;
}

void playBirdResponse() {
  int numSquawks = MIN_SQUAWKS_PER_RESPONSE +
                   (int)(lcgRand() % (MAX_SQUAWKS_PER_RESPONSE - MIN_SQUAWKS_PER_RESPONSE + 1));

  Serial.print("[Bird] Playing ");
  Serial.print(numSquawks);
  Serial.println(" squawk(s)");

  writeSilence(PAUSE_BEFORE_RESPONSE_MS);

  for (int i = 0; i < numSquawks; i++) {
    int idx = (int)(lcgRand() % NUM_SQUAWK_FILES);
    Serial.print("[Bird]   squawk: ");
    Serial.println(SQUAWK_FILES[idx]);
    playWavFile(SQUAWK_FILES[idx]);

    if (i < numSquawks - 1) {
      writeSilence(PAUSE_BETWEEN_SQUAWKS_MS);
    }
  }

  writeSilence(PAUSE_AFTER_RESPONSE_MS);
}

// ============================================================================
// ADC CALIBRATION
// ============================================================================
// Warm up the ADC by discarding early readings, which are unreliable on
// ESP32S3 immediately after boot. No midpoint measurement needed - we use
// a rolling EMA in sampleMicEnergy() instead.
void warmupADC() {
  Serial.print("[Cal] Warming up ADC (discarding first 200 reads)... ");
  for (int i = 0; i < 200; i++) {
    analogRead(PIN_MIC_ADC);
    delayMicroseconds(100);
  }
  // Seed adcMidpoint from a short average of settled readings
  int32_t sum = 0;
  for (int i = 0; i < 100; i++) {
    sum += analogRead(PIN_MIC_ADC);
    delayMicroseconds(100);
  }
  adcMidpoint = (float)sum / 100.0f;
  Serial.print("done. Initial midpoint=");
  Serial.println(adcMidpoint, 1);
  Serial.println("[Cal] DC bias will continue tracking via slow EMA during operation.");
}

// ============================================================================
// VAD - FEATURE EXTRACTION (Energy + ZCR)
// ============================================================================
// Reads CHUNK_SIZE ADC samples, updates smoothedEnergy and smoothedZCR globals.
// Also tracks raw ADC stats for diagnostic printing.
int rawAdcMin = 4095, rawAdcMax = 0;

void sampleMicFeatures() {
  float sumSq = 0.0f;
  float chunkSum = 0.0f;
  int zeroCrossings = 0;
  bool prevAbove = false;  // Whether previous sample was above midpoint
  bool firstSample = true;

  for (int i = 0; i < CHUNK_SIZE; i++) {
    int raw = analogRead(PIN_MIC_ADC);
    if (raw < rawAdcMin) rawAdcMin = raw;
    if (raw > rawAdcMax) rawAdcMax = raw;
    chunkSum += raw;

    float norm = (raw - adcMidpoint) / ADC_SCALE;  // AC signal, roughly [-1.0, 1.0]
    sumSq += norm * norm;

    // Count midpoint crossings for ZCR
    bool above = (raw > adcMidpoint);
    if (!firstSample && above != prevAbove) {
      zeroCrossings++;
    }
    prevAbove = above;
    firstSample = false;
  }

  // Update DC bias tracker with chunk mean - very slow EMA so speech
  // doesn't pull it, only long-term drift is tracked.
  float chunkMean = chunkSum / (float)CHUNK_SIZE;
  adcMidpoint = (1.0f - DC_BIAS_ALPHA) * adcMidpoint + DC_BIAS_ALPHA * chunkMean;

  float rms = sqrtf(sumSq / (float)CHUNK_SIZE);

  // Smooth both features
  smoothedEnergy = ENERGY_SMOOTHING_FACTOR * smoothedEnergy +
                   (1.0f - ENERGY_SMOOTHING_FACTOR) * rms;
  smoothedZCR    = ENERGY_SMOOTHING_FACTOR * smoothedZCR +
                   (1.0f - ENERGY_SMOOTHING_FACTOR) * (float)zeroCrossings;
}

// ============================================================================
// VAD STATE MACHINE UPDATE
// ============================================================================
void updateVAD() {
  unsigned long now = millis();
  sampleMicFeatures();

  // Speech detected when BOTH energy AND ZCR exceed thresholds.
  // Energy alone triggers on thumps; ZCR alone triggers on hiss/noise.
  // Together they reliably indicate voiced speech.
  bool isSpeech = (smoothedEnergy > ENERGY_THRESHOLD) && (smoothedZCR >= ZCR_THRESHOLD);

  switch (vadState) {

    case VAD_LOCKOUT:
      if (now - lockoutStartMs >= POST_RESPONSE_LOCKOUT_MS) {
        Serial.println("[VAD] LOCKOUT → SILENCE");
        vadState = VAD_SILENCE;
        smoothedEnergy = 0.0f;
        smoothedZCR    = 0.0f;
      }
      break;

    case VAD_SILENCE:
      if (isSpeech) {
        speechStartMs = now;
        vadState = VAD_SPEAKING;
        Serial.print("[VAD] SILENCE → SPEAKING (energy: ");
        Serial.print(smoothedEnergy, 4);
        Serial.print(", ZCR: ");
        Serial.print(smoothedZCR, 1);
        Serial.println(")");
      }
      break;

    case VAD_SPEAKING:
      if (!isSpeech) {
        pauseStartMs = now;
        vadState = VAD_PAUSE;
        Serial.print("[VAD] SPEAKING → PAUSE (energy: ");
        Serial.print(smoothedEnergy, 4);
        Serial.print(", ZCR: ");
        Serial.print(smoothedZCR, 1);
        Serial.println(")");
      }
      break;

    case VAD_PAUSE:
      if (isSpeech) {
        // User resumed talking
        vadState = VAD_SPEAKING;
        Serial.println("[VAD] PAUSE → SPEAKING (resumed)");
      } else if (now - pauseStartMs >= PAUSE_DURATION_MS) {
        unsigned long speechDuration = pauseStartMs - speechStartMs;
        if (speechDuration >= MIN_SPEECH_DURATION_MS) {
          responseCount++;
          Serial.println();
          Serial.println("============================================================");
          Serial.print("[Bird] RESPONSE #");
          Serial.println(responseCount);
          Serial.print("[Bird] Speech duration: ");
          Serial.print(speechDuration);
          Serial.println("ms");
          Serial.println("============================================================");

          playBirdResponse();

          lockoutStartMs = millis();  // Use fresh timestamp after playback
          vadState = VAD_LOCKOUT;
          Serial.println("[VAD] PAUSE → LOCKOUT");
        } else {
          // Too short - ignore
          Serial.print("[VAD] PAUSE → SILENCE (speech too short: ");
          Serial.print(speechDuration);
          Serial.println("ms)");
          vadState = VAD_SILENCE;
        }
      }
      break;
  }
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("============================================================");
  Serial.println("Bird Conversation - VAD POC");
  Serial.println("XIAO ESP32S3 + MAX9814 mic + MAX98357A amp");
  Serial.println("============================================================");
  Serial.print("  Mic ADC pin:       GPIO ");
  Serial.println(PIN_MIC_ADC);
  Serial.print("  Sample rate:       ");
  Serial.print(SAMPLE_RATE);
  Serial.println(" Hz");
  Serial.print("  Chunk size:        ");
  Serial.print(CHUNK_SIZE);
  Serial.println(" samples");
  Serial.print("  Energy threshold:  ");
  Serial.println(ENERGY_THRESHOLD, 4);
  Serial.print("  Min speech:        ");
  Serial.print(MIN_SPEECH_DURATION_MS);
  Serial.println(" ms");
  Serial.print("  Pause trigger:     ");
  Serial.print(PAUSE_DURATION_MS);
  Serial.println(" ms");
  Serial.print("  Lockout:           ");
  Serial.print(POST_RESPONSE_LOCKOUT_MS);
  Serial.println(" ms");
  Serial.println("============================================================");

  // Configure ADC for MAX9814
  analogReadResolution(12);        // 12-bit: 0-4095
  analogSetAttenuation(ADC_11db);  // 0-3.3V range
  pinMode(PIN_MIC_ADC, INPUT);
  delay(100);                      // Let ADC settle before calibrating

  // Warm up ADC and seed DC bias tracker
  warmupADC();

  // Initialize I2S output
  initI2S();
  Serial.println("[Init] I2S initialized");

  // Mount LittleFS and list squawk files
  if (!LittleFS.begin(true)) {
    Serial.println("[Init] ERROR: LittleFS mount failed!");
  } else {
    Serial.println("[Init] LittleFS mounted");
    int found = 0;
    for (int i = 0; i < NUM_SQUAWK_FILES; i++) {
      if (LittleFS.exists(SQUAWK_FILES[i])) {
        Serial.print("[Init] Found: ");
        Serial.println(SQUAWK_FILES[i]);
        found++;
      } else {
        Serial.print("[Init] MISSING: ");
        Serial.println(SQUAWK_FILES[i]);
      }
    }
    if (found == 0) {
      Serial.println("[Init] WARNING: No squawk files found.");
      Serial.println("[Init]   Upload data/ via Tools → ESP32 LittleFS Data Upload");
    } else {
      Serial.print("[Init] ");
      Serial.print(found);
      Serial.print(" of ");
      Serial.print(NUM_SQUAWK_FILES);
      Serial.println(" squawk file(s) ready");
    }
  }

  // Seed LCG with a reading from an unconnected ADC pin for randomness
  lcgState = (uint32_t)analogRead(PIN_MIC_ADC) ^ (uint32_t)millis();

  Serial.println();
  Serial.println("[Ready] Listening... speak into the microphone!");
  Serial.println();
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  updateVAD();

  // Periodically print energy level and raw ADC range for tuning
  static unsigned long lastStatusMs = 0;
  if (millis() - lastStatusMs >= 500) {
    lastStatusMs = millis();
    const char* stateNames[] = {"SILENCE", "SPEAKING", "PAUSE", "LOCKOUT"};
    Serial.print("[Status] State: ");
    Serial.print(stateNames[vadState]);
    Serial.print(" | Energy: ");
    Serial.print(smoothedEnergy, 4);
    Serial.print(" | ZCR: ");
    Serial.print(smoothedZCR, 1);
    Serial.print(" | ADC range: ");
    Serial.print(rawAdcMin);
    Serial.print("-");
    Serial.print(rawAdcMax);
    Serial.print(" (midpoint: ");
    Serial.print(adcMidpoint, 0);
    Serial.println(")");
    // Reset min/max each interval so we see per-window range
    rawAdcMin = 4095;
    rawAdcMax = 0;
  }
}

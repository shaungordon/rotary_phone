/*
 * Rotary Phone Interface for XIAO ESP32S3
 * with Adafruit I2S MAX98357A Amplifier
 * 
 * Generates authentic US telephone tones from the 1970s
 * Supports special phone numbers with audio file playback
 */

#include <driver/i2s.h>
#include <LittleFS.h>

#define FILESYSTEM LittleFS

// ============================================================================
// PIN DEFINITIONS
// ============================================================================
#define PIN_IN_USE   1  // D1 - LOW when dialing
#define PIN_PULSE    2  // D2 - Pulses HIGH for each digit
#define PIN_HOOK     3  // D3 - LOW when receiver off hook

#define PIN_I2S_BCLK 7  // D8
#define PIN_I2S_LRC  8  // D9
#define PIN_I2S_DOUT 9  // D10

// ============================================================================
// CONSTANTS
// ============================================================================
#define SAMPLE_RATE         16000  // Lower quality = more authentic phone sound
#define DEBOUNCE_MS         20     // Debounce time for all pins
#define DIGITS_IN_PHONE_NUM 7      // 7-digit phone numbers
#define IDLE_TIMEOUT_MS     10000  // 10 seconds before off-hook warning
#define NUM_RINGS           2      // Number of rings before "answering"

// ============================================================================
// PHONE STATE MACHINE
// ============================================================================
enum PhoneState {
  STATE_ON_HOOK,        // Receiver on hook, idle
  STATE_DIAL_TONE,      // Receiver off hook, playing dial tone
  STATE_DIALING,        // User is dialing a number
  STATE_CALL_CONNECTED, // Playing audio file (ringing + answer)
  STATE_ERROR,          // Invalid number or can't connect
  STATE_OFF_HOOK_WARN   // Off-hook warning tone
};

PhoneState currentState = STATE_ON_HOOK;
bool offHookWarningInitialized = false;
bool dialToneInitialized = false;
bool interceptRecordingPlayed = false;  // Track if intercept recording has played

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

DebounceState hookDebounce = {HIGH, HIGH, 0};
DebounceState inUseDebounce = {HIGH, HIGH, 0};
DebounceState pulseDebounce = {LOW, LOW, 0};

// ============================================================================
// TONE GENERATION
// ============================================================================
class ToneGenerator {
private:
  float phases[4];      // Up to 4 simultaneous frequencies
  float frequencies[4];
  int numFrequencies;
  int16_t amplitude;
  
public:
  ToneGenerator() : numFrequencies(0), amplitude(8000) {
    for (int i = 0; i < 4; i++) {
      phases[i] = 0;
      frequencies[i] = 0;
    }
  }
  
  void setTone(float freq1, float freq2 = 0, float freq3 = 0, float freq4 = 0) {
    frequencies[0] = freq1;
    frequencies[1] = freq2;
    frequencies[2] = freq3;
    frequencies[3] = freq4;
    
    numFrequencies = 0;
    if (freq1 > 0) numFrequencies++;
    if (freq2 > 0) numFrequencies++;
    if (freq3 > 0) numFrequencies++;
    if (freq4 > 0) numFrequencies++;
    
    // Reset phases
    for (int i = 0; i < 4; i++) {
      phases[i] = 0;
    }
  }
  
  void setAmplitude(int16_t amp) {
    amplitude = amp;
  }
  
  int16_t getSample() {
    if (numFrequencies == 0) return 0;
    
    float mixed = 0;
    for (int i = 0; i < numFrequencies; i++) {
      mixed += sin(phases[i]);
      
      // Increment phase
      phases[i] += 2.0 * PI * frequencies[i] / SAMPLE_RATE;
      
      // Wrap phase
      if (phases[i] >= 2.0 * PI) {
        phases[i] -= 2.0 * PI;
      }
    }
    
    // Mix and scale
    return (int16_t)(mixed * amplitude / numFrequencies);
  }
  
  void stop() {
    numFrequencies = 0;
  }
};

ToneGenerator toneGen;

// ============================================================================
// CADENCED TONE (for pulsing tones like ringback and off-hook warning)
// ============================================================================
class CadencedTone {
private:
  unsigned long onDuration;
  unsigned long offDuration;
  unsigned long lastToggleTime;
  bool isOn;
  
public:
  CadencedTone() : onDuration(0), offDuration(0), lastToggleTime(0), isOn(false) {}
  
  void setCadence(unsigned long onMs, unsigned long offMs) {
    onDuration = onMs;
    offDuration = offMs;
    lastToggleTime = millis();
    isOn = true;
  }
  
  bool isActive() {
    unsigned long now = millis();
    unsigned long elapsed = now - lastToggleTime;
    
    if (isOn && elapsed >= onDuration) {
      isOn = false;
      lastToggleTime = now;
    } else if (!isOn && elapsed >= offDuration) {
      isOn = true;
      lastToggleTime = now;
    }
    
    return isOn;
  }
  
  void stop() {
    onDuration = 0;
    offDuration = 0;
    isOn = false;
  }
};

CadencedTone cadence;

// ============================================================================
// SPECIAL PHONE NUMBERS
// ============================================================================
struct PhoneNumber {
  String number;
  String audioFile;  // Path in LittleFS
};

// Define special numbers here
PhoneNumber specialNumbers[] = {
  {"5551234", "/audio/answer_osprey.wav"},
  {"3218273", "/audio/answer_magpie.wav"},
  {"9253162", "/audio/answer_cockatoo.wav"},
  // Add more special numbers as needed
};

const int numSpecialNumbers = sizeof(specialNumbers) / sizeof(specialNumbers[0]);

// ============================================================================
// I2S AUDIO OUTPUT
// ============================================================================
void initI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  
  i2s_pin_config_t pin_config = {
    .bck_io_num = PIN_I2S_BCLK,
    .ws_io_num = PIN_I2S_LRC,
    .data_out_num = PIN_I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
}

void writeSample(int16_t sample) {
  int16_t samples[2];
  samples[0] = sample;  // Left
  samples[1] = sample;  // Right
  
  size_t bytes_written;
  i2s_write(I2S_NUM_0, samples, sizeof(samples), &bytes_written, portMAX_DELAY);
}

// ============================================================================
// DEBOUNCE FUNCTIONS
// ============================================================================
bool debounceRead(int pin, DebounceState &state) {
  bool reading = digitalRead(pin);
  unsigned long now = millis();
  
  if (reading != state.lastState) {
    state.lastChangeTime = now;
  }
  
  if ((now - state.lastChangeTime) > DEBOUNCE_MS) {
    if (reading != state.currentState) {
      state.currentState = reading;
    }
  }
  
  state.lastState = reading;
  return state.currentState;
}

// ============================================================================
// AUDIO PLAYBACK
// ============================================================================

// Play a click sound for each pulse - synthesized, no file needed
void playClickSound() {
  toneGen.setAmplitude(3000);
  
  // Sharp attack - brief high frequency burst (simulates contact break)
  toneGen.setTone(2000, 3500);
  for (int i = 0; i < (SAMPLE_RATE * 2 / 1000); i++) {  // 2ms attack
    writeSample(toneGen.getSample());
  }
  
  // Quick decay to lower frequency
  toneGen.setTone(800);
  for (int i = 0; i < (SAMPLE_RATE * 3 / 1000); i++) {  // 3ms decay
    int16_t sample = toneGen.getSample();
    sample = sample * (1.0 - (float)i / (SAMPLE_RATE * 3 / 1000));
    writeSample(sample);
  }
  
  toneGen.stop();
}

// WAV file playback from LittleFS
// Returns true if playback completed, false if interrupted by on-hook
bool playWavFile(const char* path) {
  Serial.print("[Audio] Opening: ");
  Serial.println(path);
  
  if (!FILESYSTEM.exists(path)) {
    Serial.print("[Audio] ERROR: File does not exist: ");
    Serial.println(path);
    return true;
  }
  
  File file = FILESYSTEM.open(path, "r");
  if (!file) {
    Serial.print("[Audio] ERROR: Could not open file: ");
    Serial.println(path);
    return true;
  }
  
  Serial.print("[Audio] File size: ");
  Serial.print(file.size());
  Serial.println(" bytes");
  
  if (file.size() < 44) {
    Serial.println("[Audio] ERROR: File too small to be a valid WAV");
    file.close();
    return true;
  }
  
  // --- Parse WAV header ---
  uint8_t header[44];
  if (file.read(header, 44) != 44) {
    Serial.println("[Audio] ERROR: Could not read WAV header");
    file.close();
    return true;
  }
  
  // Validate RIFF and WAVE markers
  if (header[0] != 'R' || header[1] != 'I' || header[2] != 'F' || header[3] != 'F' ||
      header[8] != 'W' || header[9] != 'A' || header[10] != 'V' || header[11] != 'E') {
    Serial.print("[Audio] ERROR: Invalid WAV header. Got: ");
    Serial.print((char)header[0]); Serial.print((char)header[1]);
    Serial.print((char)header[2]); Serial.println((char)header[3]);
    file.close();
    return true;
  }
  
  // Extract format fields (little-endian)
  uint16_t audioFormat    = header[20] | (header[21] << 8);
  uint16_t numChannels    = header[22] | (header[23] << 8);
  uint32_t fileSampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
  uint16_t bitsPerSample  = header[34] | (header[35] << 8);
  
  Serial.print("[Audio] Format: ");
  Serial.print(fileSampleRate);
  Serial.print("Hz | ");
  Serial.print(numChannels);
  Serial.print("ch | ");
  Serial.print(bitsPerSample);
  Serial.print("bit | audioFormat=");
  Serial.println(audioFormat);
  
  if (audioFormat != 1) {
    Serial.println("[Audio] ERROR: Only uncompressed PCM WAV supported (audioFormat must be 1)");
    file.close();
    return true;
  }
  
  // Find the data chunk
  uint32_t dataSize = 0;
  bool dataFound = false;
  file.seek(36);
  uint8_t chunkHeader[8];
  while (file.available() >= 8) {
    file.read(chunkHeader, 8);
    if (chunkHeader[0] == 'd' && chunkHeader[1] == 'a' &&
        chunkHeader[2] == 't' && chunkHeader[3] == 'a') {
      dataSize = chunkHeader[4] | (chunkHeader[5] << 8) |
                 (chunkHeader[6] << 16) | (chunkHeader[7] << 24);
      dataFound = true;
      Serial.print("[Audio] Data chunk found, size: ");
      Serial.println(dataSize);
      break;
    } else {
      uint32_t chunkSize = chunkHeader[4] | (chunkHeader[5] << 8) |
                           (chunkHeader[6] << 16) | (chunkHeader[7] << 24);
      Serial.print("[Audio] Skipping chunk: ");
      Serial.print((char)chunkHeader[0]); Serial.print((char)chunkHeader[1]);
      Serial.print((char)chunkHeader[2]); Serial.print((char)chunkHeader[3]);
      Serial.print(" ("); Serial.print(chunkSize); Serial.println(" bytes)");
      file.seek(file.position() + chunkSize);
    }
  }
  
  if (!dataFound) {
    Serial.println("[Audio] ERROR: No data chunk found in WAV");
    file.close();
    return true;
  }
  
  // --- Stream audio data ---
  // Check hook pin directly (not debounced) every buffer so we can
  // interrupt immediately when receiver is replaced
  const size_t bufSize = 512;
  uint8_t buf[bufSize];
  uint32_t bytesPerSample = (bitsPerSample / 8) * numChannels;
  uint32_t bytesRead = 0;
  float resampleAccum = 0.0;
  float resampleStep = (float)fileSampleRate / (float)SAMPLE_RATE;
  bool interrupted = false;
  
  while (bytesRead < dataSize && file.available()) {
    // Check hook state at start of each buffer - HIGH = on hook
    if (digitalRead(PIN_HOOK) == HIGH) {
      Serial.println("[Audio] Interrupted: phone on hook");
      interrupted = true;
      break;
    }
    
    size_t toRead = min((uint32_t)bufSize, dataSize - bytesRead);
    toRead = (toRead / bytesPerSample) * bytesPerSample;
    if (toRead == 0) break;
    
    size_t actualRead = file.read(buf, toRead);
    bytesRead += actualRead;
    
    for (size_t i = 0; i + bytesPerSample <= actualRead; i += bytesPerSample) {
      int16_t sample = 0;
      if (bitsPerSample == 16) {
        sample = (int16_t)(buf[i] | (buf[i + 1] << 8));
      } else if (bitsPerSample == 8) {
        sample = ((int16_t)buf[i] - 128) << 8;
      }
      
      resampleAccum += 1.0;
      while (resampleAccum >= resampleStep) {
        writeSample(sample);
        resampleAccum -= resampleStep;
      }
    }
  }
  
  file.close();
  if (!interrupted) {
    Serial.print("[Audio] Done: ");
    Serial.println(path);
  }
  return !interrupted;
}

// Play the sound of someone picking up their receiver to answer
// Low thud + brief electrical pop, very short and subtle
void playAnswerClick() {
  // Layer 1: Low mechanical thud (the handset weight)
  // Short burst of low-frequency energy with fast decay
  toneGen.setAmplitude(2500);
  toneGen.setTone(60, 120);
  int thudSamples = SAMPLE_RATE * 18 / 1000;  // 18ms
  for (int i = 0; i < thudSamples; i++) {
    float envelope = 1.0 - ((float)i / thudSamples);  // Linear decay
    envelope = envelope * envelope;                     // Squared for faster drop-off
    writeSample((int16_t)(toneGen.getSample() * envelope));
  }
  
  // Layer 2: Brief electrical pop (circuit closing)
  // Very short, slightly higher frequency click
  toneGen.setTone(800, 1600);
  toneGen.setAmplitude(1500);
  int popSamples = SAMPLE_RATE * 6 / 1000;  // 6ms
  for (int i = 0; i < popSamples; i++) {
    float envelope = 1.0 - ((float)i / popSamples);
    writeSample((int16_t)(toneGen.getSample() * envelope));
  }
  
  // Noticeable pause before the voice starts (they just picked up, bring receiver to ear)
  int pauseSamples = SAMPLE_RATE * 600 / 1000;  // 600ms
  for (int i = 0; i < pauseSamples; i++) {
    writeSample(0);
  }
  
  toneGen.stop();
}
void playRingingAndAnswer(const char* audioFile) {
  Serial.println("[Audio] Playing ringing tone...");
  
  // US Ringback tone: 440Hz + 480Hz, 2 seconds on, 4 seconds off
  // On the last ring we skip the 4s silent gap so the answer
  // click follows immediately after the tone ends
  for (int ring = 0; ring < NUM_RINGS; ring++) {
    Serial.print("[Audio] Ring ");
    Serial.print(ring + 1);
    Serial.print(" of ");
    Serial.println(NUM_RINGS);
    
    bool isLastRing = (ring == NUM_RINGS - 1);
    bool interrupted = false;
    
    // --- 2 second tone portion ---
    toneGen.setTone(440, 480);
    toneGen.setAmplitude(8000);
    unsigned long toneStart = millis();
    while (millis() - toneStart < 2000) {
      if (digitalRead(PIN_HOOK) == HIGH) {
        Serial.println("[Audio] Ringing interrupted: phone on hook");
        interrupted = true;
        break;
      }
      writeSample(toneGen.getSample());
    }
    toneGen.stop();
    
    if (interrupted) return;
    
    // --- 4 second silent gap (skipped on last ring) ---
    if (!isLastRing) {
      unsigned long silenceStart = millis();
      while (millis() - silenceStart < 4000) {
        if (digitalRead(PIN_HOOK) == HIGH) {
          Serial.println("[Audio] Ringing interrupted: phone on hook");
          return;
        }
        writeSample(0);
      }
    }
  }
  
  // The click of the other person picking up their receiver
  if (digitalRead(PIN_HOOK) == LOW) {
    playAnswerClick();
  }
  
  // Play the answer recording (will also self-interrupt if hook goes high)
  playWavFile(audioFile);
}

// Play "cannot be connected" announcement
void playCannotConnect() {
  playWavFile("/audio/cannot_connect.wav");
}

// Play operator recording
void playOperator() {
  playWavFile("/audio/operator.wav");
}

// ============================================================================
// PHONE NUMBER PROCESSING
// ============================================================================
String lookupSpecialNumber(String number) {
  for (int i = 0; i < numSpecialNumbers; i++) {
    if (specialNumbers[i].number == number) {
      return specialNumbers[i].audioFile;
    }
  }
  return "";  // Not found
}

void processCompletedNumber() {
  Serial.print("[Call] Number dialed: ");
  Serial.println(dialedNumber);
  
  // Special case: Operator (single zero)
  if (dialedNumber == "0") {
    Serial.println("[Call] Connecting to operator");
    currentState = STATE_CALL_CONNECTED;
    playOperator();
    lastActivityTime = millis();
    return;
  }
  
  // Check if it's a special number (only if we have 7 digits)
  if (dialedNumber.length() == DIGITS_IN_PHONE_NUM) {
    String audioFile = lookupSpecialNumber(dialedNumber);
    
    if (audioFile != "") {
      Serial.print("[Call] Special number found: ");
      Serial.println(audioFile);
      currentState = STATE_CALL_CONNECTED;
      playRingingAndAnswer(audioFile.c_str());
      lastActivityTime = millis();
    } else {
      Serial.println("[Call] Number not recognized");
      currentState = STATE_ERROR;
      playCannotConnect();
      lastActivityTime = millis();
    }
  }
}

// ============================================================================
// STATE MACHINE HANDLERS
// ============================================================================
void handleOnHook() {
  // Just silence, waiting for phone to be picked up
  writeSample(0);
}

void handleDialTone() {
  // US Dial tone: 350Hz + 440Hz continuous
  
  if (!dialToneInitialized) {
    toneGen.setTone(350, 440);
    toneGen.setAmplitude(8000);
    dialToneInitialized = true;
  }
  
  writeSample(toneGen.getSample());
  
  // Check for timeout
  if (millis() - lastActivityTime > IDLE_TIMEOUT_MS) {
    Serial.println("[Timeout] No dialing activity, switching to off-hook warning");
    currentState = STATE_OFF_HOOK_WARN;
    toneGen.stop();
    dialToneInitialized = false;
  }
}

void handleDialing() {
  // Silent while dialing (clicks are played separately)
  writeSample(0);
  
  // Check for timeout during dialing (partial number)
  if (millis() - lastActivityTime > IDLE_TIMEOUT_MS) {
    Serial.println("[Timeout] Partial number dialed but no activity, switching to off-hook warning");
    currentState = STATE_OFF_HOOK_WARN;
    dialedNumber = "";  // Reset partial number
  }
}

// Play intercept recording for off-hook warning
void playInterceptRecording() {
  playWavFile("/audio/intercept_offhook.wav");
}

void handleOffHookWarning() {
  // US Off-hook warning (ROH): 1400, 2060, 2450, 2600 Hz
  // Cadence: 0.1s on, 0.1s off (very loud and annoying!)
  // Bell System (post-1975): Play intercept recording FIRST, then the howler tone
  
  if (!offHookWarningInitialized) {
    // First play the intercept recording
    if (!interceptRecordingPlayed) {
      playInterceptRecording();
      interceptRecordingPlayed = true;
    }
    
    // Then start the howler tone
    toneGen.setTone(1400, 2060, 2450, 2600);
    toneGen.setAmplitude(16000);  // LOUD!
    cadence.setCadence(100, 100);
    offHookWarningInitialized = true;
    Serial.println("[Audio] Off-hook warning tone started");
  }
  
  if (cadence.isActive()) {
    writeSample(toneGen.getSample());
  } else {
    writeSample(0);
  }
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=================================");
  Serial.println("Rotary Phone Interface");
  Serial.println("=================================\n");
  
  // Initialize pins
  pinMode(PIN_HOOK, INPUT_PULLUP);
  pinMode(PIN_IN_USE, INPUT_PULLUP);
  pinMode(PIN_PULSE, INPUT_PULLUP);
  
  // Initialize I2S audio
  initI2S();
  Serial.println("[Init] I2S audio initialized");
  
  // Initialize LittleFS for audio files
  if (!FILESYSTEM.begin(true)) {
    Serial.println("[Init] LittleFS mount failed!");
  } else {
    Serial.println("[Init] LittleFS mounted");
    Serial.print("[Init] Total bytes: ");
    Serial.println(FILESYSTEM.totalBytes());
    Serial.print("[Init] Used bytes:  ");
    Serial.println(FILESYSTEM.usedBytes());
    // List all files
    File root = FILESYSTEM.open("/");
    if (!root || !root.isDirectory()) {
      Serial.println("[Init] WARNING: Could not open root directory");
    } else {
      File file = root.openNextFile();
      int fileCount = 0;
      while (file) {
        fileCount++;
        Serial.print("[Init] Found: ");
        Serial.print(file.name());
        Serial.print("  (");
        Serial.print(file.size());
        Serial.println(" bytes)");
        file = root.openNextFile();
      }
      if (fileCount == 0) {
        Serial.println("[Init] WARNING: No files found - was LittleFS data uploaded?");
        Serial.println("[Init]   Arduino IDE: Tools -> ESP32 LittleFS Data Upload");
      } else {
        Serial.print("[Init] ");
        Serial.print(fileCount);
        Serial.println(" file(s) found");
      }
    }
  }
  
  Serial.println("[Ready] Waiting for phone activity...\n");
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  // Read debounced pin states
  bool hookOffHook = !debounceRead(PIN_HOOK, hookDebounce);     // LOW = off hook
  bool inUseActive = !debounceRead(PIN_IN_USE, inUseDebounce);  // LOW = dialing
  bool pulseHigh = debounceRead(PIN_PULSE, pulseDebounce);      // HIGH = pulse
  
  // ============================================================
  // Hook state change detection (highest priority)
  // ============================================================
  static bool lastHookState = false;
  
  if (hookOffHook && !lastHookState) {
    // Phone just picked up
    Serial.println("\n[Event] Phone OFF HOOK");
    currentState = STATE_DIAL_TONE;
    dialedNumber = "";
    currentDigit = 0;
    lastActivityTime = millis();
    dialToneInitialized = false;
    offHookWarningInitialized = false;
    interceptRecordingPlayed = false;
    toneGen.setTone(350, 440);  // Start dial tone
  } else if (!hookOffHook && lastHookState) {
    // Phone just hung up
    Serial.println("\n[Event] Phone ON HOOK - Resetting");
    currentState = STATE_ON_HOOK;
    dialedNumber = "";
    currentDigit = 0;
    dialToneInitialized = false;
    offHookWarningInitialized = false;
    interceptRecordingPlayed = false;
    toneGen.stop();
    cadence.stop();
  }
  
  lastHookState = hookOffHook;
  
  // ============================================================
  // Only process dialing if phone is off hook AND in dialable state
  // ============================================================
  if (hookOffHook && (currentState == STATE_DIAL_TONE || currentState == STATE_DIALING)) {
    // Detect start of dialing
    static bool lastInUseState = false;
    if (inUseActive && !lastInUseState) {
      Serial.println("[Event] Started dialing");
      if (currentState == STATE_DIAL_TONE) {
        currentState = STATE_DIALING;
        toneGen.stop();  // Stop dial tone
        dialToneInitialized = false;
      }
      currentDigit = 0;  // Reset pulse counter
      lastActivityTime = millis();
    }
    lastInUseState = inUseActive;
    
    // Count pulses while dialing
    static bool lastPulseState = false;
    if (inUseActive && pulseHigh && !lastPulseState) {
      currentDigit++;
      playClickSound();  // Audible feedback
      Serial.print("  [Pulse] ");
      Serial.println(currentDigit);
      lastActivityTime = millis();
    }
    lastPulseState = pulseHigh;
    
    // Detect end of digit dialing
    static bool wasDialing = false;
    if (!inUseActive && wasDialing && currentDigit > 0) {
      // Digit complete
      int digit = (currentDigit == 10) ? 0 : currentDigit;  // 10 pulses = 0
      dialedNumber += String(digit);
      Serial.print("[Digit] Dialed: ");
      Serial.print(digit);
      Serial.print(" (Number so far: ");
      Serial.print(dialedNumber);
      Serial.println(")");
      
      currentDigit = 0;
      lastActivityTime = millis();
      
      // Special case: Check for operator (single 0)
      if (dialedNumber == "0") {
        Serial.println("[Event] Operator number detected");
        processCompletedNumber();
      }
      // Check if complete 7-digit number
      else if (dialedNumber.length() >= DIGITS_IN_PHONE_NUM) {
        Serial.println("[Event] Complete phone number dialed");
        processCompletedNumber();
      }
    }
    wasDialing = inUseActive;    
    // Check timeout during connected call or error
    if ((currentState == STATE_CALL_CONNECTED || currentState == STATE_ERROR) &&
        millis() - lastActivityTime > IDLE_TIMEOUT_MS) {
      Serial.println("[Timeout] Call completed but phone still off hook");
      currentState = STATE_OFF_HOOK_WARN;
      toneGen.stop();
      cadence.stop();
      offHookWarningInitialized = false;
    }
  } else if (hookOffHook) {
    // Phone is off hook but in a non-dialable state (CALL_CONNECTED, ERROR, OFF_HOOK_WARN)
    // Check timeout during connected call or error
    if ((currentState == STATE_CALL_CONNECTED || currentState == STATE_ERROR) &&
        millis() - lastActivityTime > IDLE_TIMEOUT_MS) {
      Serial.println("[Timeout] Call/error completed but phone still off hook");
      currentState = STATE_OFF_HOOK_WARN;
      toneGen.stop();
      cadence.stop();
      offHookWarningInitialized = false;
    }
  }
  
  // ============================================================
  // Execute current state
  // ============================================================
  switch (currentState) {
    case STATE_ON_HOOK:
      handleOnHook();
      break;
      
    case STATE_DIAL_TONE:
      handleDialTone();
      break;
      
    case STATE_DIALING:
      handleDialing();
      break;
      
    case STATE_CALL_CONNECTED:
    case STATE_ERROR:
      // Audio already played, just output silence
      writeSample(0);
      break;
      
    case STATE_OFF_HOOK_WARN:
      handleOffHookWarning();
      break;
  }
}

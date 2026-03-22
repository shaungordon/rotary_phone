/*
 * Dial Tone Test - Simple Audio Playback Test
 * XIAO ESP32S3 with Adafruit I2S MAX98357A Amplifier
 *
 * This sketch does nothing except continuously play a dial tone.
 * No pin inputs are read - purely for testing audio output hardware.
 * 
 * Use this to isolate whether noise issues are hardware (bad connection)
 * or software (complex state machine interactions).
 */

#include <Arduino.h>
#include <driver/i2s.h>

// ============================================================================
// PIN DEFINITIONS
// ============================================================================
#define PIN_I2S_BCLK  7  // D8
#define PIN_I2S_LRC   8  // D9
#define PIN_I2S_DOUT  9  // D10

// ============================================================================
// CONSTANTS
// ============================================================================
#define SAMPLE_RATE   8000   // POTS standard - authentic 1970s phone sound
#define VOLUME        0.5f   // Master volume (0.0 - 1.0)

// ============================================================================
// TONE GENERATOR
// ============================================================================
class ToneGenerator {
private:
  float   phases[2];
  float   frequencies[2];
  int     numFrequencies;
  int16_t amplitude;

public:
  ToneGenerator() : numFrequencies(0), amplitude(8000) {
    phases[0] = 0;
    phases[1] = 0;
    frequencies[0] = 0;
    frequencies[1] = 0;
  }

  void setTone(float f1, float f2 = 0) {
    frequencies[0] = f1;
    frequencies[1] = f2;
    numFrequencies = (f1 > 0) + (f2 > 0);
    phases[0] = 0;
    phases[1] = 0;
  }

  void setAmplitude(int16_t amp) {
    amplitude = amp;
  }

  int16_t getSample() {
    if (numFrequencies == 0) return 0;
    
    float mixed = 0;
    for (int i = 0; i < numFrequencies; i++) {
      mixed += sin(phases[i]);
      phases[i] += 2.0f * PI * frequencies[i] / SAMPLE_RATE;
      if (phases[i] >= 2.0f * PI) {
        phases[i] -= 2.0f * PI;
      }
    }
    
    return (int16_t)(mixed * amplitude / numFrequencies);
  }
};

ToneGenerator toneGen;

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

// Write one stereo sample pair with volume scaling applied
void writeSample(int16_t sample) {
  int16_t scaled = (int16_t)(sample * VOLUME);
  int16_t stereo[2] = {scaled, scaled};
  size_t written;
  i2s_write(I2S_NUM_0, stereo, sizeof(stereo), &written, portMAX_DELAY);
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=================================");
  Serial.println("Dial Tone Test");
  Serial.println("Hardware Audio Test - No Inputs");
  Serial.println("=================================\n");
  
  initI2S();
  Serial.println("[Init] I2S initialized");
  
  // Set up US dial tone: 350Hz + 440Hz
  toneGen.setTone(350, 440);
  toneGen.setAmplitude(8000);
  
  Serial.println("[Ready] Playing continuous dial tone...");
  Serial.println("        Listen for any noise, hiss, or audio artifacts.");
  Serial.println("        This will help isolate hardware vs software issues.\n");
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  // Simply generate and output the dial tone continuously
  // No pin reads, no state machine, no interruptions
  writeSample(toneGen.getSample());
}

/*
 * Microphone ADC Diagnostic Sketch
 * XIAO ESP32S3
 *
 * Reads the mic input and reports:
 *   - Rolling average (DC bias / midpoint)
 *   - Min and max over the last window (signal swing)
 *   - Peak-to-peak amplitude
 *   - A simple ASCII bar showing relative signal level
 *
 * No VAD, no audio output, no LittleFS - bare` minimum.
 */

// ADC pin - same as bird_conversation sketch
#define PIN_MIC_ADC   4    // A4 - mic signal input

// How many samples to collect per report window
#define WINDOW_SIZE   1600  // ~100ms at 16kHz equivalent polling rate

void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12);        // 12-bit: 0-4095
  analogSetAttenuation(ADC_11db);  // Full 0-3.3V range
  pinMode(PIN_MIC_ADC, INPUT);

  Serial.println();
  Serial.println("==============================================");
  Serial.println("  Mic ADC Diagnostic");
  Serial.println("  ADC pin: GPIO 4");
  Serial.println("  Range:   0-4095 (12-bit, 0-3.3V)");
  Serial.println("==============================================");
  Serial.println("  avg    = DC bias / midpoint");
  Serial.println("  min/max = signal swing this window");
  Serial.println("  p-p     = peak to peak (louder = bigger)");
  Serial.println("  bar     = relative level (max 40 chars)");
  Serial.println("==============================================");
  Serial.println();
}

void loop() {
  int32_t sum = 0;
  int rawMin = 4095;
  int rawMax = 0;

  for (int i = 0; i < WINDOW_SIZE; i++) {
    int raw = analogRead(PIN_MIC_ADC);
    sum += raw;
    if (raw < rawMin) rawMin = raw;
    if (raw > rawMax) rawMax = raw;
  }

  int avg = sum / WINDOW_SIZE;
  int peakToPeak = rawMax - rawMin;

  // ASCII bar scaled to 40 chars max, where 4095 = full scale
  int barLen = map(peakToPeak, 0, 4095, 0, 40);
  char bar[41];
  memset(bar, '|', barLen);
  bar[barLen] = '\0';

  Serial.print("avg=");
  Serial.print(avg);
  Serial.print("  min=");
  Serial.print(rawMin);
  Serial.print("  max=");
  Serial.print(rawMax);
  Serial.print("  p-p=");
  Serial.print(peakToPeak);
  Serial.print("  [");
  Serial.print(bar);
  Serial.println("]");
}

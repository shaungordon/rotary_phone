/*
 * config.h — Runtime configuration with NVS persistence
 * Rotary Phone Interface / XIAO ESP32S3
 *
 * Replaces compile-time #defines for VOLUME and VAD parameters with
 * a Config struct that is loaded from ESP32 NVS (non-volatile storage)
 * at boot and saved back whenever a value changes.
 *
 * Usage:
 *   cfg.load();                        // call in setup()
 *   cfg.current.volume = 0.7f;         // change a value
 *   cfg.save();                        // persist to NVS
 *   float v = cfg.current.volume;      // use the value
 */

#pragma once
#include <Preferences.h>

// ============================================================================
// NVS NAMESPACE (max 15 chars)
// ============================================================================
#define NVS_NAMESPACE "rotary_cfg"

// ============================================================================
// CONFIG STRUCT
// ============================================================================
struct Config {
  // Audio
  float    volume;              // Master volume scalar (0.0 – 1.0)

  // VAD: energy & ZCR thresholds
  float    vadEnergyThreshold;  // RMS energy threshold for speech detection
  int      vadZcrThreshold;     // Minimum zero-crossings per chunk for speech

  // Bird conversation timing
  uint32_t birdMinSpeechMs;     // Minimum speech duration to trigger a response
  uint32_t birdIdleTimeoutMs;   // Idle time before bird hangs up
};

// ============================================================================
// DEFAULTS — mirror of the original #define values
// ============================================================================
static const Config CONFIG_DEFAULTS = {
  /* volume             */ 0.5f,
  /* vadEnergyThreshold */ 0.18f,
  /* vadZcrThreshold    */ 4,
  /* birdMinSpeechMs    */ 50,
  /* birdIdleTimeoutMs  */ 8000,
};

// ============================================================================
// CONFIG MANAGER
// ============================================================================
class ConfigManager {
public:
  Config current;

  // Load from NVS; falls back to defaults for any key not yet stored.
  void load() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) {
      // NVS namespace not found yet — use defaults
      current = CONFIG_DEFAULTS;
      Serial.println("[Config] NVS namespace not found, using defaults");
      return;
    }
    current.volume             = prefs.getFloat("volume",      CONFIG_DEFAULTS.volume);
    current.vadEnergyThreshold = prefs.getFloat("vadEnergy",   CONFIG_DEFAULTS.vadEnergyThreshold);
    current.vadZcrThreshold    = prefs.getInt  ("vadZcr",      CONFIG_DEFAULTS.vadZcrThreshold);
    current.birdMinSpeechMs    = prefs.getUInt ("minSpeech",   CONFIG_DEFAULTS.birdMinSpeechMs);
    current.birdIdleTimeoutMs  = prefs.getUInt ("idleTimeout", CONFIG_DEFAULTS.birdIdleTimeoutMs);
    prefs.end();
  }

  // Save current values to NVS.
  void save() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) {
      Serial.println("[Config] ERROR: Could not open NVS for writing");
      return;
    }
    prefs.putFloat("volume",      current.volume);
    prefs.putFloat("vadEnergy",   current.vadEnergyThreshold);
    prefs.putInt  ("vadZcr",      current.vadZcrThreshold);
    prefs.putUInt ("minSpeech",   current.birdMinSpeechMs);
    prefs.putUInt ("idleTimeout", current.birdIdleTimeoutMs);
    prefs.end();
  }

  // Reset everything to defaults and persist.
  void resetToDefaults() {
    current = CONFIG_DEFAULTS;
    save();
    Serial.println("[Config] All defaults restored");
  }

  // Print all values to Serial (useful for BLE status command).
  void printAll(Print& out) const {
    out.printf("[Config] volume          = %.2f\n",  current.volume);
    out.printf("[Config] vadEnergyThresh = %.3f\n",  current.vadEnergyThreshold);
    out.printf("[Config] vadZcrThresh    = %d\n",    current.vadZcrThreshold);
    out.printf("[Config] birdMinSpeech   = %u ms\n", current.birdMinSpeechMs);
    out.printf("[Config] birdIdleTimeout = %u ms\n", current.birdIdleTimeoutMs);
  }
};

// Global instance — defined in rotary_phone.ino
extern ConfigManager cfg;

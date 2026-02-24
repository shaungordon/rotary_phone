/*
 * ble_debug.h — BLE UART debug/config interface
 * Rotary Phone Interface / XIAO ESP32S3
 *
 * Implements Nordic UART Service (NUS) over NimBLE so you can monitor
 * debug output and adjust config parameters from a phone terminal app
 * without USB access (useful once the enclosure is closed).
 *
 * Compatible apps (free):
 *   iOS:     Bluefruit Connect by Adafruit  →  UART tab
 *   Android: Serial Bluetooth Terminal by Kai Morich
 *
 * BLE device name:  "RotaryPhone"  (appears in app scan)
 *
 * Commands (type in the terminal app, one per line):
 *   status              — print all current config values
 *   get volume          — print current value
 *   set volume 0.7      — update, save to NVS, echo confirmation
 *   set vadEnergy 0.12
 *   set vadZcr 5
 *   set minSpeech 100
 *   set idleTimeout 6000
 *   reset               — restore all defaults and save
 *
 * Usage in sketch:
 *   #include "ble_debug.h"
 *   BleDebug bleDebug;
 *   // in setup():  bleDebug.begin("RotaryPhone");
 *   // in loop():   bleDebug.loop();
 *   // logging:     DLOG("value=%d\n", x);   (see macro in rotary_phone.ino)
 *
 * Requires:  NimBLE-Arduino library  (h2zero, v1.4+)
 *            Install via Arduino Library Manager or platformio.ini
 */

#pragma once
#include <NimBLEDevice.h>
#include <stdarg.h>
#include "esp_system.h"
#include "config.h"

// ============================================================================
// NORDIC UART SERVICE UUIDS
// ============================================================================
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // ESP32 → phone
#define NUS_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // phone → ESP32

// ============================================================================
// RING BUFFER CONFIG
// ============================================================================
#define BLE_LOG_LINES    40    // Number of lines stored for replay on reconnect
#define BLE_LOG_LINE_LEN 120   // Max chars per line (truncated if longer)

// ============================================================================
// BleDebug CLASS
// ============================================================================
class BleDebug : public NimBLEServerCallbacks,
                 public NimBLECharacteristicCallbacks {
public:
  // ------------------------------------------------------------------
  // PUBLIC API
  // ------------------------------------------------------------------

  // Call once in setup(). Starts advertising as "RotaryPhone-XXXX" where
  // XXXX is the last 4 hex digits of the BT MAC address, ensuring each
  // unit has a unique, identifiable name.
  void begin(const char* deviceName = "RotaryPhone") {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char uniqueName[24];
    snprintf(uniqueName, sizeof(uniqueName), "%s-%02X%02X", deviceName, mac[4], mac[5]);
    
    NimBLEDevice::init(uniqueName);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    _server = NimBLEDevice::createServer();
    _server->setCallbacks(this);

    // Create NUS service
    NimBLEService* nus = _server->createService(NUS_SERVICE_UUID);

    // TX characteristic: ESP32 notifies phone (read + notify)
    _txChar = nus->createCharacteristic(
      NUS_TX_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // RX characteristic: phone writes to ESP32 (write + write-no-response)
    _rxChar = nus->createCharacteristic(
      NUS_RX_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    _rxChar->setCallbacks(this);

    nus->start();

    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();

    // Best strategy for macOS: Name and Flags in Primary, Service UUID in Scan Response.
    NimBLEAdvertisementData advData;
    advData.setFlags(0x06); // General Discoverable | BR/EDR Not Supported
    advData.setAppearance(0x0040); // Generic Phone
    advData.setName(uniqueName);
    pAdvertising->setAdvertisementData(advData);

    NimBLEAdvertisementData scanResponseData;
    scanResponseData.addServiceUUID(NUS_SERVICE_UUID);
    pAdvertising->setScanResponseData(scanResponseData);

    // Reliable Advertising (100ms)
    pAdvertising->setMinInterval(160); 
    pAdvertising->setMaxInterval(320);

    pAdvertising->start();

    Serial.printf("[BLE] Advertising as \"%s\"\n", uniqueName);
  }

  // Call at the top of loop() — processes any pending received command.
  // Returns immediately (<1 µs) when no command is waiting.
  void loop() {
    if (_pendingCmd[0] != '\0') {
      _handleCommand(_pendingCmd);
      _pendingCmd[0] = '\0';
    }
  }

  // Printf-style logging. Writes to both Serial (handled by DLOG macro)
  // and BLE TX (if connected). Also stores in ring buffer.
  // Non-blocking: if BLE TX is busy the line is stored and will be seen
  // the next time a client connects (ring buffer replay).
  void printf(const char* fmt, ...) {
    char line[BLE_LOG_LINE_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    _storeInRingBuf(line);

    if (_connected && _txChar) {
      _sendRaw(line);
    }
  }

  bool isConnected() const { return _connected; }

  // ------------------------------------------------------------------
  // NimBLE CALLBACKS (must be public for inheritance)
  // ------------------------------------------------------------------
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    _connected = true;
    Serial.println("[BLE] Client connected");

    // Optimize connection parameters for low latency and stability.
    // 15ms min, 30ms max, 0 latency, 4s timeout (400 * 10ms)
    pServer->updateConnParams(connInfo.getConnHandle(), 12, 24, 0, 400);

    _dumpHistory();
    _sendLine("[BLE] RotaryPhone connected. Type 'status' for config.\n");
  }

  void onDisconnect(NimBLEServer* /*s*/, NimBLEConnInfo& /*connInfo*/, int /*reason*/) override {
    _connected = false;
    Serial.println("[BLE] Client disconnected");
    NimBLEDevice::startAdvertising();  // Resume advertising
  }

  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*connInfo*/) override {
    std::string val = c->getValue();
    if (val.empty()) return;

    // Copy into pending command buffer (trim trailing whitespace/newline)
    size_t len = val.size();
    if (len >= sizeof(_pendingCmd)) len = sizeof(_pendingCmd) - 1;
    memcpy(_pendingCmd, val.data(), len);
    _pendingCmd[len] = '\0';
    // Trim trailing \r\n
    for (int i = (int)len - 1; i >= 0; i--) {
      if (_pendingCmd[i] == '\r' || _pendingCmd[i] == '\n')
        _pendingCmd[i] = '\0';
      else break;
    }
  }

private:
  // ------------------------------------------------------------------
  // PRIVATE MEMBERS
  // ------------------------------------------------------------------
  NimBLEServer*         _server   = nullptr;
  NimBLECharacteristic* _txChar   = nullptr;
  NimBLECharacteristic* _rxChar   = nullptr;
  bool                  _connected = false;

  // Ring buffer — stores recent log lines for replay on reconnect
  char    _logBuf[BLE_LOG_LINES][BLE_LOG_LINE_LEN];
  uint8_t _logHead  = 0;  // Index of oldest line (if full)
  uint8_t _logCount = 0;  // Number of lines stored (0 – BLE_LOG_LINES)

  // Pending command received from BLE client (processed in loop())
  char _pendingCmd[BLE_LOG_LINE_LEN] = {'\0'};

  // ------------------------------------------------------------------
  // PRIVATE HELPERS
  // ------------------------------------------------------------------

  // Store a line in the circular ring buffer (overwrites oldest when full).
  void _storeInRingBuf(const char* line) {
    uint8_t idx = (_logHead + _logCount) % BLE_LOG_LINES;
    strncpy(_logBuf[idx], line, BLE_LOG_LINE_LEN - 1);
    _logBuf[idx][BLE_LOG_LINE_LEN - 1] = '\0';
    if (_logCount < BLE_LOG_LINES) {
      _logCount++;
    } else {
      _logHead = (_logHead + 1) % BLE_LOG_LINES;
    }
  }

  // Replay the ring buffer to a newly-connected client.
  void _dumpHistory() {
    if (_logCount == 0) return;
    _sendRaw("--- log history ---\n");
    for (uint8_t i = 0; i < _logCount; i++) {
      uint8_t idx = (_logHead + i) % BLE_LOG_LINES;
      _sendRaw(_logBuf[idx]);
    }
    _sendRaw("--- end history ---\n");
  }

  // Send a string over BLE notify. Chunks into ≤200 bytes to stay safe
  // across all iOS/Android BLE stacks.
  void _sendRaw(const char* text) {
    if (!_connected || !_txChar) return;
    size_t len = strlen(text);
    size_t offset = 0;
    while (offset < len) {
      size_t chunk = len - offset;
      if (chunk > 200) chunk = 200;
      _txChar->setValue((const uint8_t*)(text + offset), chunk);
      _txChar->notify();
      offset += chunk;
      if (offset < len) delay(10);  // Brief yield between chunks
    }
  }

  void _sendLine(const char* line) {
    _sendRaw(line);
  }

  // ------------------------------------------------------------------
  // COMMAND PARSER
  // ------------------------------------------------------------------
  // Commands:
  //   status
  //   get <key>
  //   set <key> <value>
  //   reset
  //
  // Keys: volume | vadEnergy | vadZcr | minSpeech | idleTimeout
  // ------------------------------------------------------------------
  void _handleCommand(const char* cmd) {
    Serial.printf("[BLE] cmd: %s\n", cmd);

    // Work on a mutable copy
    char buf[BLE_LOG_LINE_LEN];
    strncpy(buf, cmd, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* verb = strtok(buf, " ");
    if (!verb) return;

    // ---- status ----
    if (strcmp(verb, "status") == 0) {
      _sendStatus();
      return;
    }

    // ---- reset ----
    if (strcmp(verb, "reset") == 0) {
      cfg.resetToDefaults();
      _sendLine("[Config] All defaults restored\n");
      _sendStatus();
      return;
    }

    char* key = strtok(nullptr, " ");
    if (!key) {
      _sendLine("[BLE] Usage: get <key> | set <key> <val> | status | reset\n");
      return;
    }

    // ---- get <key> ----
    if (strcmp(verb, "get") == 0) {
      _printKey(key);
      return;
    }

    // ---- set <key> <value> ----
    if (strcmp(verb, "set") == 0) {
      char* valStr = strtok(nullptr, " ");
      if (!valStr) {
        _sendLine("[BLE] set requires a value\n");
        return;
      }
      _setKey(key, valStr);
      return;
    }

    _sendLine("[BLE] Unknown command. Try: status | get <key> | set <key> <val> | reset\n");
  }

  void _printKey(const char* key) {
    char out[BLE_LOG_LINE_LEN];
    if (strcmp(key, "volume") == 0)
      snprintf(out, sizeof(out), "[Config] volume          = %.2f\n", cfg.current.volume);
    else if (strcmp(key, "vadEnergy") == 0)
      snprintf(out, sizeof(out), "[Config] vadEnergyThresh = %.3f\n", cfg.current.vadEnergyThreshold);
    else if (strcmp(key, "vadZcr") == 0)
      snprintf(out, sizeof(out), "[Config] vadZcrThresh    = %d\n", cfg.current.vadZcrThreshold);
    else if (strcmp(key, "minSpeech") == 0)
      snprintf(out, sizeof(out), "[Config] birdMinSpeech   = %u ms\n", cfg.current.birdMinSpeechMs);
    else if (strcmp(key, "idleTimeout") == 0)
      snprintf(out, sizeof(out), "[Config] birdIdleTimeout = %u ms\n", cfg.current.birdIdleTimeoutMs);
    else
      snprintf(out, sizeof(out), "[BLE] Unknown key: %s\n", key);
    _sendLine(out);
  }

  void _setKey(const char* key, const char* valStr) {
    char out[BLE_LOG_LINE_LEN];
    bool ok = false;

    if (strcmp(key, "volume") == 0) {
      float v = atof(valStr);
      if (v < 0.0f || v > 1.0f) {
        _sendLine("[BLE] volume must be 0.0 – 1.0\n"); return;
      }
      cfg.current.volume = v;
      snprintf(out, sizeof(out), "[Config] volume = %.2f (saved)\n", v);
      ok = true;
    } else if (strcmp(key, "vadEnergy") == 0) {
      float v = atof(valStr);
      if (v <= 0.0f) { _sendLine("[BLE] vadEnergy must be > 0\n"); return; }
      cfg.current.vadEnergyThreshold = v;
      snprintf(out, sizeof(out), "[Config] vadEnergyThresh = %.3f (saved)\n", v);
      ok = true;
    } else if (strcmp(key, "vadZcr") == 0) {
      int v = atoi(valStr);
      if (v < 0) { _sendLine("[BLE] vadZcr must be >= 0\n"); return; }
      cfg.current.vadZcrThreshold = v;
      snprintf(out, sizeof(out), "[Config] vadZcrThresh = %d (saved)\n", v);
      ok = true;
    } else if (strcmp(key, "minSpeech") == 0) {
      long v = atol(valStr);
      if (v < 0) { _sendLine("[BLE] minSpeech must be >= 0\n"); return; }
      cfg.current.birdMinSpeechMs = (uint32_t)v;
      snprintf(out, sizeof(out), "[Config] birdMinSpeech = %u ms (saved)\n", (uint32_t)v);
      ok = true;
    } else if (strcmp(key, "idleTimeout") == 0) {
      long v = atol(valStr);
      if (v <= 0) { _sendLine("[BLE] idleTimeout must be > 0\n"); return; }
      cfg.current.birdIdleTimeoutMs = (uint32_t)v;
      snprintf(out, sizeof(out), "[Config] birdIdleTimeout = %u ms (saved)\n", (uint32_t)v);
      ok = true;
    } else {
      snprintf(out, sizeof(out), "[BLE] Unknown key: %s\n", key);
    }

    if (ok) cfg.save();
    _sendLine(out);
  }

  void _sendStatus() {
    char out[BLE_LOG_LINE_LEN * 6];
    snprintf(out, sizeof(out),
      "[Config] --- Current Config ---\n"
      "[Config] volume          = %.2f\n"
      "[Config] vadEnergyThresh = %.3f\n"
      "[Config] vadZcrThresh    = %d\n"
      "[Config] birdMinSpeech   = %u ms\n"
      "[Config] birdIdleTimeout = %u ms\n"
      "[Config] ----------------------\n",
      cfg.current.volume,
      cfg.current.vadEnergyThreshold,
      cfg.current.vadZcrThreshold,
      cfg.current.birdMinSpeechMs,
      cfg.current.birdIdleTimeoutMs
    );
    _sendRaw(out);
  }
};

// Global instance — defined in rotary_phone.ino
extern BleDebug bleDebug;

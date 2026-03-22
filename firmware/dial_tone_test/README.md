# Dial Tone Test

A minimal audio playback test sketch for debugging hardware noise issues.

## Purpose

This sketch plays a continuous US dial tone (350Hz + 440Hz) without reading any pin inputs. It's designed to help isolate whether noise issues are caused by:

- **Hardware problems**: Bad connections, loose wires, power supply noise, grounding issues
- **Software problems**: Complex state machine interactions, timing issues, buffer management

## What It Does

- Initializes I2S audio output on the XIAO ESP32S3
- Generates a clean dial tone using sine wave synthesis
- Continuously outputs the tone at 8kHz sample rate (POTS standard)
- **No pin inputs** - no hook detection, no dialing, no state machine

## Hardware Setup

Same as the main rotary phone project:

- **XIAO ESP32S3** connected to **Adafruit I2S MAX98357A Amplifier**
- Pin connections:
  - D8 (GPIO 7) → BCLK
  - D9 (GPIO 8) → LRC (LRCLK)
  - D10 (GPIO 9) → DIN (DOUT)

## Building and Uploading

```bash
cd firmware/dial_tone_test
pio run -t upload -t monitor
```

Or use the PlatformIO IDE extension in VS Code.

## What to Listen For

With this simple test running, listen carefully for:

- **Clean tone**: Should be a steady, pure dial tone
- **Hiss or static**: Indicates hardware noise (check connections, power supply)
- **Pops or clicks**: May indicate loose connections or grounding issues
- **Pulsing or modulation**: Could be power supply ripple or interference

If the tone is clean here but noisy in the main sketch, the issue is likely software-related (buffer management, timing, state transitions).

If the tone is noisy here, the issue is hardware-related (check all connections, try different power supply, verify ground connections).

## Volume Adjustment

The volume is set to 0.5 (50%) in the code. You can adjust this by changing the `VOLUME` constant in `dial_tone_test.cpp`:

```cpp
#define VOLUME        0.5f   // Master volume (0.0 - 1.0)
```

## Next Steps

- If clean: Issue is in the main firmware - check state machine, buffer management
- If noisy: Issue is hardware - check connections, power supply, grounding

# Voice Activity Detection (VAD) Prototype

A Python prototype for voice activity detection designed for a rotary phone bird conversation simulator. This script establishes and tunes the VAD algorithm before porting to ESP32S3.

## Project Context

This is a prototype for a rotary phone prop using a XIAO ESP32S3. When a special number is dialed, it plays a bird character's voice. The VAD system makes the conversation feel natural by detecting when the user pauses, then triggering the bird's response.

## Features

- **Energy-based VAD**: Lightweight RMS energy detection suitable for ESP32S3
- **State machine**: SILENCE → SPEAKING → PAUSE → LOCKOUT → SILENCE
- **Tunable constants**: All parameters clearly defined for easy porting to Arduino C++
- **Real-time feedback**: Console output shows state transitions and energy levels
- **Bird squawk playback**: Plays 1-3 random squawks with natural pauses when user stops talking
- **Conversation simulation**: Pauses before/after and between squawks mimic natural speech patterns

## Installation

### Prerequisites

- Python 3.7 or higher
- Working microphone (built-in Mac mic works great)

### Setup

1. Clone or download this repository
2. Install dependencies:

```bash
pip install -r requirements.txt
```

Or install manually:

```bash
pip install sounddevice numpy soundfile
```

### Audio Files

The prototype expects four squawk WAV files in the same directory:
- `squawk_1.wav`
- `squawk_2.wav`
- `squawk_3.wav`
- `squawk_4.wav`

When a user pauses, the bird will randomly play 1-3 squawks with natural pauses between them, simulating a conversational response.

## Usage

### Run the VAD prototype:

```bash
python vad_prototype.py
```

### What to expect:

1. The script will start listening through your microphone
2. Speak into the mic - you'll see state transitions in the console
3. When you pause for ~800ms, it triggers a "response"
4. The system enters lockout for 2 seconds (simulating bird talking)
5. Then it returns to listening mode
6. Press `Ctrl+C` to stop and see a summary

### Example output:

```
============================================================
Voice Activity Detection - Rotary Phone Prototype
============================================================
Sample Rate: 16000 Hz
Chunk Duration: 30 ms
Energy Threshold: 0.02
Min Speech Duration: 200 ms
Pause Duration: 800 ms
Post-Response Lockout: 2000 ms
============================================================

Listening... Speak into your microphone!

[  1234567ms] SILENCE → SPEAKING (energy: 0.0234)
[  1235890ms] SPEAKING → PAUSE (energy: 0.0156)

============================================================
🐦 RESPONSE TRIGGERED #1
   Time: 1236690ms
   Speech Duration: 1123ms
   [In real system: Play bird WAV file here]
============================================================

[  1238690ms] LOCKOUT → SILENCE (ready to listen again)
```

## Tunable Constants

All constants are defined at the top of `vad_prototype.py` for easy adjustment:

| Constant | Default | Description |
|----------|---------|-------------|
| `SAMPLE_RATE` | 16000 Hz | Audio sample rate (ESP32S3 friendly) |
| `CHUNK_DURATION_MS` | 30 ms | Processing window size |
| `ENERGY_THRESHOLD` | 0.02 | RMS threshold for speech detection |
| `MIN_SPEECH_DURATION_MS` | 200 ms | Ignore sounds shorter than this |
| `PAUSE_DURATION_MS` | 800 ms | Silence duration before response |
| `POST_RESPONSE_LOCKOUT_MS` | 2000 ms | Don't listen during bird response |
| `ENERGY_SMOOTHING_FACTOR` | 0.3 | Smoothing to reduce jitter (0-1) |

### Tuning Tips

**If the system is too sensitive** (triggers on background noise):
- Increase `ENERGY_THRESHOLD` (try 0.03 or 0.04)
- Increase `MIN_SPEECH_DURATION_MS` (try 300 or 400)

**If the system is not sensitive enough** (doesn't detect speech):
- Decrease `ENERGY_THRESHOLD` (try 0.01 or 0.015)
- Check your microphone input level

**If responses come too quickly**:
- Increase `PAUSE_DURATION_MS` (try 1000 or 1200)

**If responses feel delayed**:
- Decrease `PAUSE_DURATION_MS` (try 600 or 700)

**If you get false triggers from quick sounds**:
- Increase `MIN_SPEECH_DURATION_MS`
- Increase `ENERGY_SMOOTHING_FACTOR` (try 0.4 or 0.5)

## Porting to ESP32S3

When you're ready to port to Arduino C++, the constants are formatted for easy conversion:

```cpp
// Audio Configuration
#define SAMPLE_RATE 16000
#define CHUNK_DURATION_MS 30
#define CHUNK_SIZE (SAMPLE_RATE * CHUNK_DURATION_MS / 1000)

// Energy Threshold
#define ENERGY_THRESHOLD 0.02f

// Timing Constants
#define MIN_SPEECH_DURATION_MS 200
#define PAUSE_DURATION_MS 800
#define POST_RESPONSE_LOCKOUT_MS 2000

// Smoothing
#define ENERGY_SMOOTHING_FACTOR 0.3f
```

### ESP32S3 Implementation Notes

- Use ADC for microphone input (12-bit, 3.3V reference)
- MAX9814 preamp recommended for vintage handset mic
- Sample at 8-16kHz to match phone aesthetic
- Calculate RMS energy per chunk: `sqrt(sum(samples^2) / count)`
- Implement same state machine: SILENCE → SPEAKING → PAUSE → LOCKOUT
- Use `millis()` for timing instead of `time.time()`

## Troubleshooting

### "No module named 'sounddevice'"
```bash
pip install sounddevice numpy
```

### "Error opening audio stream"
- Check that your microphone is connected and accessible
- List available audio devices: `python -m sounddevice`
- Grant microphone permissions in System Preferences (macOS)

### No response triggers
- Speak louder or closer to the microphone
- Lower `ENERGY_THRESHOLD` in the script
- Check the real-time energy display to see current levels

### Too many false triggers
- Increase `ENERGY_THRESHOLD`
- Increase `MIN_SPEECH_DURATION_MS`
- Move to a quieter environment

## Hardware Target

- **Microcontroller**: XIAO ESP32S3
- **Microphone**: Vintage handset element (carbon/dynamic)
- **Preamp**: MAX9814 with auto gain control
- **ADC**: ESP32S3 built-in 12-bit ADC
- **Sample Rate**: 8-16kHz (phone quality)

## License

This is a prototype/example project. Use freely for your own projects!

## Next Steps

1. Run the prototype and tune constants for your environment
2. Test with different speaking styles and volumes
3. Note the final constant values that work best
4. Port the algorithm to ESP32S3 Arduino code
5. Integrate with rotary dial detection and WAV playback

---

**Questions or issues?** This is a prototype - experiment and adjust to your needs!

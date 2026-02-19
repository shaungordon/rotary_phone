# Sound Editing Notes

## File Locations

- **Raw source files:** `sounds/raw/` (not tracked in git — local only)
- **Edited assets for firmware:** `firmware/data/audio/`

## Export Settings

Audio files must be formatted for the ESP32S3 / LittleFS playback:
- Format: WAV (PCM)
- Sample rate: 16000 Hz (16 kHz)
- Bit depth: 16-bit
- Channels: Mono

## Workflow

1. Place raw recordings in `sounds/raw/`
2. Edit in Audacity (or similar):
   - Trim silence at start/end
   - Normalize to around -3 dB peak
   - Apply noise reduction if needed
3. Export to `firmware/data/audio/` using the settings above
4. Update firmware with any new filenames / bird character mappings

## Naming Convention

```
answer_<bird>.wav       — plays when bird "picks up" the call
squawk_<bird>_<n>.wav   — response clips triggered by VAD pauses
```

## Notes

<!-- Add per-file or per-bird notes here as you work -->

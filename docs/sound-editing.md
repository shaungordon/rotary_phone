# Sound Editing Notes

## File Locations

- **Raw source files:** `sounds/raw/` (not tracked in git — local only)
- **Edited assets for firmware:** `firmware/rotary_phone/data/audio/`

## Export Settings

Audio files must be formatted for the ESP32S3 / FFat playback:
- Format: WAV (Microsoft) signed 16-bit PCM
- Sample rate: 8000 Hz (8 kHz)
- Channels: Mono (1 Channel)

## Workflow

1. Place raw recordings in `sounds/raw/`
2. Edit in Audacity:
   - Trim silence at start/end
   - **Old Telephone Effect**:
     - Go to `Effect > Filter Curve EQ`.
     - Click `Manage > Factory Presets > Telephone`.
     - (Optional) Go to `Effect > Leveller` (set to light/moderate) to add vintage line distortion.
     - (Optional) Mix in a subtle background track of white noise or crackling for authenticity.
   - **Normalization**:
     - Go to `Effect > Normalize...` and set "Normalize peak amplitude to" **-12.0 dB**.
     - This ensures WAV files are balanced with the synthesized system tones (dial tone, ringing).
3. Export to `firmware/rotary_phone/data/audio/` using the settings above
4. Update firmware with any new filenames / bird character mappings

## Naming Convention

```
answer_<bird>.wav       — plays when bird "picks up" the call
squawk_<bird>_<n>.wav   — response clips triggered by VAD pauses
```

## Notes

- Using 8 kHz sample rate provides a more authentic "vintage phone" frequency response and saves significant space on the internal flash filesystem.
- Normalizing to -12 dB provides headroom for the global volume scaling applied in the firmware and prevents digital clipping.

# Rotary Phone Bird Caller

An interactive art installation using a vintage rotary phone. Dial a number and a bird answers — then it talks back to you.

## Concept

People pick up the handset and dial one of several special numbers. Each number connects them to a different bird character, which plays pre-recorded audio. Voice activity detection (VAD) lets the bird respond when the caller pauses, simulating a real conversation.

## Repository Structure

```
firmware/          ESP32S3 Arduino sketch and audio assets
vad_prototype/     Python proof-of-concept for voice activity detection (runs on Mac)
sounds/raw/        Raw/unedited source sound files (not tracked in git)
docs/              Notes on sound editing, hardware, etc.
TODO.md            Project task list
```

## Hardware

- Seeed XIAO ESP32S3
- Adafruit MAX98357A I2S amplifier
- Vintage rotary phone (handset speaker + rotary dial)
- LittleFS flash storage for audio files

## Firmware

The firmware in `firmware/` implements:
- State machine: on-hook → dial tone → dialing → connected → off-hook warning
- Rotary dial pulse detection with debouncing
- Synthesized tones (dial tone 350+440 Hz, ringback 440+480 Hz)
- I2S audio playback of WAV files stored in LittleFS
- Special 7-digit numbers mapped to bird character audio

See `firmware/rotary_phone.ino` for full implementation.

## VAD Prototype

`vad_prototype/` contains a Python script that validates the VAD algorithm on a Mac before porting to ESP32. It uses energy-based (RMS) detection to identify when the caller pauses and triggers a bird response.

See `vad_prototype/README.md` for setup and usage instructions.

## Sound Files

Edited audio assets live in `firmware/data/audio/`. Raw source files are stored locally in `sounds/raw/` (not tracked in git — see `docs/sound-editing.md` for the editing workflow).

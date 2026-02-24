# TODO

## Firmware
- [ ] Port VAD algorithm from `vad_prototype/` to ESP32S3 C++
- [ ] Integrate VAD with bird response playback in state machine
- [ ] Test VAD thresholds on actual phone handset microphone
- [ ] Add more bird characters / phone numbers

## Audio
- [ ] Record/source additional bird vocalizations
- [ ] Edit and export final audio for remaining bird characters
- [ ] Tune audio levels for phone speaker output

## Hardware
- [ ] Finalize wiring and enclosure inside phone body
- [ ] Test handset microphone input level with ESP32S3 ADC

## Project
- [ ] Document hardware wiring / pin assignments
- [ ] Write installation / setup instructions
- [ ] BLE device name: append last 4 hex digits of MAC address (e.g. "RotaryPhone-A3F1") so multiple units are distinguishable without conflicts

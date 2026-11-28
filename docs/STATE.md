# STATE (2026-11-28)

ESP32 + SI4732 receiver ("ATS-Mini" firmware fork). Two build profiles (`ats-mini/sketch.yaml`, pins in `ats-mini/Board.h`):

| Profile | Board | Radio features |
|---|---|---|
| `v3s` | ESP32-S3 N16R8 (PSRAM) | receive only, FT8/FT4 decode from analog audio on GPIO11 (ADC2), web UI |
| `classic` | ESP32-WROOM (no PSRAM) | receive plus FT8/WSPR transmit via Si5351 CLK2 and a relay on GPIO26 |

Si5351 outputs: CLK0 = optional 32768 Hz reference for the SI4732, CLK2 = transmit.

Web UI source is `webui/index.html`; regenerate `ats-mini/WebUI.h` with `python3 webui/build_webui.py`.

## Open
- Live FT8/FT4 decoding on the V3S board depends on antenna and band; verify after changes to the audio path.
- `partitions.csv` uses 4 MB of the 16 MB flash on the V3S; V3S SI4732 ppm is not calibrated.
- Transmit hardware (PA, LPF, relay) and enclosure dimensions are not verified, see `docs/enclosure-wiring.md`.
- Boot log shows `task_wdt: task not found` messages.

# STATE (handoff to PM, 2026-11-22)

Project: ESP32 (classic) + SI4732 ("ATS-Mini") radio, call XV5QH, grid OL21, web UI http://192.168.3.39. Independent RX check: Pi RTL-SDR at 192.168.3.24.

## Done
- FT8 TX via Si5351 CLK2; auto CQ / auto answer; single Auto toggle (on main: fae1394, 22d5239).
- WSPR TX on 20 m verified: Pi RTL-SDR decoded "XV5QH OL21 10" at -11 dB. WSPR RX via audio stream + wsprd on a host (b510748).
- Web fix: transmit/WSPR section stays visible at 14096 kHz USB (5e541f6).
- 3D-printable handheld enclosure, `tools/enclosure/handheld_box.scad` (+ `base.stl`, `lid.stl`, `preview_wiring.png`, `docs/enclosure-wiring.md`): board dims from the ATS-Mini Gerber, mounts, tie slots, speaker 70x30x30, full ghost harness in the preview (edc6aff).

## In progress
- Current branch `feat/auto-single-toggle` is ahead of `main` by 55 commits (`git log main..feat/auto-single-toggle`; mostly enclosure + earlier work); working tree clean, nothing uncommitted. Nothing pushed by this session.
- Other branches: feat/auto-qso, feat/tx-radio-off, feat/bt-a2dp-speaker, feat/web-options-memories-waterfall, perf/*, fix_ssb_digital_mode, make_repo_public, restore_firmware_add_features (some are worktrees "+"). Not reviewed this session.

## Open problems
- WSPR radiated power is weak: needs an external PA + 20 m LPF and/or a better antenna.
- Enclosure: module sizes/hole positions (PA, relay, LPF, Si5351) and pins marked "(?)" in docs/enclosure-wiring.md are assumed, not verified; harness notch at the divider top is crowded; encoder orientation, USB-C height, EC11 height, LCD stack unverified.

## Next steps (suggested)
1. Owner buys/measures real PA, LPF, relay modules; update `mods` in the .scad and pin table.
2. Print notes (orientation, layer height, supports) + a close-up render of the notch.
3. Optional: Si5351 ppm calibration, unified FT8/FT4/WSPR UI, merge `feat/auto-single-toggle` to main once reviewed.

## Decisions waiting on the owner
- Which PA/LPF/relay modules to use (drives enclosure dimensions); PA enable pin (GPIO12 vs GPIO19); whether to include the optional audio amp (the ATS-Mini board already has one).
- Whether to merge/push the enclosure work (push only on request).

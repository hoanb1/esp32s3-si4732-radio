# FT8 Transmit Design with Si5351 for ESP32 + SI4732

Goal: Enable automated FT8 QSOs (CQ → signal report → RR73) at QRP power levels with clean harmonic suppression and safe T/R switching for the SI4732 receiver.

## Why Not Use the ESP32 Internal APLL

Tested (2026-10-18) routing the ESP32 APLL via I2S0 MCLK to GPIO0:

- Minimum frequency step at 7 MHz is ~8.7 Hz, and at 28 MHz is ~34 Hz, whereas FT8 requires an exact 6.25 Hz tone spacing. Achieving 6.25 Hz requires high-rate dithering between adjacent steps.
- Every APLL coefficient write introduces a slight phase glitch, turning dithering into wideband click noise. Neither the SI4732 nor an external RTL-SDR could resolve clean FSK tones—only a noisy carrier.
- A raw GPIO square wave is amplitude/phase-modulated by Wi-Fi current draw, producing a 100 Hz sideband comb with no harmonic filtering.

The Si5351 solves all three problems: ~0.01 Hz MultiSynth resolution, glitch-free tone transitions, and a dedicated 50 Ω output stage.

## 1. Hardware

| Block | Recommendation | Notes |
|---|---|---|
| Synthesizer | Si5351A module (25/27 MHz crystal, ideally TCXO) | I2C address `0x60` |
| Buffer / PA | Stage 1: 3× parallel BS170 QRP Labs style (5 V, ~0.5–1 W). Stage 2: IRF510 @ 12 V (~5 W) | Separate PA supply rail from ESP32 |
| Low-Pass Filter | 7-element band-specific LPF (e.g. QRP Labs LPF kit for 40 m / 20 m) | Mandatory, harmonics ≤ −43 dBc |
| T/R Switch | DPDT signal relay (e.g. Omron G5V-2) driven by transistor + flyback diode | Fully isolates SI4732 input during TX |
| RX Input Protection | Back-to-back 1N4148 diodes + 47 Ω series resistor before AMI pin | Protects receiver against relay bounce |
| Dummy Load | 50 Ω ≥ 2 W (two parallel 100 Ω / 1 W resistors) | Used for all initial bring-up tests |

### Wiring

| ESP32 | Target | Notes |
|---|---|---|
| GPIO13 (SDA), GPIO14 (SCL) | Si5351 SDA/SCL | Shared I2C bus with SI4732 (SI4732 at `0x11`/`0x63`, no conflict with `0x60`) |
| 3V3, GND | Si5351 VIN, GND | Add 10 µF + 100 nF decoupling capacitors close to the module |
| GPIO26 | T/R relay driver transistor | `HIGH` = transmit |
| GPIO19 | PA enable (high-side/bias MOSFET) | `HIGH` = PA enabled, default `LOW` at boot |
| — | Si5351 CLK0 → PA → LPF → relay → antenna | Short coaxial jumpers |
| — | Relay (NC) → SI4732 AMI | Connects antenna to receiver when idle |
| (optional) | Si5351 CLK2 → SI4732 RCLK | Replaces high-drift 32.768 kHz crystal (~35 ppm/hour measured) |

Avoid GPIO0, GPIO2, and GPIO12 as they are boot strapping pins. Available free pins: 19, 21, 22, 26.

## 2. Firmware

1. **Si5351 Driver**: Use the Etherkit Si5351 library or a minimal custom register driver. Lock the PLL (e.g. PLLA ~ 700–900 MHz) and update only the MultiSynth0 fractional registers on tone changes; each tone writes 8 registers over 400 kHz I2C (~0.3 ms).
2. **FT8 Transmitter (`Ft8Tx.cpp`)**:
   - Encode messages using `ftx_message_encode` + `ft8_encode` from `ft8_lib` (already integrated in firmware).
   - Start transmission at 0.5 s into the slot (DT = 0) using the synchronized SNTP system clock; step through 79 symbols × 160 ms via `esp_timer`.
   - Transmit frequency = dial frequency + selected audio offset (default 1500 Hz, or a clear frequency on the waterfall).
   - Avoid I2C bus contention with SI4732: serialize all I2C transactions on the same task or guard with a mutex.
3. **T/R Sequence**: Mute RX audio / hold SI4732 → switch relay to TX → wait 10 ms → enable PA → enable CLK0 → transmit → disable CLK0 → disable PA → wait 10 ms → release relay to RX. The decoder skips the active transmit slot.
4. **Automated QSO**: State machine driven by decoded messages: CQ → (retry) → signal report → R-report → RR73 → 73. Includes a "Halt TX" button on LCD, Web UI, and Serial, plus a maximum retry count.
5. **User Interface**: Web UI and LCD controls for Callsign + Grid Locator, CQ button, TX audio pitch selection, and live TX status indicator. Callsign is stored in NVS; transmission is blocked if no callsign is configured.
6. **Calibration**: Si5351 frequency correction (ppm) measured against an RTL-SDR/OpenWebRX reference or derived from average frequency offsets of decoded FT8 stations.

## 3. Testing Procedure

1. Transmit an unmodulated Si5351 carrier into a dummy load; measure frequency accuracy and spectral purity with an RTL-SDR (OpenWebRX on Raspberry Pi): verify absence of 100 Hz sideband comb.
2. Transmit low-power FT8 into a dummy load and verify clean decoding in OpenWebRX/`jt9`, while confirming the ESP32 receiver skips the transmit slot.
3. Verify T/R relay switching: confirm SI4732 resumes normal reception on the very next slot without front-end overload or lockup.
4. Measure harmonic suppression after the LPF using an RTL-SDR at 2× and 3× the transmit frequency.
5. Connect antenna, transmit a short CQ with callsign, and verify reception reports on PSKReporter.
6. Enable automated QSO mode.

## 4. Regulatory and Safety Notes

- Transmit only with a valid amateur radio license and within authorized amateur bands; every transmission must include a valid callsign.
- Always use an appropriate band LPF before the antenna; never transmit without a 50 Ω dummy load or resonant antenna connected.
- Power the PA from a separate fused supply rail; keep ESP32 and SI4732 on a clean regulated supply to prevent RF feedback into the receiver.

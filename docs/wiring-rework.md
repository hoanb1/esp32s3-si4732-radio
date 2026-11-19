# Wiring rework: Si5351 + Si4732 digital audio

Status: plan, nothing soldered yet. "(?)" = not verified on the board, check with a multimeter/photo first. Crystal pins 2+13 confirmed (breakout schematic, 2026-11-19).

## Existing wiring (from `ats-mini/Common.h`, unchanged)

| ESP32 | Goes to |
|---|---|
| GPIO13 | I2C SDA (Si4732) |
| GPIO14 | I2C SCL (Si4732) |
| GPIO27 | Si4732 RST (pin 9) |
| GPIO34 | audio ADC1_CH6, from Si4732 line out (`Common.h:57`) |
| GPIO15 | speaker amp enable |
| GPIO16 | LCD backlight |
| GPIO23/18/5/17/4 | LCD MOSI/SCLK/CS/DC/RST |
| GPIO33/32/25 | encoder A/B/push |
| GPIO35 | VBAT monitor |

## Phase A: Si5351 + external reference clock (analog audio stays)

| # | From | To | Note |
|---|---|---|---|
| A1 | ESP32 GPIO13 | Si5351 SDA | shared bus, Si5351 is 0x60 |
| A2 | ESP32 GPIO14 | Si5351 SCL | shared bus |
| A3 | ESP32 3V3 | Si5351 VIN | 10 uF + 100 nF at the module |
| A4 | ESP32 GND | Si5351 GND | |
| A5 | Si5351 CLK2 | Si4732 pin 13 (RCLK) | 32.768 kHz square wave |
| A6 | Si4732 pin 13 / pin 2 | remove crystal XT1 (32.768 kHz) and C1 (22 pF, pin 2 side); C2 (22 pF, pin 13 side) may stay or go | confirmed on the breakout schematic: XT1 sits between pin 2 (DCLK) and pin 13 (RCLK), each side has a 22 pF to GND. Pin 2 goes nowhere else, so it is free after removal |

Firmware: Si4732 `REFCLK_FREQ`/`REFCLK_PRESCALE` set for 32.768 kHz, SSB patch reloaded, `rx_ppm` back to 0 (then calibrate again).

## Phase B: Si4732 digital audio (I2S slave, loses the analog speaker path)

| # | From | To | Note |
|---|---|---|---|
| B1 | ESP32 GPIO21 (I2S BCK) | Si4732 pin 2 (DCLK) | 47-100 ohm in series; free only after A6 |
| B2 | ESP32 GPIO22 (I2S WS) | Si4732 pin 1 (DFS) | 47-100 ohm in series; cut LOUT trace to GPIO34 and amp |
| B3 | Si4732 pin 16 (DOUT) | ESP32 GPIO34 (I2S DIN) | cut ROUT trace to the amp; GPIO34 is input-only, fine |

Speaker then needs a new source (ESP32 PWM/sigma-delta + RC filter, or an I2S DAC module).
Unverified: whether the SSB patch outputs audio on the digital path. Do phase A first, then test B with temporary wires.

## Phase C: FT8 transmit (later)

| ESP32 | To |
|---|---|
| GPIO26 | T/R relay driver (HIGH = TX) |
| GPIO19 | PA enable (HIGH = on, LOW at boot) |
| - | Si5351 CLK0 -> PA -> LPF -> relay -> antenna; relay NC -> Si4732 AMI |

See `docs/ft8-transmit-si5351.md`.

## Pin summary after A+B+C

| ESP32 | Si4732 | Si5351 | Other |
|---|---|---|---|
| GPIO13 SDA | SDA (12) | SDA | |
| GPIO14 SCL | SCLK (11) | SCL | |
| GPIO27 | RST (9) | | |
| GPIO21 | DCLK (2) | | |
| GPIO22 | DFS (1) | | |
| GPIO34 | DOUT (16) | | |
| | RCLK (13) | CLK2 | |
| | | CLK0 | PA input |
| GPIO26 | | | relay |
| GPIO19 | | | PA enable |

## Firmware facts (checked in code)

- Reference clock is set in code, not by the crystal alone: `rx.setRefClock(32802)` at `ats-mini.ino:419/441/452` (FM/AM/SSB), plus `ssbPpmLoad()` and the `rx_ppm` web action. Debug command `P<Hz>` (`Remote.cpp:513`) accepts 31130..34406 Hz.
- With the Si5351 on RCLK: output exactly 32768 Hz, set `REFCLK_FREQ` to 32768, `rx_ppm` to 0, then recalibrate. Remove the 32802 hard-coding.
- Si4732 GPO1 (pin 4) is already used as output (`setGpioCtl(1,0,0)`); GPO2 (pin 3) and GPO3 (pin 2) are unused by the code.
- `Ft8Tx.cpp` (866 lines) already exists and transmits with the ESP32 APLL (I2S0 MCLK on GPIO0 or BCK on a GPIO). It resets the Si4732 around TX (`txRadioOff/txRadioOn`). The Si5351 replaces that tone path; the APLL code goes away.

## External RCLK firmware option (Si5351 CLK2 -> Si4732 RCLK)

- Off by default. After the crystal (XT1) and C1 are removed and CLK2 is wired to Si4732 pin 13:
  `curl "http://<radio>/api/control?action=ext_ref&val=1"` (saves to NVS, board restarts).
- Boot with it enabled: Si5351 CLK2 = 32768 Hz starts before the SI4732 reset, `POWER_UP` with XOSCEN=RCLK, `REFCLK_FREQ` = 32768, ppm calibration key `ssbppm10x` (default 0, separate from the crystal's `ssbppm10` = 893 ppm).
- Safety: if a boot with it enabled never gets through `rx.setup()` (hang) or the SI4732 is not detected, the next boot switches it off and runs on the crystal. Revert by hand: `action=ext_ref&val=0`.
- Check after enabling: log shows `[SI5351] SI4732 reference = CLK2 32768 Hz`; dial a known carrier and recalibrate `rx_ppm` (it starts at 0).

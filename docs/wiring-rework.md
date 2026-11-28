# Wiring rework: Si5351 + Si4732 digital audio

Classic ESP32 board only (the V3S board uses other pins). The external reference clock is wired (see "as built" at the end); the other phases are plans. "(?)" = not verified on the board, check with a multimeter/photo first. Crystal pins 2+13 confirmed (breakout schematic, 2026-11-19).

## Existing wiring (from `ats-mini/Board.h`)

| ESP32 | Goes to |
|---|---|
| GPIO13 | I2C SDA (Si4732) |
| GPIO14 | I2C SCL (Si4732) |
| GPIO27 | Si4732 RST (pin 9) |
| GPIO34 | audio ADC1_CH6, from Si4732 line out (`Board.h`) |
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
| A5 | Si5351 CLK0 | Si4732 pin 13 (RCLK) | 32.768 kHz square wave |
| A6 | Si4732 pin 13 / pin 2 | remove crystal XT1 (32.768 kHz) and C1 (22 pF, pin 2 side); C2 (22 pF, pin 13 side) may stay or go | confirmed on the breakout schematic: XT1 sits between pin 2 (DCLK) and pin 13 (RCLK), each side has a 22 pF to GND. Pin 2 goes nowhere else, so it is free after removal |

Firmware: Si4732 `REFCLK_FREQ`/`REFCLK_PRESCALE` set for 32.768 kHz, SSB patch reloaded, `rx_ppm` back to 0 (then calibrate again).

## Phase B: Si4732 digital audio (I2S slave, loses the analog speaker path)

| # | From | To | Note |
|---|---|---|---|
| B1 | ESP32 GPIO19 (I2S BCK; the board has no GPIO21 pin) | Si4732 pin 2 (DCLK) | 47-100 ohm in series; free only after A6 |
| B2 | ESP32 GPIO22 (I2S WS) | Si4732 pin 1 (DFS) | 47-100 ohm in series; cut LOUT trace to GPIO34 and amp |
| B3 | Si4732 pin 16 (DOUT) | ESP32 GPIO34 (I2S DIN) | cut ROUT trace to the amp; GPIO34 is input-only, fine |

Speaker then needs a new source (ESP32 PWM/sigma-delta + RC filter, or an I2S DAC module).
Unverified: whether the SSB patch outputs audio on the digital path. Do phase A first, then test B with temporary wires.

## Phase C: FT8 transmit (later)

| ESP32 | To |
|---|---|
| GPIO26 | T/R relay driver (HIGH = TX) |
| GPIO12 | PA enable (optional, not used yet) |
| - | Si5351 CLK2 -> PA -> LPF -> relay -> antenna; relay NC -> Si4732 AMI |

See `docs/ft8-transmit-si5351.md`.

## Pin summary after A+B+C

| ESP32 | Si4732 | Si5351 | Other |
|---|---|---|---|
| GPIO13 SDA | SDA (12) | SDA | |
| GPIO14 SCL | SCLK (11) | SCL | |
| GPIO27 | RST (9) | | |
| GPIO19 | DCLK (2) | | |
| GPIO22 | DFS (1) | | |
| GPIO34 | DOUT (16) | | |
| | RCLK (13) | CLK0 | |
| | | CLK2 | PA input |
| GPIO26 | | | relay |
| GPIO12 | | | PA enable |

## External RCLK firmware option (Si5351 CLK0 -> Si4732 RCLK)

- Off by default. After the crystal (XT1) and C1 are removed and CLK0 is wired to Si4732 pin 13:
  `curl "http://<radio>/api/control?action=ext_ref&val=1"` (saves to NVS, board restarts).
- Boot with it enabled: Si5351 CLK0 (module pin "0") = 32768 Hz starts before the SI4732 reset, `POWER_UP` with XOSCEN=RCLK, `REFCLK_FREQ` = 32768, ppm calibration key `ssbppm10x` (default 0, separate from the crystal's `ssbppm10` = 893 ppm).
- Safety: if a boot with it enabled never gets through `rx.setup()` (hang) or the SI4732 is not detected, the next boot switches it off and runs on the crystal. Revert by hand: `action=ext_ref&val=0`.
- Check after enabling: log shows `[SI5351] SI4732 reference = CLK0 32768 Hz`; dial a known carrier and recalibrate `rx_ppm` (it starts at 0).

Wiring as built (2026-11-19): the rework wire runs from the module pin `0` (CLK0) to Si4732 pin 13, so CLK0 is the reference clock
(`SI5351_REF_CLK` in `Si5351Ctl.cpp`). CLK2 is the transmit output (`SI5351_TX_CLK`); CLK1 is free. Verified: with CLK0 = 32768 Hz the Si4732 comes alive
(RSSI 0 -> 42); with CLK2 only after a boot it stays at RSSI 0.

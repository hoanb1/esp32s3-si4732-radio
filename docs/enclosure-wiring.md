# Enclosure wiring (tools/enclosure/handheld_box.scad)

Harness routing and connections. Pin facts come from `docs/wiring-rework.md` and `ats-mini/Board.h`;
"(?)" = not verified on the board, check with a multimeter first.

## Where the wires go

| Path | Route |
|---|---|
| Battery bay -> RF section | through the 14 mm gap at the floor of the divider (front half) |
| RF section -> ATS-Mini board (I2C, relay, 3V3, GND) | over the divider through the 16 mm notch at its top (back half), then up to the board hanging from the lid |
| Speaker -> board | speaker sits in the back strip of the base under the board; wires go straight up to the board speaker pads |
| SMA -> relay | short coax across the RF section (right end wall) |
| Strapping | 6 cable-tie slot pairs in the base floor beside the modules |

## Connections

| # | From | To | Note |
|---|---|---|---|
| 1 | 2 x 18650 holder | power switch (front wall) -> ATS-Mini BAT connector | cells in PARALLEL (3.7 V). The board charges the cells itself (USB-C), no TP4056 |
| 2 | ESP32 GPIO13 (SDA), GPIO14 (SCL) | Si5351 SDA/SCL | shared bus with the SI4732 |
| 3 | ESP32 3V3, GND | Si5351 VIN, GND; relay logic | 10 uF + 100 nF at each module |
| 4 | Si5351 CLK (TX output) | PA input -> 20 m LPF -> relay NO | CLK2 (`SI5351_TX_CLK`) |
| 5 | relay COM | SMA (antenna) | |
| 6 | relay NC | SI4732 AMI input (via 47 ohm + back-to-back 1N4148) | receive path while idle |
| 7 | ESP32 GPIO26 | relay driver (transistor + flyback diode) | HIGH = TX |
| 8 | ESP32 GPIO12 or GPIO19 (?) | PA enable | optional |
| 9 | board speaker output | speaker 70 x 30 x 30 | the ATS-Mini has its own speaker amplifier (GPIO15 = enable), so the separate audio amp module is optional |
| 10 | PA supply | battery + (after the switch), 5 V boost if the PA needs it | keep the PA rail separate from the ESP32 3V3 |

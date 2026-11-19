#ifndef SI5351_CTL_H
#define SI5351_CTL_H

#include <Arduino.h>

// Si5351 (or MS5351M clone) clock generator on the shared I2C bus (GPIO13/14, address 0x60).
// Call only from loop() context: the SI4732 uses the same bus.

// Scan the bus, print the addresses found and initialise the Si5351. True if it answers at 0x60.
bool si5351Probe();

// Output clock `clk` (0..2) at `hz` (4 kHz..160 MHz); hz = 0 switches it off.
bool si5351Set(uint8_t clk, uint32_t hz);

// FT8 transmit tone: frequency in 0.01 Hz units (output state unchanged), and output on/off.
// Safe to call from the TX task: each call is whole Wire transactions.
bool si5351SetCentiHz(uint8_t clk, uint64_t centiHz);
bool si5351Enable(uint8_t clk, bool on);
bool si5351Drive(uint8_t clk, uint8_t level);   // 0..3 = 2/4/6/8 mA
#define SI5351_TX_CLK 2

// External SI4732 reference clock: Si5351 CLK0 = 32768 Hz on RCLK (pin 13) instead of the 32.768 kHz
// crystal. Needs the crystal (and C1) removed. Off by default; enabled in NVS, effective after a restart.
// If a boot with it enabled never gets through rx.setup() (no clock reached the chip) the next boot
// switches it off again.
bool extRefBegin();            // before the SI4732 reset: starts CLK2 if enabled; false = use the crystal
void extRefDone();             // after rx.setup() returned: this boot got through
bool extRefActive();
void extRefEnable(bool on);    // saved to NVS
uint16_t rxRefClockHz();       // REFCLK_FREQ for the SI4732: 32768 with the Si5351, 32802 (crystal error trim) without

#endif

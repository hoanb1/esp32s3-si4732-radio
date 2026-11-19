#ifndef SI5351_CTL_H
#define SI5351_CTL_H

#include <Arduino.h>

// Si5351 (or MS5351M clone) clock generator on the shared I2C bus (GPIO13/14, address 0x60).
// Call only from loop() context: the SI4732 uses the same bus.

// Scan the bus, print the addresses found and initialise the Si5351. True if it answers at 0x60.
bool si5351Probe();

// Output clock `clk` (0..2) at `hz` (4 kHz..160 MHz); hz = 0 switches it off.
bool si5351Set(uint8_t clk, uint32_t hz);

#endif

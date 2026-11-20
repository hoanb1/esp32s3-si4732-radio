#ifndef WSPR_H
#define WSPR_H

#include <stdint.h>

// WSPR (type 1 message: callsign, 4-char grid, power in dBm). Pure encoder, no hardware.
// 162 four-level symbols, 8192/12000 s each (0.6827 s), tone spacing 12000/8192 = 1.4648 Hz.
#define WSPR_NN 162
#define WSPR_SYMBOL_US 682667LL
#define WSPR_TONE_HZ 1.46484375

// call: up to 6 chars with a digit in position 2 or 3 (e.g. "K1ABC", "VK3ABC"); grid: 4 chars "AA00".."RR99";
// dbm: 0..60. Returns false for an invalid call/grid/power.
bool wsprEncode(const char *call, const char *grid, int dbm, uint8_t symbols[WSPR_NN]);

#endif

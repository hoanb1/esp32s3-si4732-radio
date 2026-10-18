#ifndef FT8_TX_H
#define FT8_TX_H

#include <Arduino.h>

// FT8 transmitter on the ESP32 itself: the audio PLL (APLL) drives either I2S0 MCLK on GPIO0
// (CLK_OUT1) or I2S0 BCK routed through the GPIO matrix to any pin. FT8 needs 6.25 Hz tone steps;
// one APLL fractional step is 40 MHz / 65536 / D at the output (D = total division), so a D near 98
// makes one step ~6.25 Hz and every tone is a single write of the low SDM byte. That only fits
// around 3.5-5 MHz (80 m FT8 at 3.573 MHz).

// Handle one serial 'K' command line (text after the 'K'); prints its result.
void ft8TxCommand(const char *args);
bool ft8TxBusy();   // carrier or FT8 message being sent

#endif // FT8_TX_H

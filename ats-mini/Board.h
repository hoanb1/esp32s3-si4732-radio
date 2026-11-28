#ifndef BOARD_H
#define BOARD_H

// Board selection (compile time). An ESP32-S3 build is the V3S board, anything else the classic
// ESP32 board. Included by Common.h and by tft_setup.h, so it may only hold preprocessor lines.

#include "sdkconfig.h"

#if !defined(BOARD_V3S) && defined(CONFIG_IDF_TARGET_ESP32S3)
#define BOARD_V3S
#endif

#if defined(BOARD_V3S)

// V3S: ESP32-S3-WROOM-1 N16R8 + SI4732-A10 (see tools/v3s-probe)
// SI4732/5 PINs
#define PIN_POWER_ON  15            // SI4732 supply enable (1 = Enable)
#define RESET_PIN     16            // SI4732/5 Reset
#define ESP32_I2C_SCL 17            // SI4732/5 Clock
#define ESP32_I2C_SDA 18            // SI4732/5 Data
#define AUDIO_MUTE    3             // Hardware L/R mute (1 = Mute)
#define PIN_AMP_EN    10            // Audio amplifier enable (1 = Enable)

// Display PINs, the others are in tft_setup.h (8-bit parallel bus)
#define PIN_LCD_BL    38            // LCD backlight (PWM brightness control)

// Rotary Encoder PINs
#define ENCODER_PIN_A       2
#define ENCODER_PIN_B       1
#define ENCODER_PUSH_BUTTON 21

// ADC
#define PIN_VBAT      4             // Battery voltage divider
#define FT8_ADC_PIN   11            // SI4732 LOUT (pin 1) single-wire mod, ADC2_CH0 (unreliable with WiFi on)

// Features
#define HAS_FT8_ADC   0             // Wire fitted; stays 0 until ESP32-S3 ADC capture is written
#define HAS_APLL_TX   0             // ESP32 APLL RF output (classic ESP32 only)
#define HAS_TX        0             // Si5351 CLK2 / PA transmit path not fitted yet
#define TX_RELAY_PIN  -1

// SI4732 crystal error default in 0.1 ppm: not measured on this board yet
#define RX_PPM_CAL10_DEFAULT 0

// Digital audio input (SI4732 DOUT): not wired
#define DIG_PIN_BCK   -1
#define DIG_PIN_WS    -1

#else

// Classic ESP32-D0WD-V3 board
// SI4732/5 PINs
#define PIN_POWER_ON  -1            // No switchable SI4732 supply on this board
#define RESET_PIN     27            // GPIO27   SI4732/5 Reset
#define ESP32_I2C_SCL 14            // GPIO14   SI4732/5 Clock
#define ESP32_I2C_SDA 13            // GPIO13   SI4732/5 Data
#define AUDIO_MUTE    -1            // Hardware L/R mute disabled
#define PIN_AMP_EN    15            // GPIO15   Speaker amplifier enable (1 = Enable); SI4732 line out to GPIO34 is unaffected

// Display PINs
#define PIN_LCD_BL    16            // GPIO16   LCD backlight (PWM brightness control)
// All other pins are defined by the TFT_eSPI library (MOSI=23, SCLK=18, CS=5, DC=17, RST=4)

// Rotary Encoder PINs (GPIO34 is SI4732 Audio ADC1_CH6, GPIO35 is VBAT_MON)
#define ENCODER_PIN_A       33      // GPIO33
#define ENCODER_PIN_B       32      // GPIO32
#define ENCODER_PUSH_BUTTON 25      // GPIO25

// ADC
#define PIN_VBAT      35
#define FT8_ADC_PIN   34

// Features
#define HAS_FT8_ADC   1
#define HAS_APLL_TX   1
#define HAS_TX        1
#define TX_RELAY_PIN  26            // T/R relay module, active high

// SI4732 crystal error default in 0.1 ppm, measured on this board (see ats-mini.ino)
#define RX_PPM_CAL10_DEFAULT 8929

// Digital audio input: SI4732 DCLK / DFS (DOUT goes to FT8_ADC_PIN)
#define DIG_PIN_BCK   19
#define DIG_PIN_WS    22

#endif

#endif // BOARD_H

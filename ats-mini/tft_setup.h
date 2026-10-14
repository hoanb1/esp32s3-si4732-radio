// ST7789 170x320 SPI Setup for ESP32-D0WD-V3 + SI4732 Radio

#define USER_SETUP_ID 206

#define ST7789_DRIVER
#define INIT_SEQUENCE_3

#define CGRAM_OFFSET
#define TFT_RGB_ORDER TFT_RGB
#define TFT_INVERSION_ON

#define TFT_WIDTH  170
#define TFT_HEIGHT 320

#define TFT_MISO -1
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS    5
#define TFT_DC   17
#define TFT_RST   4

// Disable TFT_BL here because PIN_LCD_BL (GPIO16) is controlled via PWM in ats-mini.ino
// #define TFT_BL 16
#define TFT_BACKLIGHT_ON HIGH

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

#define SMOOTH_FONT

#define SPI_FREQUENCY  27000000
#define SPI_READ_FREQUENCY  20000000

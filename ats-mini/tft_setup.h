// ST7789 170x320 display setup (classic ESP32: SPI, V3S: 8-bit parallel)

#include "Board.h"

#if defined(BOARD_V3S)

// V3S: ST7789 170x320 on the 8-bit parallel bus (same wiring as the upstream ATS-Mini / T-Display-S3)
#define USER_SETUP_ID 206

#define ST7789_DRIVER
#define INIT_SEQUENCE_3

#define CGRAM_OFFSET
#define TFT_RGB_ORDER TFT_RGB
#define TFT_INVERSION_ON

#define TFT_PARALLEL_8_BIT

#define TFT_WIDTH  170
#define TFT_HEIGHT 320

#define TFT_CS   6
#define TFT_DC   7
#define TFT_RST  5
#define TFT_WR   8
#define TFT_RD   9

#define TFT_D0  39
#define TFT_D1  40
#define TFT_D2  41
#define TFT_D3  42
#define TFT_D4  45
#define TFT_D5  46
#define TFT_D6  47
#define TFT_D7  48

#else

// Classic ESP32 board
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

#endif

// TFT_BL is not defined: PIN_LCD_BL (Board.h) is controlled via PWM in ats-mini.ino
#define TFT_BACKLIGHT_ON HIGH

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

#define SMOOTH_FONT

#if !defined(BOARD_V3S)
#define SPI_FREQUENCY  27000000
#define SPI_READ_FREQUENCY  20000000
#endif

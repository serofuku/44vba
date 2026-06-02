#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
void lcdInit();
void lcdSetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
void lcdWriteFB(uint8_t *buf, int len);
void lcdFlushDMA();
void lcdFillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void lcdDrawText(uint16_t x, uint16_t y, const char *text, uint16_t fg, uint16_t bg);
void delayMS(int ms);
void osInit();
uint32_t osReadKey();

#define LCD_W (240)
#define LCD_H (320)

// LCD pins
#define PIN_LCD_DC    47
#define PIN_LCD_CS    -1
#define PIN_SPI0_MOSI 12
#define PIN_SPI0_MISO -1
#define PIN_SPI0_SCLK 48
#define PIN_SYS_RSTN  3
#define PIN_LCD_BCKL  39

// Button pins
#define PIN_KEY_UP     (1)
#define PIN_KEY_DOWN   (2)
#define PIN_KEY_LEFT   (3)
#define PIN_KEY_RIGHT  (4)
#define PIN_KEY_A      (15)
#define PIN_KEY_B      (5)
#define PIN_KEY_SELECT (16)
#define PIN_KEY_START  (17)

// SD card pins (SPI mode)
#define PIN_SD_MOSI   11
#define PIN_SD_MISO   13
#define PIN_SD_CLK    10
#define PIN_SD_CS     9

#ifdef __cplusplus
}
#endif

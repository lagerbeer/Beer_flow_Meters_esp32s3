// pins.h - Add this file to your project
#ifndef PINS_H
#define PINS_H

// ST7789 TFT Display pins
#define TFT_CS           10   // Chip select
#define TFT_DC            8   // Data/Command
#define TFT_RST           9   // Reset (or connect to -1 and tie to 3.3V)
#define TFT_BL           45   // Backlight control (optional, use -1 if not used)
#define TFT_MOSI         11   // SPI MOSI
#define TFT_SCLK         12   // SPI SCLK

// Alternative pins for ESP32-S3:
// You can also use hardware SPI which typically uses:
// MOSI: 11, SCLK: 12, CS: 10, DC: 8, RST: 9

#endif
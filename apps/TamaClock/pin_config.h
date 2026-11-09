#pragma once

// Official pin map for the Waveshare ESP32-S3-Touch-AMOLED-1.75.
// Same file the two games use, kept here so the hub is a self-contained sketch.

#define XPOWERS_CHIP_AXP2101

// 466x466 AMOLED, CO5300 over QSPI
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_CS 12
#define LCD_RESET 39
#define LCD_WIDTH 466
#define LCD_HEIGHT 466

// CST9217 capacitive touch over I2C
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 11
#define TP_RESET 40

// microSD (not used by the hub, kept for reference)
#define SDMMC_CLK 2
#define SDMMC_CMD 1
#define SDMMC_DATA 3
#define SDMMC_CS 41

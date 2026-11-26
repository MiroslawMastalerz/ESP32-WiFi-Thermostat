#pragma once
// NiceMCU-32S-DEV 2.8IPS, PCB NiceMCU_32S_DEV2.0
// Piny na podstawie oficjalnego board_config.h producenta.
#define USER_SETUP_INFO "NiceMCU_32S_DEV_2_8IPS"
#define ST7789_DRIVER
#define TFT_WIDTH 240
#define TFT_HEIGHT 320

#define TFT_MISO -1
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS 15
#define TFT_DC 12
#define TFT_RST 2 // Wspolny reset LCD + CST816D
#define TFT_BL 25
#define TFT_BACKLIGHT_ON HIGH

#define USE_HSPI_PORT
// ZMIANA: TFT_RGB -> TFT_BGR (kolejność kanałów zgodna z panelem NiceMCU)
#define TFT_RGB_ORDER TFT_BGR 
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_GFXFF
#define SMOOTH_FONT

// Producent uzywa 24 MHz: zachowujemy to na pierwsze uruchomienie.
#define SPI_FREQUENCY 24000000
#define SPI_READ_FREQUENCY 16000000
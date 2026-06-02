#include "os.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "../../src/vgafont8.h"

#define TAG "OS"

void delayMS(int ms) { vTaskDelay(ms / portTICK_PERIOD_MS); }

spi_device_handle_t spiDev0;

// DMA transaction pool
#define LCD_DMA_CHUNKS 8
static spi_transaction_t dmaTransactions[LCD_DMA_CHUNKS];
static int dmaQueued = 0;

static void lcdWrite(uint8_t *buf, int len, int isCmd) {
    if (len <= 0) return;
    gpio_set_level(PIN_LCD_DC, isCmd ? 0 : 1);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = 8 * len;
    t.tx_buffer = buf;
    ESP_ERROR_CHECK(spi_device_polling_transmit(spiDev0, &t));
    gpio_set_level(PIN_LCD_DC, 1);
}

static void lcdCmd8(uint8_t cmd) { lcdWrite(&cmd, 1, 1); }
static void lcdDat8(uint8_t dat) { lcdWrite(&dat, 1, 0); }

static void lcdDat16(uint16_t dat) {
    uint8_t buf[2] = {dat >> 8, dat & 0xff};
    lcdWrite(buf, 2, 0);
}

void lcdSetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    lcdCmd8(0x2A);
    lcdDat16(x1);
    lcdDat16(x2);
    lcdCmd8(0x2B);
    lcdDat16(y1);
    lcdDat16(y2);
    lcdCmd8(0x2C);
}

void lcdWriteFB(uint8_t *buf, int len) {
    if (len <= 0) return;
    spi_transaction_t *rtrans;
    while (dmaQueued > 0) {
        ESP_ERROR_CHECK(spi_device_get_trans_result(spiDev0, &rtrans, portMAX_DELAY));
        dmaQueued--;
    }
    gpio_set_level(PIN_LCD_DC, 1);
    const int maxLen = 32760;
    int chunk = 0;
    while (len > 0) {
        int writeLen = len > maxLen ? maxLen : len;
        spi_transaction_t *t = &dmaTransactions[chunk % LCD_DMA_CHUNKS];
        memset(t, 0, sizeof(*t));
        t->length    = 8 * writeLen;
        t->tx_buffer = buf;
        ESP_ERROR_CHECK(spi_device_queue_trans(spiDev0, t, portMAX_DELAY));
        dmaQueued++;
        buf += writeLen;
        len -= writeLen;
        chunk++;
    }
}

void lcdFlushDMA() {
    spi_transaction_t *rtrans;
    while (dmaQueued > 0) {
        ESP_ERROR_CHECK(spi_device_get_trans_result(spiDev0, &rtrans, portMAX_DELAY));
        dmaQueued--;
    }
}

void lcdFillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    lcdSetWindow(x, y, x + w - 1, y + h - 1);
    uint8_t buf[128];
    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xff;
    for (int i = 0; i < 128; i += 2) {
        buf[i]   = hi;
        buf[i+1] = lo;
    }
    int total = w * h * 2;
    int sent = 0;
    while (sent < total) {
        int chunk = total - sent;
        if (chunk > 128) chunk = 128;
        lcdWrite(buf, chunk, 0);
        sent += chunk;
    }
}

void lcdDrawChar(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg) {
    if (c < 32 || c > 127) c = '?';
    const uint8_t *glyph = &vgafont8[(c - 32) * 8];
    lcdSetWindow(x, y, x + 7, y + 7);
    uint8_t buf[8 * 8 * 2];
    int idx = 0;
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 7; col >= 0; col--) {
            uint16_t color = (bits & (1 << col)) ? fg : bg;
            buf[idx++] = color >> 8;
            buf[idx++] = color & 0xff;
        }
    }
    lcdWrite(buf, sizeof(buf), 0);
}

void lcdDrawText(uint16_t x, uint16_t y, const char *text, uint16_t fg, uint16_t bg) {
    while (*text) {
        lcdDrawChar(x, y, *text++, fg, bg);
        x += 8;
        if (x + 8 > LCD_W) break;
    }
}

void lcdInit() {
    gpio_set_direction(PIN_LCD_BCKL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_BCKL, 1);
    gpio_set_direction(PIN_SYS_RSTN, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_SYS_RSTN, 0);
    delayMS(100);
    gpio_set_level(PIN_SYS_RSTN, 1);
    delayMS(100);
    gpio_set_direction(PIN_LCD_DC, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_DC, 1);

    static const spi_bus_config_t buscfg = {
        .miso_io_num     = PIN_SPI0_MISO,
        .mosi_io_num     = PIN_SPI0_MOSI,
        .sclk_io_num     = PIN_SPI0_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 32760,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    static const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 80000000,
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 8,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spiDev0));

    lcdCmd8(0x01); delayMS(50);
    lcdCmd8(0x11); delayMS(120);
    lcdCmd8(0xCF); lcdDat8(0x00); lcdDat8(0xC3); lcdDat8(0x30);
    lcdCmd8(0xED); lcdDat8(0x64); lcdDat8(0x03); lcdDat8(0x12); lcdDat8(0x81);
    lcdCmd8(0xE8); lcdDat8(0x85); lcdDat8(0x00); lcdDat8(0x78);
    lcdCmd8(0xCB); lcdDat8(0x39); lcdDat8(0x2C); lcdDat8(0x00); lcdDat8(0x34); lcdDat8(0x02);
    lcdCmd8(0xF7); lcdDat8(0x20);
    lcdCmd8(0xEA); lcdDat8(0x00); lcdDat8(0x00);
    lcdCmd8(0xC0); lcdDat8(0x1B);
    lcdCmd8(0xC1); lcdDat8(0x12);
    lcdCmd8(0xC5); lcdDat8(0x32); lcdDat8(0x3C);
    lcdCmd8(0xC7); lcdDat8(0x91);
    lcdCmd8(0x36); lcdDat8(0xC8);
    lcdCmd8(0x3A); lcdDat8(0x55);
    lcdCmd8(0xB1); lcdDat8(0x00); lcdDat8(0x10);
    lcdCmd8(0xB6); lcdDat8(0x0A); lcdDat8(0xA2);
    lcdCmd8(0xF2); lcdDat8(0x00);
    lcdCmd8(0x26); lcdDat8(0x01);
    lcdCmd8(0xE0);
    lcdDat8(0x0F); lcdDat8(0x31); lcdDat8(0x2B); lcdDat8(0x0C);
    lcdDat8(0x0E); lcdDat8(0x08); lcdDat8(0x4E); lcdDat8(0xF1);
    lcdDat8(0x37); lcdDat8(0x07); lcdDat8(0x10); lcdDat8(0x03);
    lcdDat8(0x0E); lcdDat8(0x09); lcdDat8(0x00);
    lcdCmd8(0xE1);
    lcdDat8(0x00); lcdDat8(0x0E); lcdDat8(0x14); lcdDat8(0x03);
    lcdDat8(0x11); lcdDat8(0x07); lcdDat8(0x31); lcdDat8(0xC1);
    lcdDat8(0x48); lcdDat8(0x08); lcdDat8(0x0F); lcdDat8(0x0C);
    lcdDat8(0x31); lcdDat8(0x36); lcdDat8(0x0F);
    lcdCmd8(0x11); delayMS(120);
    lcdCmd8(0x29);
}

int osKeyMap[12] = {
    PIN_KEY_A, PIN_KEY_B, PIN_KEY_SELECT, PIN_KEY_START,
    PIN_KEY_RIGHT, PIN_KEY_LEFT, PIN_KEY_UP, PIN_KEY_DOWN,
    -1, -1, -1, -1
};

uint32_t osReadKey() {
    uint32_t ret = 0;
    for (int i = 0; i < 12; i++) {
        if (osKeyMap[i] != -1) {
            if (gpio_get_level(osKeyMap[i]) == 0) {
                ret |= 1 << i;
            }
        }
    }
    return ret;
}

static sdmmc_card_t *sdCard = NULL;

esp_err_t sdInit() {
    // Pull-ups on all SD pins before init
    gpio_set_pull_mode(PIN_SD_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_CLK,  GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_CS,   GPIO_PULLUP_ONLY);

    // Small delay after pull-ups
    delayMS(20);

    esp_vfs_fat_sdmmc_mount_config_t mountCfg = {
        .format_if_mount_failed = false,
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = SPI3_HOST;
    host.max_freq_khz = 400; // Very slow for reliable init

    spi_bus_config_t sdBus = {
        .mosi_io_num     = PIN_SD_MOSI,
        .miso_io_num     = PIN_SD_MISO,
        .sclk_io_num     = PIN_SD_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &sdBus, SPI_DMA_CH_AUTO));

    sdspi_device_config_t slotCfg;
    memset(&slotCfg, 0, sizeof(slotCfg));
    slotCfg.host_id  = SPI3_HOST;
    slotCfg.gpio_cs  = PIN_SD_CS;
    slotCfg.gpio_cd  = SDSPI_SLOT_NO_CD;
    slotCfg.gpio_wp  = SDSPI_SLOT_NO_WP;
    slotCfg.gpio_int = GPIO_NUM_NC;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(
        "/sdcard", &host, &slotCfg, &mountCfg, &sdCard);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // After successful init speed up to 20MHz
    host.max_freq_khz = 20000;
    ESP_LOGI(TAG, "SD card mounted successfully");
    return ESP_OK;
}

void osInit() {
    lcdInit();
    for (int i = 0; i < 12; i++) {
        int pin = osKeyMap[i];
        if (pin != -1) {
            gpio_set_direction(pin, GPIO_MODE_INPUT);
            gpio_pullup_en(pin);
            gpio_pulldown_dis(pin);
        }
    }

    // Show SD status on screen
    lcdFillRect(0, 0, LCD_W, LCD_H, 0x0000);
    lcdDrawText(4, 140, "Initializing SD...", 0xFFFF, 0x0000);

    esp_err_t ret = sdInit();
    if (ret == ESP_OK) {
        lcdDrawText(4, 160, "SD: OK!", 0x07E0, 0x0000);
    } else {
        lcdDrawText(4, 160, "SD: FAILED!", 0xFFE0, 0x0000);
        lcdDrawText(4, 180, "Check wiring!", 0xFF00, 0x0000);
    }
    delayMS(1500);
}

#include "os.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define TAG "OS"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void delayMS(int ms) { vTaskDelay(ms / portTICK_PERIOD_MS); }

// ---------------------------------------------------------------------------
// LCD (SPI2_HOST, unchanged from original)
// ---------------------------------------------------------------------------

spi_device_handle_t spiDev0;

static void lcdWrite(uint8_t *buf, int len, int isCmd) {
    if (len <= 0) return;
    gpio_set_level(PIN_LCD_DC, isCmd ? 0 : 1);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8 * len;
    t.tx_buffer = buf;
    ESP_ERROR_CHECK(spi_device_transmit(spiDev0, &t));
    gpio_set_level(PIN_LCD_DC, 1);
}

static void lcdCmd8(uint8_t cmd) { lcdWrite(&cmd, 1, 1); }
static void lcdDat8(uint8_t dat) { lcdWrite(&dat, 1, 0); }
static void lcdDat16(uint16_t dat) {
    uint8_t buf[2] = {dat >> 8, dat & 0xff};
    lcdWrite(buf, 2, 0);
}

void lcdSetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    lcdCmd8(0x2A); lcdDat16(x1); lcdDat16(x2);
    lcdCmd8(0x2B); lcdDat16(y1); lcdDat16(y2);
    lcdCmd8(0x2C);
}

void lcdWriteFB(uint8_t *buf, int len) {
    if (len <= 0) return;
    const int maxLen = 240 * 60 * 2;
    while (len > 0) {
        int writeLen = len > maxLen ? maxLen : len;
        lcdWrite(buf, writeLen, 0);
        buf += writeLen;
        len -= writeLen;
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
        .miso_io_num = PIN_SPI0_MISO,
        .mosi_io_num = PIN_SPI0_MOSI,
        .sclk_io_num = PIN_SPI0_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 120000,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    static const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 40000000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spiDev0));

    // ILI9341 init sequence
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
    lcdCmd8(0x36); lcdDat8(0x48);
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

// ---------------------------------------------------------------------------
// Input (unchanged from original)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// SD card (SPI3_HOST)
// ---------------------------------------------------------------------------

static sdmmc_card_t *sdCard = NULL;

static esp_err_t sdMount() {
    ESP_LOGI(TAG, "Mounting SD card...");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;

    spi_bus_config_t sdBusCfg = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &sdBusCfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slotCfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slotCfg.gpio_cs = PIN_SD_CS;
    slotCfg.host_id = SPI3_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mountCfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slotCfg,
                                  &mountCfg, &sdCard);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_card_print_info(stdout, sdCard);
    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

uint8_t *sdLoadRom(const char *path, size_t *romSizeOut) {
    if (romSizeOut) *romSizeOut = 0;

    // Mount SD if not already mounted
    if (sdCard == NULL) {
        if (sdMount() != ESP_OK) {
            ESP_LOGE(TAG, "Cannot load ROM: SD not mounted");
            return NULL;
        }
    }

    // Get file size
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "ROM file not found: %s", path);
        return NULL;
    }
    size_t fileSize = (size_t)st.st_size;
    ESP_LOGI(TAG, "ROM size: %u bytes (%.2f MB)", fileSize,
             fileSize / 1048576.0f);

    // GBA ROMs must be at least 192 bytes (header) and <= 32MB
    if (fileSize < 192 || fileSize > 32 * 1024 * 1024) {
        ESP_LOGE(TAG, "ROM size out of range: %u", fileSize);
        return NULL;
    }

    // Allocate in PSRAM (malloc goes to PSRAM when
    // CONFIG_SPIRAM_USE_MALLOC=y and size > SPIRAM_MALLOC_ALWAYSINTERNAL)
    uint8_t *buf = (uint8_t *)malloc(fileSize);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for ROM", fileSize);
        return NULL;
    }
    ESP_LOGI(TAG, "ROM buffer allocated at %p", buf);

    // Read file in 32KB chunks to avoid stack overflow
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open ROM: %s", path);
        free(buf);
        return NULL;
    }

    const size_t chunkSize = 32 * 1024;
    size_t bytesRead = 0;
    while (bytesRead < fileSize) {
        size_t toRead = fileSize - bytesRead;
        if (toRead > chunkSize) toRead = chunkSize;
        size_t n = fread(buf + bytesRead, 1, toRead, f);
        if (n == 0) {
            ESP_LOGE(TAG, "Read error at offset %u", bytesRead);
            fclose(f);
            free(buf);
            return NULL;
        }
        bytesRead += n;
    }
    fclose(f);

    ESP_LOGI(TAG, "ROM loaded: %u bytes", bytesRead);
    if (romSizeOut) *romSizeOut = bytesRead;
    return buf;
}

// ---------------------------------------------------------------------------
// osInit
// ---------------------------------------------------------------------------

void osInit() {
    lcdInit();

    // Init button GPIOs
    for (int i = 0; i < 12; i++) {
        int pin = osKeyMap[i];
        if (pin != -1) {
            gpio_set_direction(pin, GPIO_MODE_INPUT);
            gpio_pullup_en(pin);
            gpio_pulldown_dis(pin);
        }
    }
}

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "os.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "spi_flash_mmap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define TAG "MAIN"
#undef B0
#include "gba.h"
#include "globals.h"

static uint16_t *FB;
volatile int frameDrawn = 0;
uint32_t frameCount = 0;

void emuRunFrame() {
    frameDrawn = 0;
    while (!frameDrawn) {
        CPULoop();
    }
    frameCount++;
}

void systemMessage(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ESP_LOGE("GBA", "%s", buf);
}

void systemDrawScreen(void) {
    frameDrawn = 1;
    uint16_t *src = pix;
    uint16_t *dst = FB;
    for (int y = 0; y < 160; y++) {
        for (int x = 0; x < 240; x++) {
            *dst++ = __builtin_bswap16(*src++);
        }
        src += 256 - 240;  // skip GBA buffer padding
    }
    lcdSetWindow(0, 40, 239, 199);  // center 160px in 320px display
    lcdWriteFB((uint8_t*)FB, 240 * 160 * 2);
}

void systemOnWriteDataToSoundBuffer(int16_t *finalWave, int length) {}

static void allocBuffers() {
    #define PSRAM_ALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    #define IRAM_ALLOC(size)  heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
    FB               = (uint16_t*)(PSRAM_ALLOC(240 * 160 * 2) ?: IRAM_ALLOC(240 * 160 * 2));
    vram             = (uint8_t *)(PSRAM_ALLOC(0x20000) ?: IRAM_ALLOC(0x20000));
    workRAM          = (uint8_t *)(PSRAM_ALLOC(0x40000) ?: IRAM_ALLOC(0x40000));
    bios             = (uint8_t *)(PSRAM_ALLOC(0x4000)  ?: IRAM_ALLOC(0x4000));
    pix              = (uint16_t*)(IRAM_ALLOC(4 * 256 * 160) ?: PSRAM_ALLOC(4 * 256 * 160));
    libretro_save_buf= (uint8_t *)(PSRAM_ALLOC(0x22000) ?: IRAM_ALLOC(0x22000));
    printf("FB:%p vram:%p workRAM:%p bios:%p pix:%p save:%p\n",
           FB, vram, workRAM, bios, pix, libretro_save_buf);
}

void emuInit() {
    CPUSetupBuffers();
    CPUInit(NULL, false);
    CPUReset();
    SetFrameskip(1);
}

extern "C" void app_main() {
    osInit();
    delayMS(200);
    allocBuffers();
    memset(FB, 0, 240 * 160 * 2);
    lcdSetWindow(0, 0, LCD_W - 1, LCD_H - 1);
    lcdWriteFB((uint8_t*)FB, LCD_W * LCD_H * 2);
    printf("44VBA starting...\n");

    spi_flash_mmap_handle_t outHandle;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "rom");
    if (!partition) {
        ESP_LOGE(TAG, "ROM partition not found!");
        return;
    }
    esp_err_t ret = esp_partition_mmap(
        partition, 0, partition->size,
        SPI_FLASH_MMAP_DATA, (const void **)&rom, &outHandle);
    ESP_ERROR_CHECK(ret);
    printf("ROM mapped at: %p size: %lu\n", rom, partition->size);

    emuInit();

    TickType_t fpsTick = xTaskGetTickCount();
    while (1) {
        joy = osReadKey();
        UpdateJoypad();
        emuRunFrame();
        if (frameCount % 120 == 0) {
            TickType_t now = xTaskGetTickCount();
            int ms = (now - fpsTick) * portTICK_PERIOD_MS;
            fpsTick = now;
            printf("FPS: %d\n", ms > 0 ? (120 * 1000 / ms) : 0);
        }
    }
}

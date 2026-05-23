#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "config.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "os.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "spi_flash_mmap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "MAIN"

#undef B0
#include "gba.h"
#include "globals.h"

// Use PSRAM for frame buffer
uint16_t *FB;

int frameDrawn = 0;
uint32_t frameCount = 0;

// Semaphore for dual core sync
static SemaphoreHandle_t frameSem;

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
    // Optimized pixel copy with bswap
    for (int y = 0; y < 160; y++) {
        for (int x = 0; x < 240; x++) {
            *dst++ = __builtin_bswap16(*src++);
        }
        src += 256 - 240;
    }
    lcdSetWindow(0, 40, 240 - 1, 200 - 1);
    lcdWriteFB((uint8_t*)FB, 240 * 160 * 2);
}

void systemOnWriteDataToSoundBuffer(int16_t *finalWave, int length) {}

void emuInit() {
    CPUSetupBuffers();
    CPUInit(NULL, false);
    CPUReset();
    // Set frameskip to 0 for full speed — change to 1 if too slow
    SetFrameskip(0);
}

extern "C" void app_main() {
    osInit();
    delayMS(500);

    // Allocate frame buffer in PSRAM
    FB = (uint16_t *)heap_caps_malloc(240 * 160 * 2, MALLOC_CAP_SPIRAM);
    if (!FB) {
        // Fallback to internal RAM
        FB = (uint16_t *)malloc(240 * 160 * 2);
    }
    memset(FB, 0, 240 * 160 * 2);

    lcdSetWindow(0, 0, LCD_W - 1, LCD_H - 1);
    lcdWriteFB((uint8_t*)FB, 240 * 160 * 2);
    lcdWriteFB((uint8_t*)FB, 240 * 160 * 2);

    printf("Hello world!\n");

    spi_flash_mmap_handle_t outHandle;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "rom");
    if (partition == NULL) {
        ESP_LOGE(TAG, "Failed to find rom partition");
        return;
    }

    esp_err_t ret = esp_partition_mmap(
        partition, 0, partition->size, SPI_FLASH_MMAP_DATA,
        (const void **)&rom, &outHandle);
    ESP_ERROR_CHECK(ret);
    printf("rom: %p\n", rom);

    // Allocate all large buffers in PSRAM
    vram             = (uint8_t *)heap_caps_malloc(0x20000, MALLOC_CAP_SPIRAM);
    workRAM          = (uint8_t *)heap_caps_malloc(0x40000, MALLOC_CAP_SPIRAM);
    bios             = (uint8_t *)heap_caps_malloc(0x4000,  MALLOC_CAP_SPIRAM);
    pix              = (uint16_t*)heap_caps_malloc(4 * 256 * 160, MALLOC_CAP_SPIRAM);
    libretro_save_buf= (uint8_t *)heap_caps_malloc(0x20000 + 0x2000, MALLOC_CAP_SPIRAM);

    // Fallback to internal RAM if PSRAM alloc fails
    if (!vram)             vram             = (uint8_t *)malloc(0x20000);
    if (!workRAM)          workRAM          = (uint8_t *)malloc(0x40000);
    if (!bios)             bios             = (uint8_t *)malloc(0x4000);
    if (!pix)              pix              = (uint16_t*)malloc(4 * 256 * 160);
    if (!libretro_save_buf)libretro_save_buf= (uint8_t *)malloc(0x20000 + 0x2000);

    printf("internalRAM: %p, vram: %p, workRAM: %p, bios: %p, pix: %p\n"
           "libretro_save_buf: %p\n",
           internalRAM, vram, workRAM, bios, pix, libretro_save_buf);

    emuInit();

    TickType_t fpsTick = xTaskGetTickCount();

    while (1) {
        joy = osReadKey();
        UpdateJoypad();
        emuRunFrame();

        if (frameCount % 60 == 0) {
            TickType_t now = xTaskGetTickCount();
            int msPassed = (now - fpsTick) * portTICK_PERIOD_MS;
            fpsTick = now;
            int fps = 60 * 1000 / msPassed;
            printf("FPS: %d\n", fps);
        }
    }
}

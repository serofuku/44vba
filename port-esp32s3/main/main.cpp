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
#include "freertos/semphr.h"

#define TAG "MAIN"
#undef B0
#include "gba.h"
#include "globals.h"

// Double buffer — Core 0 writes to back, Core 1 sends front to LCD
static uint16_t *FB[2];
static volatile int fbBack  = 0; // Core 0 writes here
static volatile int fbFront = 1; // Core 1 reads here

volatile int frameDrawn = 0;
uint32_t frameCount = 0;

static SemaphoreHandle_t fbReady;
static SemaphoreHandle_t fbDone;

// Core 1 — LCD transfer only
static void IRAM_ATTR displayTask(void *arg) {
    while (1) {
        xSemaphoreTake(fbReady, portMAX_DELAY);
        lcdSetWindow(0, 80, 239, 239);
        lcdWriteFB((uint8_t*)FB[fbFront], 240 * 160 * 2);
        lcdFlushDMA();
        xSemaphoreGive(fbDone);
    }
}

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

// Core 0 — pixel copy to back buffer then swap
void IRAM_ATTR systemDrawScreen(void) {
    frameDrawn = 1;

    // Wait for Core 1 to finish with front buffer
    xSemaphoreTake(fbDone, portMAX_DELAY);

    // Copy pixels to back buffer
    const uint16_t *src = pix;
    uint16_t *dst = FB[fbBack];
    for (int i = 0; i < 160; i++) {
        const uint32_t *s = (const uint32_t*)src;
        uint32_t *d = (uint32_t*)dst;
        for (int j = 0; j < 120; j++) {
            uint32_t px = *s++;
            *d++ = ((px & 0x00FF00FF) << 8) | ((px & 0xFF00FF00) >> 8);
        }
        src += 256;
        dst += 240;
    }

    // Swap buffers
    int tmp  = fbFront;
    fbFront  = fbBack;
    fbBack   = tmp;

    // Signal Core 1 to send new front buffer
    xSemaphoreGive(fbReady);
}

void systemOnWriteDataToSoundBuffer(int16_t *finalWave, int length) {}

static void allocBuffers() {
    #define PSRAM_ALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    #define IRAM_ALLOC(size)  heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

    // Two frame buffers for double buffering
    FB[0] = (uint16_t*)PSRAM_ALLOC(240 * 160 * 2);
    FB[1] = (uint16_t*)PSRAM_ALLOC(240 * 160 * 2);
    if (!FB[0]) FB[0] = (uint16_t*)IRAM_ALLOC(240 * 160 * 2);
    if (!FB[1]) FB[1] = (uint16_t*)IRAM_ALLOC(240 * 160 * 2);

    vram             = (uint8_t *)(PSRAM_ALLOC(0x20000) ?: IRAM_ALLOC(0x20000));
    workRAM          = (uint8_t *)(PSRAM_ALLOC(0x40000) ?: IRAM_ALLOC(0x40000));
    bios             = (uint8_t *)(PSRAM_ALLOC(0x4000)  ?: IRAM_ALLOC(0x4000));
    pix              = (uint16_t*)(PSRAM_ALLOC(4 * 256 * 160) ?: IRAM_ALLOC(4 * 256 * 160));
    libretro_save_buf= (uint8_t *)(PSRAM_ALLOC(0x22000) ?: IRAM_ALLOC(0x22000));

    printf("FB[0]:%p FB[1]:%p vram:%p workRAM:%p pix:%p\n",
           FB[0], FB[1], vram, workRAM, pix);
    printf("Free internal: %lu, Free PSRAM: %lu\n",
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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

    // Clear both buffers and full screen
    memset(FB[0], 0, 240 * 160 * 2);
    memset(FB[1], 0, 240 * 160 * 2);
    uint16_t *clearBuf = (uint16_t*)PSRAM_ALLOC(LCD_W * LCD_H * 2);
    if (clearBuf) {
        memset(clearBuf, 0, LCD_W * LCD_H * 2);
        lcdSetWindow(0, 0, LCD_W - 1, LCD_H - 1);
        lcdWriteFB((uint8_t*)clearBuf, LCD_W * LCD_H * 2);
        lcdFlushDMA();
        free(clearBuf);
    }

    // Create semaphores
    fbReady = xSemaphoreCreateBinary();
    fbDone  = xSemaphoreCreateBinary();
    xSemaphoreGive(fbDone);

    // Pin display task to Core 1 at highest priority
    xTaskCreatePinnedToCore(
        displayTask, "display",
        4096, NULL,
        configMAX_PRIORITIES - 1,
        NULL, 1);

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

    // Pin emulator to Core 0 at high priority
    vTaskPrioritySet(NULL, configMAX_PRIORITIES - 2);

    TickType_t fpsTick = xTaskGetTickCount();
    while (1) {
        joy = osReadKey();
        UpdateJoypad();
        emuRunFrame();
        if (frameCount % 60 == 0) {
            TickType_t now = xTaskGetTickCount();
            int ms = (now - fpsTick) * portTICK_PERIOD_MS;
            fpsTick = now;
            printf("FPS: %d\n", ms > 0 ? (60 * 1000 / ms) : 0);
        }
    }
}

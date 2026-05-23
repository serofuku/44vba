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
#include "esp_pm.h"
#include "os.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

#define TAG "MAIN"
#define FRAMESKIP 0  // 0 = no skip, 1 = skip every other, 2 = skip 2 etc

#undef B0
#include "gba.h"
#include "globals.h"

// Double framebuffers in PSRAM
static uint16_t *FB_front = NULL;
static uint16_t *FB_back  = NULL;

// Sync primitives
static SemaphoreHandle_t frameSemaphore = NULL;
static SemaphoreHandle_t bufferMutex    = NULL;

static int      frameDrawn       = 0;
static uint32_t frameCount       = 0;
static int      frameskipCounter = 0;

// ─── GBA callbacks ────────────────────────────────────────────────────────────

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

    // Frameskip
    frameskipCounter++;
    if (frameskipCounter <= FRAMESKIP) {
        return;
    }
    frameskipCounter = 0;

    // Copy pix → FB_back (2 pixels at a time using 32-bit writes)
    uint16_t *src   = pix;
    uint32_t *dst32 = (uint32_t *)FB_back;

    for (int y = 0; y < 160; y++) {
        for (int x = 0; x < 240; x += 2) {
            uint32_t p = ((uint32_t)__builtin_bswap16(src[x])) |
                         ((uint32_t)__builtin_bswap16(src[x + 1]) << 16);
            *dst32++ = p;
        }
        src += 256;  // GBA pix stride is 256 not 240
    }

    // Swap front and back buffers
    xSemaphoreTake(bufferMutex, portMAX_DELAY);
    uint16_t *tmp = FB_front;
    FB_front      = FB_back;
    FB_back       = tmp;
    xSemaphoreGive(bufferMutex);

    // Signal display task that a new frame is ready
    xSemaphoreGive(frameSemaphore);
}

void systemOnWriteDataToSoundBuffer(int16_t *finalWave, int length) {
    // Audio not implemented — silent
}

// ─── Emulator init ────────────────────────────────────────────────────────────

void emuInit() {
    CPUSetupBuffers();
    CPUInit(NULL, false);
    CPUReset();
    SetFrameskip(FRAMESKIP);
}

void emuRunFrame() {
    frameDrawn = 0;
    while (!frameDrawn) {
        CPULoop();
    }
    frameCount++;
}

// ─── Core 1: Emulator task ────────────────────────────────────────────────────

static void emuTask(void *arg) {
    TickType_t fpsTick = xTaskGetTickCount();

    while (1) {
        joy = osReadKey();
        UpdateJoypad();
        emuRunFrame();

        if (frameCount % 120 == 0) {
            TickType_t now = xTaskGetTickCount();
            int msPassed   = (now - fpsTick) * portTICK_PERIOD_MS;
            fpsTick        = now;
            int fps        = 120 * 1000 / msPassed;
            printf("FPS: %d\n", fps);
        }
    }
}

// ─── Core 0: Display task ─────────────────────────────────────────────────────

static void displayTask(void *arg) {
    while (1) {
        if (xSemaphoreTake(frameSemaphore, portMAX_DELAY) == pdTRUE) {
            xSemaphoreTake(bufferMutex, portMAX_DELAY);
            uint16_t *fb = FB_front;
            xSemaphoreGive(bufferMutex);

            lcdSetWindow(0, 40, 240 - 1, 200 - 1);
            lcdWriteFB((uint8_t *)fb, 240 * 160 * 2);
        }
    }
}

// ─── app_main ─────────────────────────────────────────────────────────────────

extern "C" void app_main() {
    // Lock both cores to 240MHz
    esp_pm_config_esp32s3_t pm_config = {
        .max_freq_mhz       = 240,
        .min_freq_mhz       = 240,
        .light_sleep_enable = false
    };
    esp_pm_configure(&pm_config);

    osInit();
    delayMS(500);

    // Allocate double framebuffers in PSRAM
    FB_front = (uint16_t *)heap_caps_malloc(240 * 160 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    FB_back  = (uint16_t *)heap_caps_malloc(240 * 160 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);

    if (!FB_front || !FB_back) {
        ESP_LOGE(TAG, "Failed to allocate framebuffers in PSRAM");
        return;
    }

    memset(FB_front, 0, 240 * 160 * sizeof(uint16_t));
    memset(FB_back,  0, 240 * 160 * sizeof(uint16_t));

    // Clear display on boot
    lcdSetWindow(0, 0, LCD_W - 1, LCD_H - 1);
    lcdWriteFB((uint8_t *)FB_front, 240 * 160 * sizeof(uint16_t));

    // Allocate all GBA buffers in PSRAM
    vram              = (uint8_t *)  heap_caps_malloc(0x20000,          MALLOC_CAP_SPIRAM);
    workRAM           = (uint8_t *)  heap_caps_malloc(0x40000,          MALLOC_CAP_SPIRAM);
    bios              = (uint8_t *)  heap_caps_malloc(0x4000,           MALLOC_CAP_SPIRAM);
    pix               = (uint16_t *) heap_caps_malloc(4 * 256 * 160,    MALLOC_CAP_SPIRAM);
    libretro_save_buf = (uint8_t *)  heap_caps_malloc(0x20000 + 0x2000, MALLOC_CAP_SPIRAM);

    printf("internalRAM: %p, vram: %p, workRAM: %p, bios: %p, pix: %p\n, libretro_save_buf: %p\n",
           internalRAM, vram, workRAM, bios, pix, libretro_save_buf);

    if (!vram || !workRAM || !bios || !pix || !libretro_save_buf) {
        ESP_LOGE(TAG, "Failed to allocate GBA buffers");
        return;
    }

    // Map ROM partition
    spi_flash_mmap_handle_t outHandle;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "rom");

    if (partition == NULL) {
        ESP_LOGE(TAG, "Failed to find rom partition");
        return;
    }

    esp_err_t ret = esp_partition_mmap(
        partition, 0, partition->size,
        ESP_PARTITION_MMAP_DATA,
        (const void **)&rom, &outHandle);
    ESP_ERROR_CHECK(ret);
    printf("rom: %p\n", rom);

    // Init emulator
    emuInit();

    // Create sync primitives
    frameSemaphore = xSemaphoreCreateBinary();
    bufferMutex    = xSemaphoreCreateMutex();

    if (!frameSemaphore || !bufferMutex) {
        ESP_LOGE(TAG, "Failed to create semaphores");
        return;
    }

    // Launch display task on Core 0
    xTaskCreatePinnedToCore(
        displayTask, "disp",
        4096, NULL, 5, NULL, 0);

    // Launch emulator task on Core 1
    xTaskCreatePinnedToCore(
        emuTask, "emu",
        8192, NULL, 5, NULL, 1);

    // app_main exits — tasks keep running
}

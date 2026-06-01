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

#define TAG "MAIN"

// ROM path on SD card — put your .gba file here
#define ROM_PATH "/sdcard/game.gba"

#undef B0
#include "gba.h"
#include "globals.h"

uint16_t FB[240 * 160];
int frameDrawn = 0;
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
    uint16_t *src = pix;   // Stride is 256
    uint16_t *dst = FB;    // Stride is 240
    for (int y = 0; y < 160; y++) {
        for (int x = 0; x < 240; x++) {
            *dst++ = __builtin_bswap16(*src++);
        }
        src += 256 - 240;
    }
    lcdSetWindow(0, 40, 240 - 1, 200 - 1);
    lcdWriteFB((uint8_t *)FB, 240 * 160 * 2);
}

void systemOnWriteDataToSoundBuffer(int16_t *finalWave, int length) {}

void emuInit() {
    CPUSetupBuffers();
    CPUInit(NULL, false);
    CPUReset();
    SetFrameskip(0x1);
}

extern "C" void app_main() {
    osInit();
    delayMS(500);

    // Clear screen
    lcdSetWindow(0, 0, LCD_W - 1, LCD_H - 1);
    lcdWriteFB((uint8_t *)FB, sizeof(FB));
    lcdWriteFB((uint8_t *)FB, sizeof(FB));

    printf("44VBA starting...\n");

    // Allocate GBA internal buffers
    vram              = (uint8_t  *)malloc(0x20000);
    workRAM           = (uint8_t  *)malloc(0x40000);
    bios              = (uint8_t  *)malloc(0x4000);
    pix               = (uint16_t *)malloc(4 * 256 * 160);
    libretro_save_buf = (uint8_t  *)malloc(0x20000 + 0x2000);

    printf("internalRAM: %p, vram: %p, workRAM: %p, bios: %p, pix: %p, "
           "libretro_save_buf: %p\n",
           internalRAM, vram, workRAM, bios, pix, libretro_save_buf);

    // Load ROM from SD card into PSRAM
    size_t romSize = 0;
    rom = (uint8_t *)sdLoadRom(ROM_PATH, &romSize);
    if (rom == NULL) {
        ESP_LOGE(TAG, "Failed to load ROM from %s — halting", ROM_PATH);
        // Blink backlight to signal error instead of silently hanging
        while (1) {
            gpio_set_level(PIN_LCD_BCKL, 0);
            delayMS(300);
            gpio_set_level(PIN_LCD_BCKL, 1);
            delayMS(300);
        }
    }
    printf("ROM loaded: %u bytes at %p\n", (unsigned)romSize, rom);

    emuInit();

    TickType_t fpsTick = xTaskGetTickCount();
    while (1) {
        joy = osReadKey();
        UpdateJoypad();
        emuRunFrame();

        if (frameCount % 120 == 0) {
            TickType_t now = xTaskGetTickCount();
            int msPassed = (now - fpsTick) * portTICK_PERIOD_MS;
            fpsTick = now;
            int fps = 120 * 1000 / msPassed;
            printf("FPS: %d\n", fps);
        }
    }
}

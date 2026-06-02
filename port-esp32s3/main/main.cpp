#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "os.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "MAIN"
#undef B0
#include "gba.h"
#include "globals.h"

// Colors RGB565
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_BLUE    0x001F
#define COLOR_CYAN    0x07FF
#define COLOR_GRAY    0x8410
#define COLOR_DGRAY   0x4208
#define COLOR_GREEN   0x07E0
#define COLOR_YELLOW  0xFFE0

// Double buffer
static uint16_t *FB[2];
static volatile int fbBack  = 0;
static volatile int fbFront = 1;

volatile int frameDrawn = 0;
uint32_t frameCount = 0;

static SemaphoreHandle_t fbReady;
static SemaphoreHandle_t fbDone;

// ROM loaded into PSRAM from SD
static uint8_t *romBuf = NULL;
static size_t   romSize = 0;

// ─── FILE SELECTOR ───────────────────────────────────────────

#define MAX_FILES   100
#define VISIBLE     16    // rows visible on screen at once
#define ROW_H       18    // pixels per row
#define HEADER_H    24    // header bar height

static char fileList[MAX_FILES][64];
static int  fileCount = 0;
static int  selected  = 0;
static int  scrollTop = 0;

static void scanGBA() {
    fileCount = 0;
    DIR *dir = opendir("/sdcard");
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && fileCount < MAX_FILES) {
        int len = strlen(ent->d_name);
        if (len > 4 &&
            (ent->d_name[len-4] == '.' ) &&
            (ent->d_name[len-3] == 'g' || ent->d_name[len-3] == 'G') &&
            (ent->d_name[len-2] == 'b' || ent->d_name[len-2] == 'B') &&
            (ent->d_name[len-1] == 'a' || ent->d_name[len-1] == 'A')) {
            strncpy(fileList[fileCount], ent->d_name, 63);
            fileList[fileCount][63] = 0;
            fileCount++;
        }
    }
    closedir(dir);
}

static void drawSelector() {
    // Header
    lcdFillRect(0, 0, LCD_W, HEADER_H, COLOR_BLUE);
    lcdDrawText(4, 8, "44VBA - Select ROM", COLOR_WHITE, COLOR_BLUE);

    // File list area
    lcdFillRect(0, HEADER_H, LCD_W, LCD_H - HEADER_H, COLOR_BLACK);

    if (fileCount == 0) {
        lcdDrawText(4, HEADER_H + 8, "No .gba files found!", COLOR_YELLOW, COLOR_BLACK);
        lcdDrawText(4, HEADER_H + 24, "Put .gba files on SD", COLOR_GRAY, COLOR_BLACK);
        return;
    }

    for (int i = 0; i < VISIBLE; i++) {
        int idx = scrollTop + i;
        if (idx >= fileCount) break;

        int y = HEADER_H + i * ROW_H;
        uint16_t bg = (idx == selected) ? COLOR_CYAN  : COLOR_BLACK;
        uint16_t fg = (idx == selected) ? COLOR_BLACK : COLOR_WHITE;

        lcdFillRect(0, y, LCD_W, ROW_H, bg);

        // Truncate filename to fit screen (240px / 8px per char = 30 chars)
        char display[31];
        strncpy(display, fileList[idx], 30);
        display[30] = 0;
        // Remove .gba extension for display
        int dl = strlen(display);
        if (dl > 4) display[dl - 4] = 0;

        lcdDrawText(4, y + 4, display, fg, bg);
    }

    // Scrollbar
    if (fileCount > VISIBLE) {
        int barH = (LCD_H - HEADER_H) * VISIBLE / fileCount;
        int barY = HEADER_H + (LCD_H - HEADER_H) * scrollTop / fileCount;
        lcdFillRect(LCD_W - 4, HEADER_H, 4, LCD_H - HEADER_H, COLOR_DGRAY);
        lcdFillRect(LCD_W - 4, barY, 4, barH, COLOR_CYAN);
    }

    // Bottom hint
    lcdFillRect(0, LCD_H - 16, LCD_W, 16, COLOR_DGRAY);
    lcdDrawText(4, LCD_H - 12, "UP/DN:Move  A:Select", COLOR_WHITE, COLOR_DGRAY);
}

static bool loadROM(const char *filename) {
    char path[128];
    snprintf(path, sizeof(path), "/sdcard/%s", filename);

    // Show loading screen
    lcdFillRect(0, 0, LCD_W, LCD_H, COLOR_BLACK);
    lcdDrawText(4, 140, "Loading...", COLOR_WHITE, COLOR_BLACK);
    lcdDrawText(4, 160, filename, COLOR_CYAN, COLOR_BLACK);

    FILE *f = fopen(path, "rb");
    if (!f) {
        lcdDrawText(4, 190, "ERROR: Cannot open!", COLOR_YELLOW, COLOR_BLACK);
        delayMS(2000);
        return false;
    }

    fseek(f, 0, SEEK_END);
    romSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Allocate ROM buffer in PSRAM
    romBuf = (uint8_t*)heap_caps_malloc(romSize, MALLOC_CAP_SPIRAM);
    if (!romBuf) {
        lcdDrawText(4, 190, "ERROR: Out of PSRAM!", COLOR_YELLOW, COLOR_BLACK);
        fclose(f);
        delayMS(2000);
        return false;
    }

    // Read in chunks with progress bar
    size_t chunkSize = 32768;
    size_t loaded = 0;
    while (loaded < romSize) {
        size_t toRead = romSize - loaded;
        if (toRead > chunkSize) toRead = chunkSize;
        fread(romBuf + loaded, 1, toRead, f);
        loaded += toRead;

        // Progress bar
        int barW = (LCD_W - 8) * loaded / romSize;
        lcdFillRect(4, 185, LCD_W - 8, 12, COLOR_DGRAY);
        lcdFillRect(4, 185, barW, 12, COLOR_GREEN);
    }
    fclose(f);

    char sizeBuf[32];
    snprintf(sizeBuf, sizeof(sizeBuf), "%d KB loaded", (int)(romSize / 1024));
    lcdDrawText(4, 205, sizeBuf, COLOR_GREEN, COLOR_BLACK);
    delayMS(500);
    return true;
}

static void runSelector() {
    scanGBA();
    drawSelector();

    uint32_t lastKey = 0;

    while (1) {
        uint32_t key = osReadKey();
        uint32_t pressed = key & ~lastKey;
        lastKey = key;

        bool changed = false;

        if (pressed & (1 << 6)) { // UP
            if (selected > 0) {
                selected--;
                if (selected < scrollTop) scrollTop = selected;
                changed = true;
            }
        }
        if (pressed & (1 << 7)) { // DOWN
            if (selected < fileCount - 1) {
                selected++;
                if (selected >= scrollTop + VISIBLE)
                    scrollTop = selected - VISIBLE + 1;
                changed = true;
            }
        }
        if (pressed & (1 << 0)) { // A button — select
            if (fileCount > 0) {
                if (loadROM(fileList[selected])) return;
                drawSelector();
            }
        }

        if (changed) drawSelector();
        delayMS(80);
    }
}

// ─── EMULATOR ────────────────────────────────────────────────

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

void IRAM_ATTR systemDrawScreen(void) {
    frameDrawn = 1;
    xSemaphoreTake(fbDone, portMAX_DELAY);
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
    int tmp  = fbFront;
    fbFront  = fbBack;
    fbBack   = tmp;
    xSemaphoreGive(fbReady);
}

void systemOnWriteDataToSoundBuffer(int16_t *finalWave, int length) {}

static void allocBuffers() {
    #define PSRAM_ALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    #define IRAM_ALLOC(size)  heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

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
    printf("Free internal:%lu Free PSRAM:%lu\n",
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
    memset(FB[0], 0, 240 * 160 * 2);
    memset(FB[1], 0, 240 * 160 * 2);

    // Clear screen
    uint16_t *clearBuf = (uint16_t*)PSRAM_ALLOC(LCD_W * LCD_H * 2);
    if (clearBuf) {
        memset(clearBuf, 0, LCD_W * LCD_H * 2);
        lcdSetWindow(0, 0, LCD_W - 1, LCD_H - 1);
        lcdWriteFB((uint8_t*)clearBuf, LCD_W * LCD_H * 2);
        lcdFlushDMA();
        free(clearBuf);
    }

    // Show file selector and wait for ROM selection
    runSelector();

    // ROM is now in romBuf — set global rom pointer
    rom = (uint8_t*)romBuf;

    // Setup dual core display
    fbReady = xSemaphoreCreateBinary();
    fbDone  = xSemaphoreCreateBinary();
    xSemaphoreGive(fbDone);
    xTaskCreatePinnedToCore(
        displayTask, "display",
        4096, NULL,
        configMAX_PRIORITIES - 1,
        NULL, 1);

    printf("Starting emulator...\n");

    emuInit();
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

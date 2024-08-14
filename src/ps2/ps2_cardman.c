#include "ps2_cardman.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "card_emu/ps2_mc_data_interface.h"
#include "card_emu/ps2_sd2psxman.h"
#include "debug.h"
#include "game_db/game_db.h"
#include "hardware/timer.h"
#include "pico/multicore.h"
#if WITH_PSRAM
#include "ps2_dirty.h"
#include "psram/psram.h"
#endif
#include "sd.h"
#include "settings.h"

#define BLOCK_SIZE            (512)
#define SECTOR_COUNT          (PS2_CARD_SIZE_8M / BLOCK_SIZE)

#if WITH_PSRAM
uint8_t available_sectors[SECTOR_COUNT / 8]; // bitmap
#endif
static uint8_t flushbuf[BLOCK_SIZE];
static int fd = -1;
int current_read_sector = 0, priority_sector = -1;

#define MAX_GAME_NAME_LENGTH (127)
#define MAX_PREFIX_LENGTH    (4)
#define MAX_SLICE_LENGTH     ( 10 * 1000 )

static int card_idx;
static int card_chan;
static bool needs_update;
static uint32_t card_size;
static cardman_cb_t cardman_cb;
static char folder_name[MAX_GAME_ID_LENGTH];
static uint64_t cardprog_start;
static int cardman_sectors_done;
static size_t cardprog_pos;

static ps2_cardman_state_t cardman_state = PS2_CM_STATE_CLOSED;

static enum {
    CARDMAN_CREATE,
    CARDMAN_OPEN,
    CARDMAN_IDLE
} cardman_operation;


int ps2_cardman_read_sector(int sector, void *buf512) {
    if (fd < 0)
        return -1;

    if (sd_seek(fd, sector * BLOCK_SIZE) != 0)
        return -1;

    if (sd_read(fd, buf512, BLOCK_SIZE) != BLOCK_SIZE)
        return -1;

    return 0;
}

int ps2_cardman_write_sector(int sector, void *buf512) {
    if (fd < 0)
        return -1;

    if (sd_seek(fd, sector * BLOCK_SIZE) != 0)
        return -1;

    if (sd_write(fd, buf512, BLOCK_SIZE) != BLOCK_SIZE)
        return -1;

    return 0;
}

bool ps2_cardman_is_sector_available(int sector) {
#if WITH_PSRAM
    return available_sectors[sector / 8] & (1 << (sector % 8));
#else
    return true;
#endif
}

void ps2_cardman_mark_sector_available(int sector) {
#if WITH_PSRAM
    available_sectors[sector / 8] |= (1 << (sector % 8));
#endif
}

void ps2_cardman_set_priority_sector(int sector) {
    priority_sector = sector;
}

void ps2_cardman_flush(void) {
    if (fd >= 0)
        sd_flush(fd);
}

static void ensuredirs(void) {
    char cardpath[32];

    snprintf(cardpath, sizeof(cardpath), "MemoryCards/PS2/%s", folder_name);

    sd_mkdir("MemoryCards");
    sd_mkdir("MemoryCards/PS2");
    sd_mkdir(cardpath);
    
    if (!sd_exists("MemoryCards") || !sd_exists("MemoryCards/PS2") || !sd_exists(cardpath))
        fatal("error creating directories");
}

static const uint8_t block0[384] = {
    0x53, 0x6F, 0x6E, 0x79, 0x20, 0x50, 0x53, 0x32, 0x20, 0x4D, 0x65, 0x6D, 0x6F, 0x72, 0x79, 0x20, 0x43, 0x61, 0x72, 0x64, 0x20, 0x46, 0x6F, 0x72, 0x6D, 0x61,
    0x74, 0x20, 0x31, 0x2E, 0x32, 0x2E, 0x30, 0x2E, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x10, 0x00, 0x00, 0xFF, 0x00, 0x20, 0x00, 0x00,
    0x49, 0x00, 0x00, 0x00, 0xC7, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x03, 0x00, 0x00, 0xFE, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x2B,
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x41, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};

static const uint8_t block2000[128] = {
    0x09, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x0D, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x0F, 0x00,
    0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00,
    0x16, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x1B, 0x00, 0x00, 0x00, 0x1C, 0x00,
    0x00, 0x00, 0x1D, 0x00, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00,
    0x23, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00, 0x26, 0x00, 0x00, 0x00, 0x27, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00};

static const uint8_t blockA400[512] = {
    0x27, 0x84, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x31, 0x0C, 0x18, 0x0A, 0xE6, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F,
    0x31, 0x0C, 0x18, 0x0A, 0xE6, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const uint8_t blockA600[512] = {
    0x26, 0xA4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x31, 0x0C, 0x18, 0x0A, 0xE6, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F,
    0x31, 0x0C, 0x18, 0x0A, 0xE6, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2E, 0x2E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};


static void genblock(size_t pos, void *vbuf) {
    uint8_t *buf = vbuf;
    
    #define CARD_SIZE_MB    ( card_size / (1024 * 1024) )

    memset(buf, 0xFF, BLOCK_SIZE);

    if (pos == 0) {
        // 0x30: Clusters Total (2 Bytes): card_size / 1024
        // 0x34: Alloc start: 0x49
        // 0x38: Alloc end: ((((card_size / 8) / 1024) - 2) * 8) - 41
        // 0x40: BBlock 1 - ((card_size / 8) / 1024) - 1
        // 0x44: BBlock 2 - ((card_size / 8) / 1024) - 2
        memcpy(buf, block0, sizeof(block0));
        (*(uint16_t*)&buf[0x30]) = (uint16_t)(card_size / 1024);                        // Total cluster
        (*(uint32_t*)&buf[0x34]) = 0x49;   // Alloc Start
        (*(uint32_t*)&buf[0x38]) = (uint32_t)((((card_size / 8) / 1024) - 2) * 8) - 0x49;   // Alloc End
        (*(uint32_t*)&buf[0x40]) = (uint32_t)(((card_size / 8) / 1024) - 1);
        (*(uint32_t*)&buf[0x44]) = (uint32_t)(((card_size / 8) / 1024) - 2);
    } else if (pos == 0x2000) {
        // Indirect FAT
        uint8_t byte = 0x09;
        int32_t count = ( CARD_SIZE_MB * 16 ) % PS2_PAGE_SIZE;
        for (int i = 0; i < count; i++) {
            if (i % 4 == 0) {
                buf[i] = byte++;
            } else {
                buf[i] = 0;
            }
        }
    } else if ((pos == 0x2200) && (CARD_SIZE_MB > 32)) {
        // Indirect FAT
        uint8_t byte = 0x49;
        int32_t count = (16 * (CARD_SIZE_MB - 32)) % PS2_PAGE_SIZE;
        for (int i = 0; i < count; i++) {
            if (i % 4 == 0) {
                buf[i] = byte++;
            } else {
                buf[i] = 0;
            }
        }
       // memcpy(buf, block2000, sizeof(block2000));
    //} else if (pos >= 0x2400 && pos < 0xA400) {
    } else if (pos >= 0x2400 && pos < 0x12400) {
        // FAT Table
        for (size_t i = 0; i < BLOCK_SIZE / 4; ++i) {
            uint32_t val = 0x7FFFFFFF;
            memcpy(&buf[i * 4], &val, sizeof(val));
        }
        if (pos == 0x2400) {

            buf[0] = buf[1] = buf[2] = buf[3] = 0xFF;
        }
//        if (pos == 0xA200) {
        if (pos == 0x12200) {
            // ???
            //memset(buf + 0x11C, 0xFF, BLOCK_SIZE - 0x11C);
            memset(buf + 0x9C, 0xFF, BLOCK_SIZE - 0x9C);
        }
//    } else if (pos == 0xA400) {
      } else if (pos == 0x12400) {
        // Allocatable Clusters are here
        memcpy(buf, blockA400, sizeof(blockA400));
//    } else if (pos == 0xA600) {
    } else if (pos == 0x12600) {
        memcpy(buf, blockA600, sizeof(blockA600));
    }
}

static int next_sector_to_load() {
    if (priority_sector != -1) {
        if (ps2_cardman_is_sector_available(priority_sector))
            priority_sector = -1;
        else
            return priority_sector;
    }

    while (current_read_sector < SECTOR_COUNT) {
        if (!ps2_cardman_is_sector_available(current_read_sector))
            return current_read_sector++;
        else
            current_read_sector++;
    }

    return -1;
}

static void ps2_cardman_continue(void) {
    if (cardman_operation == CARDMAN_OPEN) {
        uint64_t slice_start = time_us_64();
        if (settings_get_sd_mode() || card_size > PS2_CARD_SIZE_8M) {
            uint64_t end = time_us_64();
            printf("took = %.2f s; SD read speed = %.2f kB/s\n", (end - cardprog_start) / 1e6, 1000000.0 * card_size / (end - cardprog_start) / 1024);
            ps2_mc_data_interface_card_changed();
            cardman_cb(100, true);
            cardman_operation = CARDMAN_IDLE;
        } else {
#if WITH_PSRAM
            while (time_us_64() - slice_start < MAX_SLICE_LENGTH) {
                ps2_dirty_lock();
                int sector_idx = next_sector_to_load();
                if (sector_idx == -1) {
                    ps2_dirty_unlock();
                    cardman_operation = CARDMAN_IDLE;
                    uint64_t end = time_us_64();
                    printf("OK!\n");

                    printf("took = %.2f s; SD read speed = %.2f kB/s\n", (end - cardprog_start) / 1e6, 1000000.0 * card_size / (end - cardprog_start) / 1024);
                    ps2_mc_data_interface_card_changed();
                    cardman_cb(100, true);
                    break;
                }

                size_t pos = sector_idx * BLOCK_SIZE;
                if (sd_seek(fd, pos) != 0)
                    fatal("cannot read memcard\nseek");

                if (sd_read(fd, flushbuf, BLOCK_SIZE) != BLOCK_SIZE)
                    fatal("cannot read memcard\nread %u", pos);

                psram_write_dma(pos, flushbuf, BLOCK_SIZE, NULL);
                psram_wait_for_dma();
                ps2_cardman_mark_sector_available(sector_idx);
                ps2_dirty_unlock();

                cardprog_pos = cardman_sectors_done * BLOCK_SIZE;

                if (cardman_cb)
                    cardman_cb(100 * cardprog_pos / card_size, false);

                cardman_sectors_done++;
            }
#endif
        }
    } else if (cardman_operation == CARDMAN_CREATE) {
        uint64_t slice_start = time_us_64();
        while (time_us_64() - slice_start < MAX_SLICE_LENGTH) {
            cardprog_pos = cardman_sectors_done * BLOCK_SIZE;
            if (cardprog_pos >= card_size) {
                sd_flush(fd);
                printf("OK!\n");
                ps2_history_tracker_format();
                cardman_operation = CARDMAN_IDLE;
                uint64_t end = time_us_64();
                printf("took = %.2f s; SD write speed = %.2f kB/s\n", (end - cardprog_start) / 1e6, 1000000.0 * card_size / (end - cardprog_start) / 1024);
                if (cardman_cb) 
                    cardman_cb(100, true);

                break;
            }
            if (settings_get_sd_mode() || (settings_get_ps2_cardsize() > 8)) {
                //if (settings_get_ps2_cardsize() == 8)
                    genblock(cardprog_pos, flushbuf);
                //else
                //    memset(flushbuf, 0xFF, BLOCK_SIZE);
//                genblock(cardprog_pos, flushbuf);
                sd_write(fd, flushbuf, BLOCK_SIZE);
               // QPRINTF("%s writing pos %u\n", __func__, cardprog_pos);

                //ps2_cardman_write_sector(cardman_sectors_done, flushbuf);
            } else {
#if WITH_PSRAM
                ps2_dirty_lock();
                // read back from PSRAM to make sure to retain already rewritten sectors, if any
                psram_read_dma(cardprog_pos, flushbuf, BLOCK_SIZE, NULL);
                psram_wait_for_dma();

                if (sd_write(fd, flushbuf, BLOCK_SIZE) != BLOCK_SIZE)
                    fatal("cannot init memcard");

                ps2_dirty_unlock();
#endif
            }

            if (cardman_cb)
                cardman_cb(100 * cardprog_pos / card_size, cardman_state == CARDMAN_IDLE);

            cardman_sectors_done++;
        }
        
    } else if (cardman_cb) {
        cardman_cb(100, true);
    }
}

void ps2_cardman_open(void) {
    char path[64];

    needs_update = false;

    sd_init();
    ensuredirs();

    if (PS2_CM_STATE_BOOT == cardman_state)
        snprintf(path, sizeof(path), "MemoryCards/PS2/%s/BootCard.mcd", folder_name);
    else
        snprintf(path, sizeof(path), "MemoryCards/PS2/%s/%s-%d.mcd", folder_name, folder_name, card_chan);

    if (card_idx != PS2_CARD_IDX_SPECIAL) {
        /* this is ok to do on every boot because it wouldn't update if the value is the same as currently stored */
        settings_set_ps2_card(card_idx);
        settings_set_ps2_channel(card_chan);
    }

    printf("Switching to card path = %s\n", path);

    if (!sd_exists(path)) {
        card_size = settings_get_ps2_cardsize() * 1024 * 1024;
        cardman_operation = CARDMAN_CREATE;
        fd = sd_open(path, O_RDWR | O_CREAT | O_TRUNC);
        cardman_sectors_done = 0;
        cardprog_pos = 0;
        if (card_size > PS2_CARD_SIZE_8M) {
            ps2_mc_data_interface_set_sdmode(true);
        } else {
            ps2_mc_data_interface_set_sdmode(settings_get_sd_mode());
        }

        if (fd < 0)
            fatal("cannot open for creating new card");

        printf("create new image at %s... ", path);
        cardprog_start = time_us_64();

    #if WITH_PSRAM
        if (!settings_get_sd_mode() || card_size <= PS2_CARD_SIZE_8M) {
            // quickly generate and write an empty card into PSRAM so that it's immediately available, takes about ~0.6s
            for (size_t pos = 0; pos < card_size; pos += BLOCK_SIZE) {
                if (card_size == PS2_CARD_SIZE_8M)
                    genblock(pos, flushbuf);
                else
                    memset(flushbuf, 0xFF, BLOCK_SIZE);

                ps2_dirty_lock();
                psram_write_dma(pos, flushbuf, BLOCK_SIZE, NULL);

                psram_wait_for_dma();
                ps2_cardman_mark_sector_available(pos / BLOCK_SIZE);
                ps2_dirty_unlock();
            }
        }
    #endif
        if (cardman_cb)
            cardman_cb(0, false);
        
    } else {
        fd = sd_open(path, O_RDWR);
        card_size = sd_filesize(fd);
        cardman_operation = CARDMAN_OPEN;
        cardprog_pos = 0;
        cardman_sectors_done = 0;

        if (fd < 0)
            fatal("cannot open card");

        switch (card_size) {
            case PS2_CARD_SIZE_512K:
            case PS2_CARD_SIZE_1M:
            case PS2_CARD_SIZE_2M:
            case PS2_CARD_SIZE_4M:
            case PS2_CARD_SIZE_8M:
                ps2_mc_data_interface_set_sdmode(settings_get_sd_mode());
                break;
            case PS2_CARD_SIZE_16M:
            case PS2_CARD_SIZE_32M:
            case PS2_CARD_SIZE_64M:
                ps2_mc_data_interface_set_sdmode(true);
                break;
            default:
                fatal("Card %d Channel %d is corrupted", card_idx, card_chan);
                break;
        }

        /* read 8 megs of card image */
        printf("reading card (%lu KB).... ", (uint32_t)(card_size / 1024));
        cardprog_start = time_us_64();
        if (cardman_cb)
            cardman_cb(0, false);
    }
    QPRINTF("Open Finished!\n");
}

void ps2_cardman_close(void) {
    if (fd < 0)
        return;
    ps2_cardman_flush();
    sd_close(fd);
    fd = -1;
    current_read_sector = 0;
    priority_sector = -1;
#if WITH_PSRAM
    memset(available_sectors, 0, sizeof(available_sectors));
#endif
}

void ps2_cardman_set_channel(uint16_t chan_num) {
    if (chan_num != card_chan)
        needs_update = true;
    if ((PS2_CM_STATE_NORMAL == cardman_state) || (PS2_CM_STATE_GAMEID == cardman_state)) {
        if (chan_num <= CHAN_MAX && chan_num >= CHAN_MIN) {
            card_chan = chan_num;
        }
    } else {
        card_idx = settings_get_ps2_card();
        card_chan = settings_get_ps2_channel();
    }
    snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
}

void ps2_cardman_next_channel(void) {
    if ((PS2_CM_STATE_NORMAL == cardman_state) || (PS2_CM_STATE_GAMEID == cardman_state)) {
        card_chan += 1;
        if (card_chan > CHAN_MAX)
            card_chan = CHAN_MIN;
    } else {
        card_idx = settings_get_ps2_card();
        card_chan = settings_get_ps2_channel();
        cardman_state = PS2_CM_STATE_NORMAL;
        snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
    }
    needs_update = true;
}

void ps2_cardman_prev_channel(void) {
    if ((PS2_CM_STATE_NORMAL == cardman_state) || (PS2_CM_STATE_GAMEID == cardman_state)) {
        card_chan -= 1;
        if (card_chan < CHAN_MIN)
            card_chan = CHAN_MAX;
    } else {
        card_idx = settings_get_ps2_card();
        card_chan = settings_get_ps2_channel();
        cardman_state = PS2_CM_STATE_NORMAL;
        snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
    }
    needs_update = true;
}

void ps2_cardman_set_idx(uint16_t idx_num) {
    if (idx_num != card_idx)
        needs_update = true;
    if ((idx_num >= IDX_MIN) && (idx_num <= UINT16_MAX)) {
        card_idx = idx_num;
        card_chan = CHAN_MIN;
    }
    snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
}

static void ps2_cardman_special_idx(int newIndx) {
    char parent_id[MAX_GAME_ID_LENGTH] = { 0x00 };
    if (settings_get_ps2_game_id())
        (void)game_db_get_current_parent(parent_id);

    DPRINTF("Parent ID is %s, State is %i, new Index: %i\n", parent_id, cardman_state, newIndx);
    if (PS2_CM_STATE_NORMAL == cardman_state) {
        if (parent_id[0]) {
            card_idx = PS2_CARD_IDX_SPECIAL;
            cardman_state = PS2_CM_STATE_GAMEID;
            card_chan = CHAN_MIN;
            snprintf(folder_name, sizeof(folder_name), "%s", parent_id);
        } else if (settings_get_ps2_autoboot()) {
            card_idx = PS2_CARD_IDX_SPECIAL;
            cardman_state = PS2_CM_STATE_BOOT;
            card_chan = CHAN_MIN;
            snprintf(folder_name, sizeof(folder_name), "BOOT");
        } else {
            cardman_state = PS2_CM_STATE_NORMAL;
            card_idx = IDX_MIN;
            snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
        }
    } else if (PS2_CM_STATE_BOOT == cardman_state) {
        if ((newIndx > PS2_CARD_IDX_SPECIAL) && (parent_id[0])) {
            card_idx = PS2_CARD_IDX_SPECIAL;
            cardman_state = PS2_CM_STATE_GAMEID;
            card_chan = CHAN_MIN;
            snprintf(folder_name, sizeof(folder_name), "%s", parent_id);
        } else {
            card_idx = settings_get_ps2_card();
            card_chan = settings_get_ps2_channel();
            cardman_state = PS2_CM_STATE_NORMAL;
            snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
        }
    } else if (PS2_CM_STATE_GAMEID == cardman_state) {
        if ((newIndx < PS2_CARD_IDX_SPECIAL) && (settings_get_ps2_autoboot())) {
            // Prev Pressed and Boot available
            card_idx = PS2_CARD_IDX_SPECIAL;
            cardman_state = PS2_CM_STATE_BOOT;
            card_chan = CHAN_MIN;
            snprintf(folder_name, sizeof(folder_name), "BOOT");
        } else {
            card_idx = settings_get_ps2_card();
            card_chan = settings_get_ps2_channel();
            cardman_state = PS2_CM_STATE_NORMAL;
            snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
        }
    }
}

void ps2_cardman_next_idx(void) {
    int newIdx = card_idx + 1;
    if (PS2_CM_STATE_NORMAL != cardman_state) {
        ps2_cardman_special_idx(newIdx);
    } else {
        card_idx = (newIdx > (int)UINT16_MAX) ? UINT16_MAX : newIdx;
        card_chan = CHAN_MIN;
        snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
    }
    needs_update = true;
}

void ps2_cardman_prev_idx(void) {
    int newIdx = card_idx - 1;
    if ((PS2_CM_STATE_NORMAL != cardman_state) || (PS2_CARD_IDX_SPECIAL == newIdx)) {
        ps2_cardman_special_idx(newIdx);
    } else {
        card_idx = newIdx;
        card_chan = CHAN_MIN;
        snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
    }
    needs_update = true;
}

int ps2_cardman_get_idx(void) {    
    return (cardman_state == PS2_CM_STATE_NORMAL) ? card_idx : PS2_CARD_IDX_SPECIAL;
}

int ps2_cardman_get_channel(void) {
    return card_chan;
}

void ps2_cardman_set_gameid(const char *const card_game_id) {
    if (!settings_get_ps2_game_id())
        return;

    char new_folder_name[MAX_GAME_ID_LENGTH];
    if (card_game_id[0]) {
        snprintf(new_folder_name, sizeof(new_folder_name), "%s", card_game_id);
        if ((strcmp(new_folder_name, folder_name) != 0) 
            || (PS2_CM_STATE_GAMEID != cardman_state)){
            card_idx = PS2_CARD_IDX_SPECIAL;
            cardman_state = PS2_CM_STATE_GAMEID;
            card_chan = CHAN_MIN;
            snprintf(folder_name, sizeof(folder_name), "%s", card_game_id);
            needs_update = true;
        }
    }
}

void ps2_cardman_set_progress_cb(cardman_cb_t func) {
    cardman_cb = func;
}

char *ps2_cardman_get_progress_text(void) {
    static char progress[32];

    if (cardman_operation != CARDMAN_IDLE)
        snprintf(progress, sizeof(progress), "%s %.2f kB/s", cardman_operation == CARDMAN_CREATE ? "Wr" : "Rd", 1000000.0 * cardprog_pos / (time_us_64() - cardprog_start) / 1024);
    else
        snprintf(progress, sizeof(progress), "Switching...");

    return progress;
}

uint32_t ps2_cardman_get_card_size(void) {
    return card_size;
}

const char *ps2_cardman_get_folder_name(void) {
    return folder_name;
}

ps2_cardman_state_t ps2_cardman_get_state(void) {
    return cardman_state;
}

bool ps2_cardman_needs_update(void) {
    return needs_update;
}

bool ps2_cardman_is_accessible(void) {
    // SD: X IDLE   => X
    // SD: X CREATE => /
    // SD: X OPEN =>   /
    // SD: / IDLE   => X
    // SD: / CREATE => X
    // SD: / OPEN   => X
    if ((card_size > PS2_CARD_SIZE_8M) || (settings_get_sd_mode()) )
        return (cardman_operation == CARDMAN_IDLE);
    else
        return true;
}


void ps2_cardman_init(void) {
    if (settings_get_ps2_autoboot()) {
        card_idx = PS2_CARD_IDX_SPECIAL;
        cardman_state = PS2_CM_STATE_BOOT;
        card_chan = CHAN_MIN;
        snprintf(folder_name, sizeof(folder_name), "BOOT");
    } else {
        card_idx = settings_get_ps2_card();
        card_chan = settings_get_ps2_channel();
        cardman_state = PS2_CM_STATE_NORMAL;
        snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
    }
    cardman_operation = CARDMAN_IDLE;
}

void ps2_cardman_task(void) {
    ps2_cardman_continue();
}

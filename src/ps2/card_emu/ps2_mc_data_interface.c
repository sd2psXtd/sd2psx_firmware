#include <sd.h>
#include <settings.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hardware/timer.h"
#include "history_tracker/ps2_history_tracker.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#include "pico/time.h"
#include "ps2_mc_data_interface.h"
#include "bigmem.h"
#if WITH_PSRAM
#include "psram.h"
#include "ps2_dirty.h"
#endif
#include "ps2_mc_internal.h"
#include "ps2_cardman.h"

#include "debug.h"


//#define DPRINTF(fmt, x...) //printf(fmt, ##x)
#define TPRINTF(fmt, x...) //printf(fmt, ##x)

#define ERASE_CACHE     2
#define WRITE_CACHE     ( ERASE_CACHE * ERASE_SECTORS )
#define READ_CACHE      3

#define PAGE_CACHE_SIZE ( WRITE_CACHE + ERASE_CACHE + READ_CACHE )
#define MAX_READ_AHEAD  1

#define MAX_TIME_SLICE  ( 3 * 1000 )

static bool dma_in_progress = false;

static ps2_mcdi_page_t      pages[PAGE_CACHE_SIZE];
static ps2_mcdi_page_t*     ops[PAGE_CACHE_SIZE];
static bool                 queue_full = false;
static int                  ops_head = 0;
static int                  ops_tail = 0;
static ps2_mcdi_page_t*     prev_read_setup;
static bool                 sdmode;
static bool                 write_occured;
static bool                 busy_cycle;


static uint8_t erase_count = 0;
static uint8_t write_count = 0;
static uint8_t read_count  = 0;

static inline void __time_critical_func(push_op)(ps2_mcdi_page_t* op) {
    while (queue_full) {TPRINTF(".");};
    ops[ops_head] = op;
    ops_head = ( ops_head + 1 ) % PAGE_CACHE_SIZE;
    queue_full = (ops_head == ops_tail);
    switch (op->page_state) {
        case PAGE_READ_REQ:
        case PAGE_READ_AHEAD_REQ:
            read_count++;
            break;
        case PAGE_WRITE_REQ:
            write_count++;
            break;
        case PAGE_ERASE_REQ:
            erase_count++;
            break;
        default:
        break;
    }
}

static inline ps2_mcdi_page_t* __time_critical_func(pop_op)(void) {
    ps2_mcdi_page_t* ptr = ops[ops_tail];
    ops_tail = (ops_tail + 1) % PAGE_CACHE_SIZE;
    queue_full = false;
    return ptr;
}

static inline int __time_critical_func(op_fill_status)(void) {
    return (ops_head - ops_tail + PAGE_CACHE_SIZE) % PAGE_CACHE_SIZE;
}

static inline ps2_mcdi_page_t* __time_critical_func(ps2_mc_data_interface_find_page)(uint32_t page, bool read) {
    ps2_mcdi_page_t* page_p = NULL;
    for (int i = 0; i < PAGE_CACHE_SIZE; i++) {
        if (pages[i].page == page) {
            if ( ( pages[i].page_state != PAGE_EMPTY ) 
                && ((read && (
                    (pages[i].page_state == PAGE_READ_REQ)
                    || (pages[i].page_state == PAGE_READ_AHEAD_REQ)
                    || (pages[i].page_state == PAGE_DATA_AVAILABLE)
                    || (pages[i].page_state == PAGE_READ_AHEAD_AVAILABLE)))
                || (!read && 
                    ((pages[i].page_state == PAGE_WRITE_REQ)
                    || (pages[i].page_state == PAGE_ERASE_REQ))))
                ) {
                page_p = &pages[i];
                break;
            }
        }
    }

    return page_p;
}

static inline ps2_mcdi_page_t* __time_critical_func(ps2_mc_data_interface_find_slot)(void) {
    int i = 0;
    ps2_mcdi_page_t* page = NULL;
    while (!page) {
        if (pages[i].page_state == PAGE_EMPTY) {
            page = &pages[i];
            break;
        }
        i++;
        if (i == PAGE_CACHE_SIZE)
            TPRINTF("%s:Page Cache full\n", __func__);
        i = i % PAGE_CACHE_SIZE;
    }
    return page;
}

static void ps2_mc_data_interface_set_page(ps2_mcdi_page_t* page, uint32_t addr, int state) {
    multicore_lockout_start_blocking();
    page->page = addr;
    page->page_state = state;
    multicore_lockout_end_blocking();
}


#if WITH_PSRAM
static void __time_critical_func(ps2_mc_data_interface_rx_done)() {
    dma_in_progress = false;
    
    ps2_dirty_unlock();
}

void __time_critical_func(ps2_mc_data_interface_start_dma)(ps2_mcdi_page_t* page_p) {
    psram_wait_for_dma();
    ps2_dirty_lockout_renew();
    /* the spinlock will be unlocked by the DMA irq once all data is tx'd */
    ps2_dirty_lock();
    dma_in_progress = true;
    page_p->page_state = PAGE_DATA_AVAILABLE;
    psram_read_dma(page_p->page * PS2_PAGE_SIZE, page_p->data, PS2_PAGE_SIZE, ps2_mc_data_interface_rx_done);
    DPRINTF("C%u: %s start dma %lu\n", get_core_num(), __func__, page_p->page);
    busy_cycle = true;
}
#endif



void __time_critical_func(ps2_mc_data_interface_setup_read_page)(uint32_t page, bool readahead, bool wait) {

    if (page * PS2_PAGE_SIZE + PS2_PAGE_SIZE <= ps2_cardman_get_card_size()) {

        if (sdmode) {
            
            DPRINTF("C%u: %s got page %u\n", get_core_num(), __func__, page);

            if (read_count > READ_CACHE) {
                TPRINTF("!R!\n");
                while (read_count > READ_CACHE) {}
            }
            
            ps2_mcdi_page_t* page_p = ps2_mc_data_interface_find_page(page, true);
            if (!page_p) {
                page_p = ps2_mc_data_interface_find_slot();
                page_p->page = page;
                page_p->page_state = PAGE_READ_REQ;
                if (get_core_num() == 1) {
                    push_op(page_p);
                } else {
                    read_count++;
                }
                
                DPRINTF("C%u: %s setting up read %u\n", get_core_num(), __func__, page);
            } else if (page_p->page_state == PAGE_READ_AHEAD_AVAILABLE) {
                page_p->page_state = PAGE_DATA_AVAILABLE;
                DPRINTF("%s Hit ReadAhead\n", __func__);
            } else {
                DPRINTF("%s found %u for %u \n", page_p->page_state, page_p->page);
            }

            if (readahead) {
                for (int i = 1; (i < MAX_READ_AHEAD) && ((page + i) * PS2_PAGE_SIZE + PS2_PAGE_SIZE <= ps2_cardman_get_card_size()); i++) {
                    ps2_mcdi_page_t* next_p = ps2_mc_data_interface_find_page(page + i, true);
                    if (!next_p) {
                        ps2_mcdi_page_t* next_p = ps2_mc_data_interface_find_slot();
                        
                        next_p->page = page + i;
                        next_p->page_state = PAGE_READ_AHEAD_REQ;
                        push_op(next_p);
                    }
                }

            }

            if (get_core_num() == 1) {
                DPRINTF("%s Waiting page %u - State: %u\n", __func__, page, page_p->page_state);
                while (wait && (page_p->page_state != PAGE_DATA_AVAILABLE) && (page_p->page_state != PAGE_READ_AHEAD_AVAILABLE)) { sleep_us(1);};
            } else {
                if ((page_p->page_state == PAGE_READ_AHEAD_REQ) 
                    || (page_p->page_state == PAGE_READ_REQ)) {
                    ps2_cardman_read_sector(page_p->page, page_p->data);
                    page_p->page_state = PAGE_DATA_AVAILABLE;
                }
            }
            prev_read_setup = page_p;
            
        } else {

#if WITH_PSRAM
            
            ps2_mcdi_page_t* page_p = ps2_mc_data_interface_find_page(page, true);
            if (!page_p) {
                page_p = ps2_mc_data_interface_find_slot();
                page_p->page = page;
                page_p->page_state = PAGE_READ_REQ;
                
                DPRINTF("%s setting up read %u\n", __func__, page);
            } else  {
                DPRINTF("%s found %u for %u \n", page_p->page_state, page_p->page);
            }

            DPRINTF("%s Waiting page %u - State: %u\n", __func__, page, page_p->page_state);

            prev_read_setup = page_p;

            page_p->page = page;
            page_p->page_state = PAGE_READ_REQ;
            if (!ps2_cardman_is_sector_available(page)) {
                ps2_cardman_set_priority_sector(page);
            } else {
                ps2_mc_data_interface_start_dma(page_p);
            }
#endif
        }
    }
}

ps2_mcdi_page_t* __time_critical_func(ps2_mc_data_interface_get_page)(uint32_t page) {
    ps2_mcdi_page_t* ret = NULL;

    if (!prev_read_setup || prev_read_setup->page_state == PAGE_EMPTY)
        ret = ps2_mc_data_interface_find_page(page, true);
    else
        ret = prev_read_setup;
    if (sdmode) {
        if (!ret || ret->page_state == PAGE_EMPTY) {
            TPRINTF("Miss ???\n");
            ps2_mc_data_interface_setup_read_page(page, false, true);
            ret = ps2_mc_data_interface_find_page(page, true);
        }

        if (ret->page_state == PAGE_READ_AHEAD_AVAILABLE) {
            ret->page_state = PAGE_DATA_AVAILABLE;
        }
    } else {
#if WITH_PSRAM
        if (!ps2_cardman_is_sector_available(ret->page)) {
            ps2_cardman_set_priority_sector(ret->page);
            while (!ps2_cardman_is_sector_available(ret->page)) {} // wait for core 0 to load the sector into PSRAM
            ps2_mc_data_interface_start_dma(ret);
        }
#endif
    }
    return ret;
}

void __time_critical_func(ps2_mc_data_interface_write_mc)(uint32_t page, void *buf) {
    if (page * PS2_PAGE_SIZE + PS2_PAGE_SIZE <= ps2_cardman_get_card_size()) {

        if (sdmode) {
            if (get_core_num() == 0) {
                ps2_cardman_write_sector(page, buf);
                ps2_cardman_flush();
            } else {
                if (write_count > WRITE_CACHE) {
                    TPRINTF("!W! %u/%u\n", write_count, op_fill_status());
                    while (write_count > WRITE_CACHE) {};
                };

                ps2_mcdi_page_t* slot = ps2_mc_data_interface_find_slot();
                
                memcpy(slot->data, buf, PS2_PAGE_SIZE);
                slot->page = page;
                slot->page_state = PAGE_WRITE_REQ;
                push_op(slot);
            }

            DPRINTF("%s Done\n", __func__);
        } else {
#if WITH_PSRAM
            psram_wait_for_dma();
            ps2_dirty_lockout_renew();
            ps2_dirty_lock();
            psram_write_dma(page * PS2_PAGE_SIZE, buf, PS2_PAGE_SIZE, NULL);
            psram_wait_for_dma();
            ps2_cardman_mark_sector_available(page);
            ps2_dirty_mark(write_sector);
            ps2_dirty_unlock();
            write_occured = true;
        #endif
        }
    }
}

bool __time_critical_func(ps2_mc_data_interface_write_busy)(void) {
    if (sdmode)
        return (write_count > WRITE_CACHE);
    else
        return false;
}


void __time_critical_func(ps2_mc_data_interface_erase)(uint32_t page) {
    if ((page + ERASE_SECTORS) * PS2_PAGE_SIZE <= ps2_cardman_get_card_size()) {

        if (sdmode) {
            if (!ps2_mc_data_interface_find_page(page, false)) {
                
                ps2_mcdi_page_t* slot = ps2_mc_data_interface_find_slot();

                slot->page = page;
                slot->page_state = PAGE_ERASE_REQ;

                push_op(slot);
            }
        } else {
#if WITH_PSRAM
            uint8_t erasebuff[PS2_PAGE_SIZE] = { 0 };
            memset(erasebuff, 0xFF, PS2_PAGE_SIZE);
            ps2_dirty_lockout_renew();
            ps2_dirty_lock();
            for (int i = 0; i < ERASE_SECTORS; ++i) {
                psram_write_dma((page + i) * PS2_PAGE_SIZE, erasebuff, PS2_PAGE_SIZE, NULL);
                psram_wait_for_dma();
                ps2_cardman_mark_sector_available(page + i);
                ps2_dirty_mark(page + i);
            }
            ps2_dirty_unlock();
#endif
            
        }
    }
}


inline void __time_critical_func(ps2_mc_data_interface_wait_for_byte)(uint32_t offset) {
#if WITH_PSRAM
    if (!sdmode) {
        if (offset <= PS2_PAGE_SIZE)
            while (dma_in_progress && (psram_read_dma_remaining() >= (PS2_PAGE_SIZE - offset))) {};
    } else
#endif
    {
        while (prev_read_setup->page_state != PAGE_DATA_AVAILABLE) {};
    }
}

void __time_critical_func (ps2_mc_data_interface_invalidate_read)(uint32_t page) {
    //if (sdmode) {
        if (page * PS2_PAGE_SIZE <= ps2_cardman_get_card_size()) {
            for (int i = 0; i < PAGE_CACHE_SIZE; i++)
                if (((pages[i].page_state == PAGE_DATA_AVAILABLE) 
                    || (pages[i].page_state == PAGE_READ_AHEAD_AVAILABLE))
                    && ((pages[i].page == page)) )
                  {
                    pages[i].page_state = PAGE_EMPTY;
                    pages[i].page = 0;
                    read_count--;
                    DPRINTF("%s Invalidated %u\n", __func__, page);
                }
        }
    //}
}

void __time_critical_func (ps2_mc_data_interface_invalidate_readahead)(void) {
    if (sdmode) {

        for (int i = 0; i < PAGE_CACHE_SIZE; i++)
            if ((pages[i].page_state == PAGE_READ_AHEAD_AVAILABLE) 
                || (pages[i].page_state == PAGE_READ_AHEAD_REQ)  )
            {
                DPRINTF("%s invalidating %u\n", __func__, pages[i].page);

                pages[i].page_state = PAGE_EMPTY;
                pages[i].page = 0;
                read_count--;
            }
        
    }
}



// Core 0
void ps2_mc_data_interface_card_changed(void) {
    //if (sdmode) {
    DPRINTF("Card changed\n");

    for(int i = 0; i < PAGE_CACHE_SIZE; i++) {
        pages[i].page_state = PAGE_EMPTY;
        pages[i].page = 0;
        pages[i].data = &cache[i * PS2_PAGE_SIZE];
        ops[i] = NULL;
    }
//    ps2_mc_data_interface_invalidate_readahead();
    //} else {
    //    pages[0].page_state = PAGE_EMPTY;
    //    pages[0].page = 0;
    //    pages[0].data = cache;
    //}
#if WITH_PSRAM
    ps2_dirty_init();
#endif
    read_count = 0;
    write_count = 0;
    erase_count = 0;
    write_occured = false;

    DPRINTF("Done!\n");
}

bool ps2_mc_data_interface_write_occured(void) {
    return write_occured;
}

bool ps2_mc_data_interface_busy(void) {
    if (sdmode) {
        return busy_cycle;
    } else {
#if WITH_PSRAM
        return (ps2_dirty_activity > 0);
#endif
    }    
}

void ps2_mc_data_interface_set_sdmode(bool mode) {
    sdmode = mode;
}

bool ps2_mc_data_interface_get_sdmode(void) {
    return sdmode;
}

void ps2_mc_data_interface_task(void) {

    write_occured = false;
    if (sdmode) {
        uint64_t time_start = time_us_64();
        static bool flush_req = false;
        busy_cycle = false;
        
        while ((op_fill_status() > 0) && ((time_us_64() - time_start) < MAX_TIME_SLICE) ){
            ps2_mcdi_page_t* page_p = pop_op();
            busy_cycle = true;
            if (page_p)
                switch(page_p->page_state) {
                    case PAGE_READ_REQ:
                        DPRINTF("%s Reading page %u\n", __func__, page_p->page);
                        ps2_cardman_read_sector(page_p->page, page_p->data);
                        ps2_mc_data_interface_set_page(page_p, page_p->page, PAGE_DATA_AVAILABLE);
                        break;
                    case PAGE_READ_AHEAD_REQ:
                        DPRINTF("%s Reading ahead page %u\n", __func__, page_p->page);
                        ps2_cardman_read_sector(page_p->page, page_p->data);
                        ps2_mc_data_interface_set_page(page_p, page_p->page, PAGE_READ_AHEAD_AVAILABLE);
                        break;
                    case PAGE_WRITE_REQ:
                        DPRINTF("%s Writing page %u\n", __func__, page_p->page);
                        write_occured = true;
                        ps2_cardman_write_sector(page_p->page, page_p->data);
                        ps2_history_tracker_registerPageWrite(page_p->page);
                        ps2_mc_data_interface_invalidate_read(page_p->page);
                        ps2_mc_data_interface_set_page(page_p, 0, PAGE_EMPTY);
                        write_count--;
                        flush_req = true;
                        break;
                    case PAGE_ERASE_REQ:
                        TPRINTF("%s Erasing page %u\n", __func__, page_p->page);
                        write_occured = true;
                        uint8_t erase_buff[PS2_PAGE_SIZE] = { 0x0 };
                        memset((void*)erase_buff, 0xFF, PS2_PAGE_SIZE);
                        for (int j = 0; j < ERASE_SECTORS; j++) {
                            ps2_cardman_write_sector(page_p->page + j, erase_buff);
                        }
                        ps2_mc_data_interface_set_page(page_p, 0, PAGE_EMPTY);
                        ps2_mc_data_interface_invalidate_read(page_p->page);
                        ps2_mc_data_interface_invalidate_readahead();
                        erase_count--;
                        flush_req = true;
                        break;
                    default:
                        break;
                }
        }
        if (op_fill_status() > 0)
            TPRINTF("%u left\n", op_fill_status());
        else if (flush_req) {
            ps2_cardman_flush();
            flush_req = false;
        }
    } else {
#if WITH_PSRAM
    ps2_dirty_task();
    busy_cycle = dma_in_progress;
#endif
    }
}
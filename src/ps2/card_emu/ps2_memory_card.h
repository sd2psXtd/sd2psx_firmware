#pragma once

void ps2_memory_card_main(void);
void ps2_memory_card_enter(void);
void ps2_memory_card_exit(void);
void ps2_memory_card_unload(void);

void ps2_memory_card_set_cmd_callback(void (*cb)(void));
void ps2_queue_tx_byte_on_reset(uint8_t byte);

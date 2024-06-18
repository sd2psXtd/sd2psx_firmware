#pragma once
#include <stdint.h>


void ps2_history_tracker_registerPageWrite(uint32_t page);
void ps2_history_tracker_registerRead(void);
void ps2_history_tracker_init(void);
void ps2_history_tracker_run(void);
void ps2_history_tracker_card_changed(void);
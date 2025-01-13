#pragma once

#include <stddef.h>
#include <stdint.h>
#include <pico/platform.h>


#define MAX_GAME_ID_LENGTH   (16)
#define MAX_FOLDER_LENGTH   (250)

void game_db_extract_title_id(const uint8_t* const in_title_id, char* const out_title_id, const size_t in_title_id_length, const size_t out_buffer_size);
bool game_db_sanity_check_title_id(const char* const title_id);


void game_db_get_current_name(char* const game_name);
int game_db_get_current_parent(char* const parent_id);
int game_db_update_game(const char* const game_id);
void game_db_get_game_name(const char* game_id, char* game_name);

void game_db_init(void);
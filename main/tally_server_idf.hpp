#pragma once

#include <cstdint>

void tally_server_start();
void tally_server_set_sources(uint8_t count);
void tally_server_set_flag(uint8_t index, uint8_t flags);
void tally_server_reset_flags();
void tally_server_set_all_flags(uint8_t count, const uint8_t* flags);
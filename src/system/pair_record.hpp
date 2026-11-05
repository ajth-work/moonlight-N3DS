/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#ifndef MOONLIGHT_3DS_PATH
#define MOONLIGHT_3DS_PATH "/3ds/moonlight"
#endif

#ifdef __cplusplus
#include <string>
#include <vector>

void trim(std::string &s);
bool add_saved_host_entry(std::string address, uint16_t port);
bool remove_saved_host_entry(const std::string &entry);
bool replace_saved_host_entry(const std::string &old_entry,
                             const std::string &new_entry);
std::vector<std::string> list_saved_host_entries();
bool pairing_layout_v2_preferred();
bool set_pairing_layout_v2_preferred(bool enabled);
bool save_host_public_key_pin(std::string address, uint16_t port,
                              const char *pin);
bool load_host_public_key_pin(std::string address, uint16_t port,
                              std::string &pin);
void remove_host_public_key_pin(std::string address, uint16_t port);
#endif

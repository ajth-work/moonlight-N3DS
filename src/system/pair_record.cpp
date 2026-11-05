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

#include <algorithm>
#include <cctype>
#include <fstream>
#include <locale>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstdio>
#include <sstream>

#include "pair_record.hpp"
#include "public_key_pin.h"

// trim from start (in place)
inline void ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
}

// trim from end (in place)
inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(),
                         [](unsigned char ch) { return !std::isspace(ch); })
                .base(),
            s.end());
}

// trim from both ends (in place)
void trim(std::string &s) {
    rtrim(s);
    ltrim(s);
}

static std::string paired_address_file_path() {
    return std::string(MOONLIGHT_3DS_PATH) + "/paired";
}

static bool write_paired_addresses(const std::vector<std::string> &addresses) {
    const std::string path = paired_address_file_path();
    const std::string temporary = path + ".tmp";
    FILE *file = fopen(temporary.c_str(), "w");
    if (file == NULL) return false;
    bool ok = true;
    for (const auto &address : addresses) {
        if (fprintf(file, "%s\n", address.c_str()) < 0) {
            ok = false;
            break;
        }
    }
    if (fclose(file) != 0) ok = false;
    if (!ok || rename(temporary.c_str(), path.c_str()) != 0) {
        remove(temporary.c_str());
        return false;
    }
    return true;
}

bool add_saved_host_entry(std::string address, uint16_t port) {
    address += ":" + std::to_string(port);

    // Prevent duplicates
    auto address_list = list_saved_host_entries();
    for (auto entry : address_list) {
        if (entry == address) {
            return true;
        }
    }
    address_list.push_back(address);

    return write_paired_addresses(address_list);
}

bool remove_saved_host_entry(const std::string &entry) {
    if (entry.empty()) return false;
    auto addresses = list_saved_host_entries();
    const size_t old_size = addresses.size();
    addresses.erase(std::remove(addresses.begin(), addresses.end(), entry),
                    addresses.end());
    return addresses.size() != old_size && write_paired_addresses(addresses);
}

bool replace_saved_host_entry(const std::string &old_entry,
                              const std::string &new_entry) {
    if (old_entry.empty() || new_entry.empty()) return false;
    auto addresses = list_saved_host_entries();
    auto old_position = std::find(addresses.begin(), addresses.end(), old_entry);
    if (old_position == addresses.end()) return false;
    if (old_entry == new_entry) return true;
    if (std::find(addresses.begin(), addresses.end(), new_entry) != addresses.end())
        return false;
    *old_position = new_entry;
    return write_paired_addresses(addresses);
}

std::vector<std::string> list_saved_host_entries() {
    std::vector<std::string> addresses = std::vector<std::string>();
    std::ifstream pair_file(paired_address_file_path());
    std::string line;
    while (std::getline(pair_file, line)) {
        trim(line);
        addresses.push_back(line);
    }
    return addresses;
}

bool pairing_layout_v2_preferred() {
    std::ifstream file(std::string(MOONLIGHT_3DS_PATH) + "/pairing-layout");
    int version = 1;
    return (file >> version) && version == 2;
}

bool set_pairing_layout_v2_preferred(bool enabled) {
    const std::string path = std::string(MOONLIGHT_3DS_PATH) + "/pairing-layout";
    const std::string temporary = path + ".tmp";
    FILE *file = fopen(temporary.c_str(), "w");
    if (file == NULL) return false;
    const bool wrote = fprintf(file, "%d\n", enabled ? 2 : 1) >= 0;
    const bool closed = fclose(file) == 0;
    if (!wrote || !closed || rename(temporary.c_str(), path.c_str()) != 0) {
        remove(temporary.c_str());
        return false;
    }
    return true;
}

static std::string pin_file_path() {
    return std::string(MOONLIGHT_3DS_PATH) + "/host-pins";
}

static std::string endpoint_key(std::string address, uint16_t port) {
    if (address.empty() || address.find_first_of(" \t\r\n") != std::string::npos)
        return std::string();
    return address + ":" + std::to_string(port);
}

bool load_host_public_key_pin(std::string address, uint16_t port,
                              std::string &pin) {
    const std::string key = endpoint_key(address, port);
    if (key.empty()) return false;
    std::ifstream file(pin_file_path());
    std::string stored_key, stored_pin;
    while (file >> stored_key >> stored_pin) {
        if (stored_key == key && valid_public_key_pin(stored_pin.c_str())) {
            pin = stored_pin;
            return true;
        }
    }
    return false;
}

bool save_host_public_key_pin(std::string address, uint16_t port,
                              const char *pin) {
    const std::string key = endpoint_key(address, port);
    if (key.empty() || !valid_public_key_pin(pin)) return false;
    std::vector<std::pair<std::string, std::string>> entries;
    std::ifstream input(pin_file_path());
    std::string old_key, old_pin;
    while (input >> old_key >> old_pin) {
        if (old_key != key) entries.emplace_back(old_key, old_pin);
    }
    entries.emplace_back(key, pin);
    const std::string path = pin_file_path();
    const std::string temp = path + ".tmp";
    FILE *file = fopen(temp.c_str(), "w");
    if (file == NULL) return false;
    bool ok = true;
    for (const auto &entry : entries) {
        if (fprintf(file, "%s %s\n", entry.first.c_str(), entry.second.c_str()) < 0)
            ok = false;
    }
    if (fclose(file) != 0) ok = false;
    if (!ok || rename(temp.c_str(), path.c_str()) != 0) {
        remove(temp.c_str());
        return false;
    }
    return true;
}

void remove_host_public_key_pin(std::string address, uint16_t port) {
    const std::string key = endpoint_key(address, port);
    if (key.empty()) return;
    const std::string path = pin_file_path();
    const std::string temp = path + ".tmp";
    std::ifstream input(path);
    FILE *output = fopen(temp.c_str(), "w");
    if (output == NULL) return;
    std::string old_key, old_pin;
    while (input >> old_key >> old_pin) {
        if (old_key != key) fprintf(output, "%s %s\n", old_key.c_str(), old_pin.c_str());
    }
    if (fclose(output) == 0) rename(temp.c_str(), path.c_str());
    else remove(temp.c_str());
}

extern "C" bool host_pin_store_save(const char *address, uint16_t port,
                                     const char *pin) {
    return address != NULL && save_host_public_key_pin(address, port, pin);
}

extern "C" bool host_pin_store_save_after_pairing(const char *address,
                                                    uint16_t port,
                                                    const char *pin,
                                                    bool pairing_succeeded) {
    if (!pairing_succeeded) return false;
    return host_pin_store_save(address, port, pin);
}

extern "C" bool host_pin_store_load(const char *address, uint16_t port,
                                     char *pin, size_t pin_size) {
    std::string value;
    if (address == NULL || pin == NULL || !load_host_public_key_pin(address, port, value) ||
        value.size() + 1 > pin_size) return false;
    memcpy(pin, value.c_str(), value.size() + 1);
    return true;
}

extern "C" void host_pin_store_remove(const char *address, uint16_t port) {
    if (address != NULL) remove_host_public_key_pin(address, port);
}

extern "C" bool host_upload_consent_get(const char *address, uint16_t port,
                                         bool *known, bool *enabled) {
    char pin[53];
    if (known == NULL || enabled == NULL || address == NULL ||
        !host_pin_store_load(address, port, pin, sizeof(pin))) return false;
    *known = false;
    *enabled = false;
    std::ifstream input(pin_file_path() + "-upload-consent");
    std::string stored_pin;
    int stored_value;
    while (input >> stored_pin >> stored_value) {
        if (stored_pin == pin && (stored_value == 0 || stored_value == 1)) {
            *known = true;
            *enabled = stored_value == 1;
            return true;
        }
    }
    return true;
}

extern "C" bool host_upload_consent_set(const char *address, uint16_t port,
                                         bool enabled) {
    char pin[53];
    if (address == NULL || !host_pin_store_load(address, port, pin, sizeof(pin))) return false;
    const std::string path = pin_file_path() + "-upload-consent";
    const std::string temporary = path + ".tmp";
    std::vector<std::pair<std::string, int>> entries;
    std::ifstream input(path);
    std::string old_pin;
    int old_value;
    while (input >> old_pin >> old_value) {
        if (old_pin != pin && (old_value == 0 || old_value == 1))
            entries.emplace_back(old_pin, old_value);
    }
    entries.emplace_back(pin, enabled ? 1 : 0);
    FILE *output = fopen(temporary.c_str(), "w");
    if (output == NULL) return false;
    bool ok = true;
    for (const auto &entry : entries) {
        if (fprintf(output, "%s %d\n", entry.first.c_str(), entry.second) < 0) ok = false;
    }
    if (fclose(output) != 0) ok = false;
    if (!ok || rename(temporary.c_str(), path.c_str()) != 0) {
        remove(temporary.c_str());
        return false;
    }
    return true;
}

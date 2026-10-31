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

void add_pair_address(std::string address, uint16_t port) {
    address += ":" + std::to_string(port);

    // Prevent duplicates
    auto address_list = list_paired_addresses();
    for (auto entry : address_list) {
        if (entry == address) {
            return;
        }
    }
    address_list.push_back(address);

    char *address_file = (char *)MOONLIGHT_3DS_PATH "/paired";
    remove(address_file);

    FILE *fd = fopen(address_file, "w");
    for (auto addr_string : address_list) {
        trim(addr_string);
        fprintf(fd, "%s\n", addr_string.c_str());
    }
    fclose(fd);
}

void remove_pair_address(std::string address, uint16_t port) {
    address += ":" + std::to_string(port);

    auto address_list = list_paired_addresses();

    char *address_file = (char *)MOONLIGHT_3DS_PATH "/paired";
    remove(address_file);

    FILE *fd = fopen(address_file, "w");
    for (auto addr_string : address_list) {
        if (addr_string != address) {
            trim(addr_string);
            fprintf(fd, "%s\n", addr_string.c_str());
        }
    }
    fclose(fd);
}

std::vector<std::string> list_paired_addresses() {
    std::vector<std::string> addresses = std::vector<std::string>();
    std::ifstream pair_file(MOONLIGHT_3DS_PATH "/paired");
    std::string line;
    while (std::getline(pair_file, line)) {
        trim(line);
        addresses.push_back(line);
    }
    return addresses;
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

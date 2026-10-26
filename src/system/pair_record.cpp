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
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#include "pair_record.hpp"

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


std::vector<ServerAddressRecord> list_server_addresses() {
    std::vector<ServerAddressRecord> addresses;
    std::ifstream history_file(MOONLIGHT_3DS_PATH "/servers");
    std::string line;

    while (std::getline(history_file, line)) {
        trim(line);
        if (line.empty()) {
            continue;
        }

        std::size_t separator = line.find('\\t');
        std::string address =
            separator == std::string::npos ? line : line.substr(separator + 1);
        trim(address);
        if (address.empty()) {
            continue;
        }

        bool failed = separator != std::string::npos &&
                      line.substr(0, separator) == "failed";
        bool already_listed = false;
        for (const auto &entry : addresses) {
            if (entry.address == address) {
                already_listed = true;
                break;
            }
        }
        if (!already_listed) {
            addresses.push_back({address, failed});
        }
    }

    // Keep hosts saved by older builds available after introducing history.
    for (auto address : list_paired_addresses()) {
        trim(address);
        if (address.empty()) {
            continue;
        }

        bool already_listed = false;
        for (const auto &entry : addresses) {
            if (entry.address == address) {
                already_listed = true;
                break;
            }
        }
        if (!already_listed) {
            addresses.push_back({address, false});
        }
    }

    return addresses;
}

void remember_server_address(std::string address, uint16_t port, bool failed) {
    address += ":" + std::to_string(port);
    trim(address);

    auto addresses = list_server_addresses();
    bool found = false;
    for (auto &entry : addresses) {
        if (entry.address == address) {
            entry.failed = failed;
            found = true;
            break;
        }
    }
    if (!found) {
        addresses.push_back({address, failed});
    }

    // The address list is needed before a connection attempt finishes, so make
    // sure its parent directory exists even on a first run.
    mkdir("/3ds", 0777);
    mkdir(MOONLIGHT_3DS_PATH, 0777);

    char *address_file = (char *)MOONLIGHT_3DS_PATH "/servers";
    FILE *fd = fopen(address_file, "w");
    if (fd == NULL) {
        perror("Failed to save server address history");
        return;
    }

    for (const auto &entry : addresses) {
        fprintf(fd, "%s\\t%s\\n", entry.failed ? "failed" : "saved",
                entry.address.c_str());
    }
    fclose(fd);
}

/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
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

#include <stdlib.h>

#define CERTIFICATE_FILE_NAME "client.pem"
#define KEY_FILE_NAME "key.pem"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _HTTP_DATA {
    char *memory;
    size_t size;
    long response_code;
} HTTP_DATA, *PHTTP_DATA;

int http_init(const char *keyDirectory, int logLevel);
PHTTP_DATA http_create_data();
int http_request(char *url, PHTTP_DATA data);
int http_request_pinned(char *url, PHTTP_DATA data, const char *public_key_pin);
int http_request_post_pinned(char *url, PHTTP_DATA data, const char *public_key_pin,
                             const char *body, size_t body_size);
void http_cleanup();
void http_free_data(PHTTP_DATA data);
void http_set_timeout_s(uint32_t connection_timeout_in);
void http_set_log_level(int log_level_in);

#ifdef __cplusplus
}
#endif

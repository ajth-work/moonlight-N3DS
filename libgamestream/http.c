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

#include "http.h"
#include "errors.h"
#include "diagnostics.h"

#include <curl/curl.h>
#include <stdbool.h>
#include <string.h>

static CURL *curl;
static uint32_t connection_timeout_s = 60;
static int log_level = 0;

static int _curl_progress(void *clientp, curl_off_t download_total,
                         curl_off_t download_now, curl_off_t upload_total,
                         curl_off_t upload_now) {
    (void)clientp;
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;
    return diag_cancel_requested() ? 1 : 0;
}

static void _append(char *out, size_t capacity, size_t *used,
                    const char *text, size_t length) {
    size_t available = *used < capacity ? capacity - *used : 0;
    if (available <= 1) {
        return;
    }
    if (length > available - 1) {
        length = available - 1;
    }
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = '\0';
}

/* Keep the URL shape and parameter names without persisting credentials,
 * pairing challenges, or client identifiers in the removable-SD log. */
static void _redact_url(const char *url, char *out, size_t capacity) {
    size_t used = 0;
    const char *cursor = url;
    out[0] = '\0';
    while (*cursor != '\0' && *cursor != '?') {
        _append(out, capacity, &used, cursor, 1);
        cursor++;
    }
    if (*cursor != '?') {
        return;
    }
    _append(out, capacity, &used, cursor++, 1);
    while (*cursor != '\0' && used + 1 < capacity) {
        const char *key = cursor;
        while (*cursor != '\0' && *cursor != '=' && *cursor != '&') {
            cursor++;
        }
        _append(out, capacity, &used, key, (size_t)(cursor - key));
        if (*cursor == '=') {
            _append(out, capacity, &used, "=[redacted]", 11);
            cursor++;
            while (*cursor != '\0' && *cursor != '&') {
                cursor++;
            }
        }
        if (*cursor == '&') {
            _append(out, capacity, &used, cursor++, 1);
        }
    }
}

static size_t _write_curl(void *contents, size_t size, size_t nmemb,
                          void *userp) {
    size_t realsize = size * nmemb;
    PHTTP_DATA mem = (PHTTP_DATA)userp;

    mem->memory = realloc(mem->memory, mem->size + realsize + 1);
    if (mem->memory == NULL)
        return 0;

    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

int http_init(const char *keyDirectory, int logLevel) {
    curl = curl_easy_init();
    if (!curl)
        return GS_FAILED;

    char certificateFilePath[4096];
    snprintf(certificateFilePath, sizeof(certificateFilePath), "%s/%s",
             keyDirectory, CERTIFICATE_FILE_NAME);

    char keyFilePath[4096];
    snprintf(keyFilePath, sizeof(keyFilePath), "%s/%s", keyDirectory,
             KEY_FILE_NAME);

    log_level = logLevel;

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLENGINE_DEFAULT, 1L);
    curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
    curl_easy_setopt(curl, CURLOPT_SSLCERT, certificateFilePath);
    curl_easy_setopt(curl, CURLOPT_SSLKEYTYPE, "PEM");
    curl_easy_setopt(curl, CURLOPT_SSLKEY, keyFilePath);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_curl);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, _curl_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_SESSIONID_CACHE, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, connection_timeout_s);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, log_level > 0 ? 1L : 0);

    return GS_OK;
}

void http_set_timeout_s(uint32_t connection_timeout_in) {
    connection_timeout_s = connection_timeout_in;
}

void http_set_log_level(int log_level_in) { log_level = log_level_in; }

int http_request(char *url, PHTTP_DATA data) {
    char safe_url[4096];
    long response_code = 0;
    _redact_url(url, safe_url, sizeof(safe_url));
    diag_http_begin("GET", safe_url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, connection_timeout_s);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, log_level > 0 ? 1L : 0);
#ifdef __FreeBSD__
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1);
#endif

    if (log_level) {
        printf("Request %s\n", url);
    }

    if (data->size > 0) {
        free(data->memory);
        data->memory = malloc(1);
        if (data->memory == NULL) {
            diag_http_end(response_code, GS_OUT_OF_MEMORY);
            return GS_OUT_OF_MEMORY;
        }

        data->size = 0;
    }
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    diag_http_end(response_code, (int)res);

    if (res != CURLE_OK) {
        gs_error = curl_easy_strerror(res);
        return GS_FAILED;
    } else if (data->memory == NULL) {
        return GS_OUT_OF_MEMORY;
    }

    if (log_level) {
        printf("Response:\n%s\n\n", data->memory);
    }

    return GS_OK;
}

void http_cleanup() { curl_easy_cleanup(curl); }

PHTTP_DATA http_create_data() {
    PHTTP_DATA data = malloc(sizeof(HTTP_DATA));
    if (data == NULL)
        return NULL;

    data->memory = malloc(1);
    if (data->memory == NULL) {
        free(data);
        return NULL;
    }
    data->size = 0;

    return data;
}

void http_free_data(PHTTP_DATA data) {
    if (data != NULL) {
        if (data->memory != NULL)
            free(data->memory);

        free(data);
    }
}

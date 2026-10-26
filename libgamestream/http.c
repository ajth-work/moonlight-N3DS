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

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <mbedtls/debug.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_internal.h>
#include <mbedtls/x509_crt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static CURL *curl;
static uint32_t connection_timeout_s = 60;
static int log_level = 0;
static bool tls_trace_attempted = false;
static bool tls_trace_request_active = false;
static bool tls_trace_saw_certificate_out = false;
static bool tls_trace_saw_handshake = false;
static bool tls_trace_saw_negotiated = false;
static unsigned char *tls_certificate_pem;
static size_t tls_certificate_pem_length;
static unsigned char *tls_private_key_pem;
static size_t tls_private_key_pem_length;
static mbedtls_x509_crt tls_mbed_certificate;
static mbedtls_pk_context tls_mbed_private_key;
static bool tls_mbed_identity_initialized;

static bool _read_identity_file(const char *path, unsigned char **contents,
                                size_t *length) {
    FILE *file = fopen(path, "rb");
    long file_length;
    unsigned char *buffer;
    size_t bytes_read;
    int close_result;
    bool success = false;

    if (file == NULL) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_length = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    buffer = (unsigned char *)malloc((size_t)file_length + 1);
    if (buffer == NULL) {
        fclose(file);
        return false;
    }
    bytes_read = fread(buffer, 1, (size_t)file_length, file);
    bool read_ok = bytes_read == (size_t)file_length && !ferror(file);
    close_result = fclose(file);
    if (read_ok && close_result == 0) {
        buffer[bytes_read] = '\0';
        *contents = buffer;
        *length = bytes_read;
        success = true;
    } else {
        free(buffer);
    }
    return success;
}

static void _retire_legacy_combined_identity(const char *key_directory) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/client-combined.pem", key_directory);
    if (remove(path) == 0) {
        diag_log("TLS_IDENTITY_CLEANUP", "legacy_combined_pem=removed");
    } else if (errno == ENOENT) {
        diag_log("TLS_IDENTITY_CLEANUP", "legacy_combined_pem=absent");
    } else {
        diag_log("TLS_IDENTITY_CLEANUP",
                 "legacy_combined_pem=remove_failed errno=%d", errno);
    }
}

static void _free_tls_identity(void) {
    if (tls_mbed_identity_initialized) {
        mbedtls_x509_crt_free(&tls_mbed_certificate);
        mbedtls_pk_free(&tls_mbed_private_key);
        tls_mbed_identity_initialized = false;
    }
    free(tls_certificate_pem);
    free(tls_private_key_pem);
    tls_certificate_pem = NULL;
    tls_private_key_pem = NULL;
    tls_certificate_pem_length = 0;
    tls_private_key_pem_length = 0;
}

#define CURL_SETOPT_LOG(option, value, setting) do { \
    CURLcode setopt_result = curl_easy_setopt(curl, option, value); \
    diag_log("CURL_SETOPT", "option=%s setting=%s result=%d detail=%s", \
             #option, setting, (int)setopt_result, \
             curl_easy_strerror(setopt_result)); \
} while (0)

static bool _is_https(const char *url) {
    return url != NULL && strncmp(url, "https://", 8) == 0;
}

static const mbedtls_ssl_key_cert *_find_identity_in_config(
    const mbedtls_ssl_config *config) {
    const mbedtls_ssl_key_cert *entry;

    for (entry = config->key_cert; entry != NULL; entry = entry->next) {
        if (entry->cert != NULL && entry->key != NULL &&
            entry->cert->raw.len == tls_mbed_certificate.raw.len &&
            memcmp(entry->cert->raw.p, tls_mbed_certificate.raw.p,
                   tls_mbed_certificate.raw.len) == 0 &&
            mbedtls_pk_check_pair(&entry->cert->pk, entry->key) == 0) {
            return entry;
        }
    }
    return NULL;
}

static void _mbedtls_debug(void *context, int level, const char *file,
                           int line, const char *message) {
    (void)context;
    if (message != NULL) {
        diag_log("MBEDTLS_TLS_TRACE", "level=%d file=%s line=%d %s", level,
                 file != NULL ? file : "?", line, message);
    }
}

static CURLcode _curl_ssl_context(CURL *handle, void *ssl_context,
                                  void *user_data) {
    mbedtls_ssl_config *config = (mbedtls_ssl_config *)ssl_context;
    const mbedtls_ssl_key_cert *identity;
    const char *route;
    int result;

    (void)handle;
    (void)user_data;
    if (config == NULL || !tls_mbed_identity_initialized) {
        diag_log("TLS_IDENTITY_CTX", "result=failed reason=missing_context_or_identity");
        return CURLE_SSL_CONNECT_ERROR;
    }

    identity = _find_identity_in_config(config);
    if (identity != NULL) {
        route = "blob";
    } else {
        result = mbedtls_ssl_conf_own_cert(config, &tls_mbed_certificate,
                                           &tls_mbed_private_key);
        diag_log("TLS_IDENTITY_CTX",
                 "operation=mbedtls_ssl_conf_own_cert result=%d", result);
        if (result != 0) {
            return CURLE_SSL_CERTPROBLEM;
        }
        identity = _find_identity_in_config(config);
        if (identity == NULL) {
            diag_log("TLS_IDENTITY_CTX",
                     "result=failed reason=identity_not_readable_after_configuration");
            return CURLE_SSL_CERTPROBLEM;
        }
        route = "ctx";
    }

    diag_log("TLS_IDENTITY_CTX",
             "result=ok route=%s readback=mbedtls_ssl_config.key_cert certificate_der_bytes=%lu",
             route, (unsigned long)identity->cert->raw.len);
    diag_tls_context_checkpoint(identity->cert->raw.p,
                                identity->cert->raw.len, route);

    if (tls_trace_request_active) {
        mbedtls_ssl_conf_dbg(config, _mbedtls_debug, NULL);
        mbedtls_debug_set_threshold(4);
        diag_log("MBEDTLS_DEBUG", "request_scope=first_https level=4 line_filter=none");
    }
    return CURLE_OK;
}

static int _curl_debug(CURL *handle, curl_infotype type, char *data,
                       size_t size, void *user_data) {
    char line[DIAG_LOG_LINE_MAX];
    char lower[DIAG_LOG_LINE_MAX];
    size_t length = size;
    size_t i;

    (void)handle;
    (void)user_data;
    if (type != CURLINFO_TEXT || data == NULL) {
        return 0;
    }

    if (length >= sizeof(line)) {
        length = sizeof(line) - 1;
    }
    for (i = 0; i < length; i++) {
        unsigned char character = (unsigned char)data[i];
        line[i] = (character == '\r' || character == '\n' ||
                   character < 0x20) ? ' ' : (char)character;
        lower[i] = (char)tolower((unsigned char)line[i]);
    }
    while (length > 0 && line[length - 1] == ' ') {
        length--;
    }
    line[length] = '\0';
    lower[length] = '\0';

    if (strstr(lower, "certificate") != NULL &&
        strstr(lower, "out") != NULL) {
        tls_trace_saw_certificate_out = true;
    }
    if (strstr(lower, "handshake") != NULL || strstr(lower, "tlsv") != NULL) {
        tls_trace_saw_handshake = true;
    }
    if (strstr(lower, "ssl connection using") != NULL ||
        strstr(lower, "tls connection using") != NULL) {
        tls_trace_saw_negotiated = true;
    }

    diag_log("CURL_TLS_TRACE", "%s", line);
    return 0;
}

static void _log_certinfo(CURL *handle) {
    struct curl_certinfo *certificate_info = NULL;
    CURLcode result = curl_easy_getinfo(handle, CURLINFO_CERTINFO,
                                        &certificate_info);
    int certificate_count = certificate_info != NULL
        ? certificate_info->num_of_certs : 0;
    int i;

    diag_log("CURL_CERTINFO",
             "result=%d detail=%s available=%s peer_chain_count=%d",
             (int)result, curl_easy_strerror(result),
             certificate_count > 0 ? "yes" : "no", certificate_count);
    if (result != CURLE_OK || certificate_info == NULL ||
        certificate_info->certinfo == NULL) {
        return;
    }

    for (i = 0; i < certificate_count; i++) {
        const struct curl_slist *field;
        char subject[256] = "unavailable";
        char issuer[256] = "unavailable";
        for (field = certificate_info->certinfo[i]; field != NULL;
             field = field->next) {
            if (field->data == NULL) {
                continue;
            }
            if (strncmp(field->data, "Subject:", 8) == 0) {
                snprintf(subject, sizeof(subject), "%s", field->data + 8);
            } else if (strncmp(field->data, "Issuer:", 7) == 0) {
                snprintf(issuer, sizeof(issuer), "%s", field->data + 7);
            }
        }
        diag_log("CURL_CERTINFO", "peer_chain_depth=%d subject=%s issuer=%s",
                 i, subject, issuer);
    }
}

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
    struct curl_blob certificate_blob;
    struct curl_blob private_key_blob;
    CURLcode context_callback_result;

    curl = curl_easy_init();
    if (!curl)
        return GS_FAILED;

    const curl_version_info_data *curl_info =
        curl_version_info(CURLVERSION_NOW);
    diag_log("CURL_RUNTIME", "version=%s tls_backend=%s",
             curl_version(),
             curl_info != NULL && curl_info->ssl_version != NULL
                 ? curl_info->ssl_version : "unavailable");

    char certificateFilePath[4096];
    snprintf(certificateFilePath, sizeof(certificateFilePath), "%s/%s",
             keyDirectory, CERTIFICATE_FILE_NAME);

    char keyFilePath[4096];
    snprintf(keyFilePath, sizeof(keyFilePath), "%s/%s", keyDirectory,
             KEY_FILE_NAME);
    _retire_legacy_combined_identity(keyDirectory);

    log_level = logLevel;
    tls_trace_attempted = false;
    tls_trace_request_active = false;
    tls_trace_saw_certificate_out = false;
    tls_trace_saw_handshake = false;
    tls_trace_saw_negotiated = false;

    mbedtls_x509_crt_init(&tls_mbed_certificate);
    mbedtls_pk_init(&tls_mbed_private_key);
    tls_mbed_identity_initialized = true;
    if (!_read_identity_file(certificateFilePath, &tls_certificate_pem,
                             &tls_certificate_pem_length) ||
        !_read_identity_file(keyFilePath, &tls_private_key_pem,
                             &tls_private_key_pem_length)) {
        diag_log("TLS_IDENTITY_LOAD", "result=failed source=keys_pem_memory");
        curl_easy_cleanup(curl);
        curl = NULL;
        _free_tls_identity();
        return GS_FAILED;
    }

    int parse_certificate_result = mbedtls_x509_crt_parse(
        &tls_mbed_certificate, tls_certificate_pem,
        tls_certificate_pem_length + 1);
    int parse_private_key_result = mbedtls_pk_parse_key(
        &tls_mbed_private_key, tls_private_key_pem,
        tls_private_key_pem_length + 1, NULL, 0);
    diag_log("TLS_IDENTITY_LOAD",
             "source=keys_pem_memory certificate_parse=%d private_key_parse=%d",
             parse_certificate_result, parse_private_key_result);
    if (parse_certificate_result != 0 || parse_private_key_result != 0) {
        curl_easy_cleanup(curl);
        curl = NULL;
        _free_tls_identity();
        return GS_FAILED;
    }

    CURL_SETOPT_LOG(CURLOPT_SSL_VERIFYHOST, 0L, "0");
    CURL_SETOPT_LOG(CURLOPT_SSLENGINE_DEFAULT, 1L, "default");
    CURL_SETOPT_LOG(CURLOPT_SSLCERTTYPE, "PEM", "PEM");
    CURL_SETOPT_LOG(CURLOPT_SSLKEYTYPE, "PEM", "PEM");
    certificate_blob.data = tls_certificate_pem;
    certificate_blob.len = tls_certificate_pem_length;
    certificate_blob.flags = CURL_BLOB_COPY;
    private_key_blob.data = tls_private_key_pem;
    private_key_blob.len = tls_private_key_pem_length;
    private_key_blob.flags = CURL_BLOB_COPY;
    CURL_SETOPT_LOG(CURLOPT_SSLCERT_BLOB, &certificate_blob,
                    "keys/client.pem:PEM");
    CURL_SETOPT_LOG(CURLOPT_SSLKEY_BLOB, &private_key_blob,
                    "keys/key.pem:PEM");

    context_callback_result = curl_easy_setopt(
        curl, CURLOPT_SSL_CTX_FUNCTION, _curl_ssl_context);
    diag_log("CURL_SETOPT",
             "option=CURLOPT_SSL_CTX_FUNCTION setting=mbedtls_ssl_config_readback_and_fallback result=%d detail=%s",
             (int)context_callback_result,
             curl_easy_strerror(context_callback_result));
    if (context_callback_result != CURLE_OK) {
        curl_easy_cleanup(curl);
        curl = NULL;
        _free_tls_identity();
        return GS_FAILED;
    }
    free(tls_certificate_pem);
    free(tls_private_key_pem);
    tls_certificate_pem = NULL;
    tls_private_key_pem = NULL;
    tls_certificate_pem_length = 0;
    tls_private_key_pem_length = 0;

    CURL_SETOPT_LOG(CURLOPT_SSL_VERIFYPEER, 0L, "0");
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
    bool capture_tls = !_is_https(url) ? false : !tls_trace_attempted;
    _redact_url(url, safe_url, sizeof(safe_url));
    diag_http_begin("GET", safe_url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, connection_timeout_s);
    if (!capture_tls) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, log_level > 0 ? 1L : 0);
    }
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
    if (capture_tls) {
        tls_trace_attempted = true;
        tls_trace_request_active = true;
        tls_trace_saw_certificate_out = false;
        tls_trace_saw_handshake = false;
        tls_trace_saw_negotiated = false;
        CURL_SETOPT_LOG(CURLOPT_VERBOSE, 1L, "1");
        CURL_SETOPT_LOG(CURLOPT_DEBUGFUNCTION, _curl_debug,
                        "handshake-trace");
        CURL_SETOPT_LOG(CURLOPT_DEBUGDATA, NULL, "diag-log");
        CURL_SETOPT_LOG(CURLOPT_CERTINFO, 1L, "1");
    }
    CURLcode res = curl_easy_perform(curl);
    if (capture_tls) {
        tls_trace_request_active = false;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (capture_tls) {
        _log_certinfo(curl);
        diag_log("CURL_TLS_RESULT",
                 "perform=%d detail=%s response_code=%ld client_certificate_out_record=%s handshake_detail=%s negotiated_connection=%s",
                 (int)res, curl_easy_strerror(res), response_code,
                 tls_trace_saw_certificate_out ? "observed" : "not_observed",
                 tls_trace_saw_handshake ? "observed" : "not_observed",
                 tls_trace_saw_negotiated ? "observed" : "not_observed");
        CURL_SETOPT_LOG(CURLOPT_CERTINFO, 0L, "0");
        CURL_SETOPT_LOG(CURLOPT_DEBUGFUNCTION,
                        (curl_debug_callback)NULL, "disabled");
        CURL_SETOPT_LOG(CURLOPT_DEBUGDATA, NULL, "default");
        CURL_SETOPT_LOG(CURLOPT_VERBOSE, log_level > 0 ? 1L : 0L,
                        log_level > 0 ? "1" : "0");
    }
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

void http_cleanup() {
    if (curl != NULL) {
        curl_easy_cleanup(curl);
        curl = NULL;
    }
    _free_tls_identity();
    diag_log("TLS_IDENTITY_CLEANUP", "source=memory objects=freed");
}

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

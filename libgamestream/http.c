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
#include "public_key_pin.h"

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
static char tls_combined_identity_path[4096];
static bool tls_combined_identity_created;
static mbedtls_x509_crt tls_mbed_certificate;
static mbedtls_pk_context tls_mbed_private_key;
static bool tls_mbed_identity_initialized;
/* http_init is a serialized singleton path; keep path assembly off its stack. */
static char tls_identity_file_path[4096];

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

static bool _read_identity_file_from_directory(const char *directory,
                                               const char *filename,
                                               unsigned char **contents,
                                               size_t *length) {
    size_t directory_length;
    size_t filename_length;

    if (directory == NULL || filename == NULL) {
        return false;
    }

    directory_length = strlen(directory);
    filename_length = strlen(filename);
    if (directory_length > sizeof(tls_identity_file_path) - 2 ||
        filename_length > sizeof(tls_identity_file_path) -
                              directory_length - 2) {
        return false;
    }

    memcpy(tls_identity_file_path, directory, directory_length);
    tls_identity_file_path[directory_length] = '/';
    memcpy(tls_identity_file_path + directory_length + 1, filename,
           filename_length + 1);
    return _read_identity_file(tls_identity_file_path, contents, length);
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

static bool _create_combined_identity_file(const char *key_directory) {
    FILE *file;
    size_t written;
    bool success = false;
    int close_result;

    if (snprintf(tls_combined_identity_path,
                 sizeof(tls_combined_identity_path),
                 "%s/client-combined.pem", key_directory) >=
        (int)sizeof(tls_combined_identity_path)) {
        diag_log("TLS_IDENTITY_FILE", "result=failed reason=path_too_long");
        return false;
    }

    file = fopen(tls_combined_identity_path, "wb");
    if (file == NULL) {
        diag_log("TLS_IDENTITY_FILE", "result=failed reason=open errno=%d",
                 errno);
        return false;
    }

    written = fwrite(tls_certificate_pem, 1, tls_certificate_pem_length, file);
    if (written == tls_certificate_pem_length &&
        tls_certificate_pem_length > 0 &&
        tls_certificate_pem[tls_certificate_pem_length - 1] != '\n') {
        success = fputc('\n', file) != EOF;
    } else {
        success = written == tls_certificate_pem_length;
    }
    if (success) {
        written = fwrite(tls_private_key_pem, 1,
                         tls_private_key_pem_length, file);
        success = written == tls_private_key_pem_length;
    }
    if (success) {
        success = fflush(file) == 0;
    }
    close_result = fclose(file);
    success = success && close_result == 0;
    if (!success) {
        remove(tls_combined_identity_path);
        tls_combined_identity_path[0] = '\0';
        diag_log("TLS_IDENTITY_FILE", "result=failed reason=write_or_close");
        return false;
    }

    tls_combined_identity_created = true;
    diag_log("TLS_IDENTITY_FILE",
             "result=created source=existing_cert_and_key combined_pem_bytes=%lu",
             (unsigned long)(tls_certificate_pem_length +
                 tls_private_key_pem_length +
                 (tls_certificate_pem_length > 0 &&
                  tls_certificate_pem[tls_certificate_pem_length - 1] != '\n'
                      ? 1 : 0)));
    return true;
}

static void _remove_combined_identity_file(void) {
    if (!tls_combined_identity_created) {
        return;
    }
    if (remove(tls_combined_identity_path) == 0 || errno == ENOENT) {
        diag_log("TLS_IDENTITY_CLEANUP", "combined_pem=removed");
    } else {
        diag_log("TLS_IDENTITY_CLEANUP",
                 "combined_pem=remove_failed errno=%d", errno);
    }
    tls_combined_identity_created = false;
    tls_combined_identity_path[0] = '\0';
}

static void _log_private_key_header(void) {
    char header[96];
    size_t start = 0;
    size_t length = 0;
    while (start < tls_private_key_pem_length &&
           (tls_private_key_pem[start] == ' ' ||
            tls_private_key_pem[start] == '\t' ||
            tls_private_key_pem[start] == '\r' ||
            tls_private_key_pem[start] == '\n')) {
        start++;
    }
    while (start + length < tls_private_key_pem_length &&
           tls_private_key_pem[start + length] != '\r' &&
           tls_private_key_pem[start + length] != '\n' &&
           length + 1 < sizeof(header)) {
        header[length] = (char)tls_private_key_pem[start + length];
        length++;
    }
    header[length] = '\0';
    diag_log("TLS_IDENTITY_LOAD", "key_pem_header=%s", length > 0
                 ? header : "unavailable");
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

static CURLcode _curl_ssl_context(CURL *handle, void *ssl_context,
                                 void *user_data);

static bool _is_pair_challenge(const char *url) {
    return _is_https(url) && strstr(url, "phrase=pairchallenge") != NULL;
}

static bool _is_retryable_ssl_error(CURLcode result) {
    return result == CURLE_SSL_CERTPROBLEM ||
           result == CURLE_SSL_CONNECT_ERROR ||
           result == CURLE_PEER_FAILED_VERIFICATION ||
           result == CURLE_SSL_CACERT_BADFILE;
}

static bool _reset_response(PHTTP_DATA data) {
    if (data->memory == NULL) {
        data->memory = (char *)malloc(1);
        if (data->memory == NULL) {
            return false;
        }
    }
    data->size = 0;
    data->memory[0] = '\0';
    return true;
}

static bool _set_file_tls_route(void) {
    CURLcode callback_result = curl_easy_setopt(
        curl, CURLOPT_SSL_CTX_FUNCTION, (curl_ssl_ctx_callback)NULL);
    CURLcode data_result = curl_easy_setopt(curl, CURLOPT_SSL_CTX_DATA, NULL);
    CURLcode certificate_result = curl_easy_setopt(
        curl, CURLOPT_SSLCERT, tls_combined_identity_path);
    CURLcode key_result = curl_easy_setopt(
        curl, CURLOPT_SSLKEY, tls_combined_identity_path);

    diag_log("TLS_ROUTE_CONFIG",
             "route=file callback=%d callback_data=%d certificate=%d private_key=%d",
             (int)callback_result, (int)data_result,
             (int)certificate_result, (int)key_result);
    return callback_result == CURLE_OK && data_result == CURLE_OK &&
           certificate_result == CURLE_OK && key_result == CURLE_OK;
}

static bool _set_ctx_tls_route(void) {
    CURLcode certificate_result = curl_easy_setopt(curl, CURLOPT_SSLCERT, NULL);
    CURLcode key_result = curl_easy_setopt(curl, CURLOPT_SSLKEY, NULL);
    CURLcode callback_result = curl_easy_setopt(
        curl, CURLOPT_SSL_CTX_FUNCTION, _curl_ssl_context);
    CURLcode data_result = curl_easy_setopt(curl, CURLOPT_SSL_CTX_DATA, NULL);

    diag_log("TLS_ROUTE_CONFIG",
             "route=ctx callback=%d callback_data=%d certificate=%d private_key=%d",
             (int)callback_result, (int)data_result,
             (int)certificate_result, (int)key_result);
    return callback_result == CURLE_OK && data_result == CURLE_OK &&
           certificate_result == CURLE_OK && key_result == CURLE_OK;
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
    bool config_had_identity;
    bool config_had_app_identity;
    int result;

    (void)handle;
    (void)user_data;
    diag_log("TLS_IDENTITY_CTX", "callback=fired");
    if (config == NULL || !tls_mbed_identity_initialized) {
        diag_log("TLS_IDENTITY_CTX", "result=failed reason=missing_context_or_identity");
        return CURLE_SSL_CONNECT_ERROR;
    }

    config_had_identity = config->key_cert != NULL;
    identity = _find_identity_in_config(config);
    config_had_app_identity = identity != NULL;
    diag_log("TLS_IDENTITY_CTX",
             "config_already_carried_identity=%s config_already_carried_app_identity=%s",
             config_had_identity ? "yes" : "no",
             config_had_app_identity ? "yes" : "no");

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

    diag_log("TLS_IDENTITY_CTX",
             "result=ok route=ctx readback=mbedtls_ssl_config.key_cert certificate_der_bytes=%lu",
             (unsigned long)identity->cert->raw.len);
    diag_tls_context_checkpoint(identity->cert->raw.p,
                                identity->cert->raw.len, "ctx");

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
    int parse_certificate_result;
    int parse_private_key_result;
    mbedtls_pk_type_t private_key_type_id;
    const char *private_key_type;

    uintptr_t stack_pointer;
    __asm__ volatile("mov %0, sp" : "=r"(stack_pointer) : : "memory");
    diag_stack_checkpoint("connect.http_init.entry", stack_pointer);

    curl = curl_easy_init();
    if (!curl)
        return GS_FAILED;

    tls_combined_identity_created = false;
    tls_combined_identity_path[0] = '\0';

    const curl_version_info_data *curl_info =
        curl_version_info(CURLVERSION_NOW);
    diag_log("CURL_RUNTIME", "version=%s tls_backend=%s",
             curl_version(),
             curl_info != NULL && curl_info->ssl_version != NULL
                 ? curl_info->ssl_version : "unavailable");

    log_level = logLevel;
    tls_trace_attempted = false;
    tls_trace_request_active = false;
    tls_trace_saw_certificate_out = false;
    tls_trace_saw_handshake = false;
    tls_trace_saw_negotiated = false;

    mbedtls_x509_crt_init(&tls_mbed_certificate);
    mbedtls_pk_init(&tls_mbed_private_key);
    tls_mbed_identity_initialized = true;
    if (!_read_identity_file_from_directory(
            keyDirectory, CERTIFICATE_FILE_NAME, &tls_certificate_pem,
            &tls_certificate_pem_length) ||
        !_read_identity_file_from_directory(
            keyDirectory, KEY_FILE_NAME, &tls_private_key_pem,
            &tls_private_key_pem_length)) {
        diag_log("TLS_IDENTITY_LOAD", "result=failed source=existing_pem_files");
        curl_easy_cleanup(curl);
        curl = NULL;
        _free_tls_identity();
        return GS_FAILED;
    }

    parse_certificate_result = mbedtls_x509_crt_parse(
        &tls_mbed_certificate, tls_certificate_pem,
        tls_certificate_pem_length + 1);
    parse_private_key_result = mbedtls_pk_parse_key(
        &tls_mbed_private_key, tls_private_key_pem,
        tls_private_key_pem_length + 1, NULL, 0);
    diag_log("TLS_IDENTITY_LOAD",
             "source=existing_pem_files certificate_parse=%d private_key_parse=%d",
             parse_certificate_result, parse_private_key_result);
    if (parse_certificate_result != 0 || parse_private_key_result != 0) {
        curl_easy_cleanup(curl);
        curl = NULL;
        _free_tls_identity();
        return GS_FAILED;
    }

    private_key_type_id = mbedtls_pk_get_type(&tls_mbed_private_key);
    private_key_type = mbedtls_pk_get_name(&tls_mbed_private_key);
    diag_log("TLS_IDENTITY_LOAD", "private_key_type=%s mbedtls_pk_type=%d",
             private_key_type != NULL ? private_key_type : "unknown",
             (int)private_key_type_id);
    _log_private_key_header();
    if (!_create_combined_identity_file(keyDirectory)) {
        curl_easy_cleanup(curl);
        curl = NULL;
        _free_tls_identity();
        return GS_FAILED;
    }
    diag_tls_identity_checkpoint(tls_mbed_certificate.raw.p,
                                 tls_mbed_certificate.raw.len, "file");

    CURL_SETOPT_LOG(CURLOPT_SSL_VERIFYHOST, 0L, "0");
    CURL_SETOPT_LOG(CURLOPT_SSLCERTTYPE, "PEM", "PEM");
    CURL_SETOPT_LOG(CURLOPT_SSLKEYTYPE, "PEM", "PEM");
    if (!_set_file_tls_route()) {
        curl_easy_cleanup(curl);
        curl = NULL;
        _remove_combined_identity_file();
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

static CURLcode _perform_https_attempt(const char *url, PHTTP_DATA data,
                                      const char *route, unsigned int attempt,
                                      long *response_code) {
    CURLcode result;
    char response_text[32];

    *response_code = 0;
    diag_tls_route_attempt(route);
    curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, response_code);
    if (*response_code > 0) {
        snprintf(response_text, sizeof(response_text), "%ld", *response_code);
    } else {
        snprintf(response_text, sizeof(response_text), "none");
    }
    diag_log("TLS_REQUEST_ATTEMPT",
             "attempt=%u route=%s perform=%d detail=%s response_code=%s",
             attempt, route, (int)result, curl_easy_strerror(result),
             response_text);
    if (_is_pair_challenge(url) && *response_code > 0) {
        diag_log_sha256("PAIR_CHALLENGE_RESPONSE", "response_body",
                        (const unsigned char *)data->memory, data->size);
    }
    return result;
}

static int _http_request(char *url, PHTTP_DATA data, const char *pin) {
    /* Diagnostic-only truncation; CURLOPT_URL still receives the original URL.
     * Per-call storage avoids races without holding a lock across curl. */
    char safe_url[384];
    long response_code = 0;
    bool https = _is_https(url);
    bool capture_tls = https && !tls_trace_attempted;
    bool attempted_ctx = false;
    bool performed_ctx = false;
    CURLcode result;
    if (pin != NULL) {
        if (curl == NULL || url == NULL || data == NULL || !_is_https(url) ||
            !valid_public_key_pin(pin)) return GS_FAILED;
        if (curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, pin) != CURLE_OK ||
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK) {
            curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, NULL);
            return GS_FAILED;
        }
    }
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
        printf("Request %s\n", safe_url);
    }

    if (!_reset_response(data)) {
        if (pin != NULL) curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, NULL);
        diag_http_end(response_code, GS_OUT_OF_MEMORY);
        return GS_OUT_OF_MEMORY;
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
    }

    if (https) {
        if (!_set_file_tls_route()) {
            result = CURLE_SSL_CERTPROBLEM;
            diag_log("TLS_REQUEST_ATTEMPT",
                     "attempt=1 route=file perform=not_run reason=route_setup_failed response_code=none");
        } else {
            result = _perform_https_attempt(url, data, "file", 1,
                                            &response_code);
        }

        if (_is_retryable_ssl_error(result)) {
            attempted_ctx = true;
            response_code = 0;
            if (!_reset_response(data)) {
                result = CURLE_OUT_OF_MEMORY;
                diag_log("TLS_REQUEST_ATTEMPT",
                         "attempt=2 route=ctx perform=not_run reason=response_reset_failed response_code=none");
            } else if (!_set_ctx_tls_route()) {
                result = CURLE_SSL_CERTPROBLEM;
                diag_log("TLS_REQUEST_ATTEMPT",
                         "attempt=2 route=ctx perform=not_run reason=route_setup_failed response_code=none");
            } else {
                performed_ctx = true;
                result = _perform_https_attempt(url, data, "ctx", 2,
                                                &response_code);
            }
        }
        if (attempted_ctx) {
            _set_file_tls_route();
        }
        curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 0L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 0L);
    } else {
        result = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    }

    if (capture_tls) {
        tls_trace_request_active = false;
        mbedtls_debug_set_threshold(0);
        diag_log("MBEDTLS_DEBUG", "request_scope=closed level=0");
        diag_log("CURL_TLS_RESULT",
                 "perform=%d detail=%s response_code=%ld route=%s client_certificate_out_record=%s handshake_detail=%s negotiated_connection=%s",
                 (int)result, curl_easy_strerror(result), response_code,
                 performed_ctx ? "ctx" : "file",
                 tls_trace_saw_certificate_out ? "observed" : "not_observed",
                 tls_trace_saw_handshake ? "observed" : "not_observed",
                 tls_trace_saw_negotiated ? "observed" : "not_observed");
        CURL_SETOPT_LOG(CURLOPT_DEBUGFUNCTION,
                        (curl_debug_callback)NULL, "disabled");
        CURL_SETOPT_LOG(CURLOPT_DEBUGDATA, NULL, "default");
        CURL_SETOPT_LOG(CURLOPT_VERBOSE, log_level > 0 ? 1L : 0L,
                        log_level > 0 ? "1" : "0");
    }
    if (pin != NULL) curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, NULL);
    diag_http_end(response_code, (int)result);

    if (result == CURLE_OUT_OF_MEMORY) {
        return GS_OUT_OF_MEMORY;
    }
    if (result != CURLE_OK) {
        gs_error = curl_easy_strerror(result);
        return GS_FAILED;
    } else if (data->memory == NULL) {
        return GS_OUT_OF_MEMORY;
    }

    if (log_level) {
        printf("Response: %lu bytes\n\n", (unsigned long)data->size);
    }

    return GS_OK;
}

int http_request(char *url, PHTTP_DATA data) {
    return _http_request(url, data, NULL);
}

int http_request_pinned(char *url, PHTTP_DATA data, const char *public_key_pin) {
    if (public_key_pin == NULL) return GS_FAILED;
    return _http_request(url, data, public_key_pin);
}

void http_cleanup() {
    if (curl != NULL) {
        curl_easy_cleanup(curl);
        curl = NULL;
    }
    _remove_combined_identity_file();
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

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
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static CURL *curl;
static uint32_t connection_timeout_s = 60;
static int log_level = 0;
static char tls_identity_path[4096];
static bool tls_identity_file_created = false;
static bool tls_trace_attempted = false;
static bool tls_trace_saw_certificate_out = false;
static bool tls_trace_saw_handshake = false;
static bool tls_trace_saw_negotiated = false;

#define CURL_SETOPT_LOG(option, value, setting) do { \
    CURLcode setopt_result = curl_easy_setopt(curl, option, value); \
    diag_log("CURL_SETOPT", "option=%s setting=%s result=%d detail=%s", \
             #option, setting, (int)setopt_result, \
             curl_easy_strerror(setopt_result)); \
} while (0)

static bool _append_file(FILE *output, const char *path,
                         bool ensure_newline) {
    char buffer[1024];
    size_t count;
    size_t total = 0;
    int last_character = EOF;
    FILE *input = fopen(path, "rb");
    bool success = true;

    if (input == NULL) {
        return false;
    }

    while ((count = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        if (fwrite(buffer, 1, count, output) != count) {
            success = false;
            break;
        }
        total += count;
        last_character = (unsigned char)buffer[count - 1];
    }
    if (ferror(input)) {
        success = false;
    }
    if (fclose(input) != 0) {
        success = false;
    }
    if (total == 0) {
        success = false;
    }
    if (success && ensure_newline && last_character != '\n' &&
        fputc('\n', output) == EOF) {
        success = false;
    }
    return success;
}

/* libcurl's 3DS build uses mbedTLS, which reads PEM certs and keys from files.
 * Give it one file containing both objects to avoid separate identity loading. */
static bool _build_combined_identity(const char *certificate_path,
                                    const char *key_path,
                                    const char *combined_path) {
    FILE *output = fopen(combined_path, "wb");
    bool success;

    if (output == NULL) {
        diag_log("TLS_IDENTITY_BUILD", "result=failed stage=open path=%s",
                 combined_path);
        return false;
    }

    success = _append_file(output, certificate_path, true) &&
              _append_file(output, key_path, false);
    if (fclose(output) != 0) {
        success = false;
    }
    if (!success) {
        remove(combined_path);
        diag_log("TLS_IDENTITY_BUILD",
                 "result=failed stage=copy certificate_path=%s key_path=%s",
                 certificate_path, key_path);
        return false;
    }

    diag_log("TLS_IDENTITY_BUILD",
             "result=ok format=combined_pem certificate_path=%s key_path=%s output_path=%s",
             certificate_path, key_path, combined_path);
    return true;
}

static bool _is_https(const char *url) {
    return url != NULL && strncmp(url, "https://", 8) == 0;
}

static int _curl_debug(CURL *handle, curl_infotype type, char *data,
                       size_t size, void *user_data) {
    char line[DIAG_LOG_LINE_MAX];
    char lower[DIAG_LOG_LINE_MAX];
    size_t length = size;
    size_t i;
    bool interesting;

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

    interesting = strstr(lower, "ssl") != NULL ||
                  strstr(lower, "tls") != NULL ||
                  strstr(lower, "cert") != NULL ||
                  strstr(lower, "handshake") != NULL ||
                  strstr(lower, "mbedtls") != NULL ||
                  strstr(lower, "openssl") != NULL;
    if (!interesting) {
        return 0;
    }

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

    char combinedIdentityPath[4096];
    snprintf(combinedIdentityPath, sizeof(combinedIdentityPath),
             "%s/client-combined.pem", keyDirectory);

    log_level = logLevel;
    tls_trace_attempted = false;
    tls_trace_saw_certificate_out = false;
    tls_trace_saw_handshake = false;
    tls_trace_saw_negotiated = false;

    CURL_SETOPT_LOG(CURLOPT_SSL_VERIFYHOST, 0L, "0");
    CURL_SETOPT_LOG(CURLOPT_SSLENGINE_DEFAULT, 1L, "default");
    if (!_build_combined_identity(certificateFilePath, keyFilePath,
                                  combinedIdentityPath)) {
        curl_easy_cleanup(curl);
        curl = NULL;
        return GS_FAILED;
    }
    snprintf(tls_identity_path, sizeof(tls_identity_path), "%s",
             combinedIdentityPath);
    tls_identity_file_created = true;
    CURL_SETOPT_LOG(CURLOPT_SSLCERTTYPE, "PEM", "PEM");
    CURL_SETOPT_LOG(CURLOPT_SSLCERT, combinedIdentityPath,
                    combinedIdentityPath);
    CURL_SETOPT_LOG(CURLOPT_SSLKEYTYPE, "PEM", "PEM");
    CURL_SETOPT_LOG(CURLOPT_SSLKEY, combinedIdentityPath,
                    combinedIdentityPath);
    FILE *certificate_file = fopen(certificateFilePath, "r");
    X509 *tls_certificate = certificate_file != NULL
        ? PEM_read_X509(certificate_file, NULL, NULL, NULL)
        : NULL;
    if (certificate_file != NULL) {
        fclose(certificate_file);
    }
    diag_cert_checkpoint(DIAG_CERT_TLS, tls_certificate, "file",
                         certificateFilePath, keyFilePath, NULL);
    diag_log("TLS_IDENTITY", "curl_source=runtime_combined_pem cert=%s key=%s",
             combinedIdentityPath, combinedIdentityPath);
    if (tls_certificate != NULL) {
        X509_free(tls_certificate);
    }
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
    if (tls_identity_file_created) {
        if (remove(tls_identity_path) == 0) {
            diag_log("TLS_IDENTITY_CLEANUP", "combined_pem=removed");
        } else {
            diag_log("TLS_IDENTITY_CLEANUP",
                     "combined_pem=remove_failed path=%s", tls_identity_path);
        }
        tls_identity_file_created = false;
        tls_identity_path[0] = '\0';
    }
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

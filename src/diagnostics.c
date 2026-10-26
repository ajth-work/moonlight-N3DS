#include "diagnostics.h"

#include <3ds.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/sha.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DIAG_LOG_PATH "sdmc:/3ds/moonlight/moonlight.log"
#define UNIX_EPOCH_OFFSET_MS 2208988800000ULL
#define DIAG_FORMATTED_LINE_MAX 4096

static LightLock diag_lock;
static FILE *diag_file = NULL;
static bool diag_initialized = false;
static bool diag_cancel = false;
static bool diag_watchdog_reported = false;
static bool diag_http_active = false;
static char diag_phase[128] = "startup";
static char diag_http_method[16] = "";
static char diag_http_call[DIAG_LOG_LINE_MAX] = "";
static char diag_pair_fingerprint[DIAG_CERT_FINGERPRINT_BUFFER_SIZE] = "";
static char diag_pair_source[DIAG_CERT_SOURCE_MAX] = "";
static char diag_tls_fingerprint[DIAG_CERT_FINGERPRINT_BUFFER_SIZE] = "";
static char diag_tls_source[DIAG_CERT_SOURCE_MAX] = "";
static char diag_recent[DIAG_RECENT_LINE_COUNT][DIAG_LOG_LINE_MAX];
static unsigned int diag_recent_count = 0;
static unsigned int diag_recent_next = 0;
static u64 diag_start_tick = 0;
static u64 diag_phase_tick = 0;

bool diag_cert_fingerprint(X509 *certificate, char *out,
                           unsigned int out_capacity) {
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned char *der;
    unsigned char *der_cursor;
    int der_length;
    unsigned int i;

    if (out == NULL || out_capacity < DIAG_CERT_FINGERPRINT_BUFFER_SIZE) {
        return false;
    }
    out[0] = '\0';
    if (certificate == NULL) {
        return false;
    }

    der_length = i2d_X509(certificate, NULL);
    if (der_length <= 0) {
        return false;
    }
    der = (unsigned char *)malloc((size_t)der_length);
    if (der == NULL) {
        return false;
    }
    der_cursor = der;
    if (i2d_X509(certificate, &der_cursor) != der_length) {
        free(der);
        return false;
    }

    SHA256(der, (size_t)der_length, digest);
    free(der);
    for (i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[DIAG_CERT_FINGERPRINT_HEX_LENGTH] = '\0';
    return true;
}

static void bio_to_text(BIO *bio, char *out, size_t capacity) {
    char *data = NULL;
    long length;
    size_t copy_length;
    if (capacity == 0) {
        return;
    }
    out[0] = '\0';
    if (bio == NULL) {
        return;
    }
    length = BIO_get_mem_data(bio, &data);
    if (length <= 0 || data == NULL) {
        return;
    }
    copy_length = (size_t)length;
    if (copy_length >= capacity) {
        copy_length = capacity - 1;
    }
    memcpy(out, data, copy_length);
    out[copy_length] = '\0';
}

static const char *cert_stage_event(DIAG_CERT_STAGE stage) {
    switch (stage) {
    case DIAG_CERT_STARTUP:
        return "CERT_STARTUP";
    case DIAG_CERT_PAIRING:
        return "CERT_PAIR";
    case DIAG_CERT_TLS:
        return "CERT_TLS";
    default:
        return "CERT_UNKNOWN";
    }
}

bool diag_cert_checkpoint(DIAG_CERT_STAGE stage, X509 *certificate,
                          const char *source, const char *certificate_path,
                          const char *key_path, const char *p12_path) {
    char fingerprint[DIAG_CERT_FINGERPRINT_BUFFER_SIZE] = "";
    char subject[512] = "unavailable";
    char serial[128] = "unavailable";
    char not_before[128] = "unavailable";
    char not_after[128] = "unavailable";
    BIGNUM *serial_number;
    char *serial_hex;
    BIO *bio;
    bool fingerprint_ok;

    fingerprint_ok = diag_cert_fingerprint(certificate, fingerprint,
                                             sizeof(fingerprint));
    if (certificate != NULL) {
        if (X509_NAME_oneline(X509_get_subject_name(certificate), subject,
                              sizeof(subject)) == NULL) {
            snprintf(subject, sizeof(subject), "unavailable");
        }

        serial_number = ASN1_INTEGER_to_BN(
            X509_get_serialNumber(certificate), NULL);
        if (serial_number != NULL) {
            serial_hex = BN_bn2hex(serial_number);
            if (serial_hex != NULL) {
                snprintf(serial, sizeof(serial), "0x%s", serial_hex);
                OPENSSL_free(serial_hex);
            }
            BN_free(serial_number);
        }
        bio = BIO_new(BIO_s_mem());
        if (bio != NULL) {
            if (ASN1_TIME_print(bio, X509_get0_notBefore(certificate)) > 0) {
                bio_to_text(bio, not_before, sizeof(not_before));
            }
            BIO_free(bio);
        }
        bio = BIO_new(BIO_s_mem());
        if (bio != NULL) {
            if (ASN1_TIME_print(bio, X509_get0_notAfter(certificate)) > 0) {
                bio_to_text(bio, not_after, sizeof(not_after));
            }
            BIO_free(bio);
        }
    }

    diag_log(cert_stage_event(stage),
             "sha256_der=%s source=%s certificate_path=%s key_path=%s p12_path=%s subject=%s serial=%s notBefore=%s notAfter=%s",
             fingerprint_ok ? fingerprint : "unavailable",
             source != NULL ? source : "unknown",
             certificate_path != NULL ? certificate_path : "-",
             key_path != NULL ? key_path : "-",
             p12_path != NULL ? p12_path : "-", subject, serial,
             not_before, not_after);

    if (stage == DIAG_CERT_PAIRING || stage == DIAG_CERT_TLS) {
        LightLock_Lock(&diag_lock);
        if (stage == DIAG_CERT_PAIRING) {
            snprintf(diag_pair_fingerprint, sizeof(diag_pair_fingerprint),
                     "%s", fingerprint_ok ? fingerprint : "");
            snprintf(diag_pair_source, sizeof(diag_pair_source), "%s",
                     source != NULL ? source : "unknown");
        } else {
            snprintf(diag_tls_fingerprint, sizeof(diag_tls_fingerprint),
                     "%s", fingerprint_ok ? fingerprint : "");
            snprintf(diag_tls_source, sizeof(diag_tls_source), "%s",
                     source != NULL ? source : "unknown");
        }
        LightLock_Unlock(&diag_lock);
    }
    return fingerprint_ok;
}

void diag_get_cert_panel_state(
    char *pair_fingerprint, unsigned int pair_fingerprint_capacity,
    char *pair_source, unsigned int pair_source_capacity,
    char *tls_fingerprint, unsigned int tls_fingerprint_capacity,
    char *tls_source, unsigned int tls_source_capacity) {
    LightLock_Lock(&diag_lock);
    if (pair_fingerprint != NULL && pair_fingerprint_capacity > 0) {
        snprintf(pair_fingerprint, pair_fingerprint_capacity, "%s",
                 diag_pair_fingerprint);
    }
    if (pair_source != NULL && pair_source_capacity > 0) {
        snprintf(pair_source, pair_source_capacity, "%s", diag_pair_source);
    }
    if (tls_fingerprint != NULL && tls_fingerprint_capacity > 0) {
        snprintf(tls_fingerprint, tls_fingerprint_capacity, "%s",
                 diag_tls_fingerprint);
    }
    if (tls_source != NULL && tls_source_capacity > 0) {
        snprintf(tls_source, tls_source_capacity, "%s", diag_tls_source);
    }
    LightLock_Unlock(&diag_lock);
}

static uint64_t ticks_to_ms(u64 ticks) {
    return (uint64_t)(ticks * 1000ULL / SYSCLOCK_ARM11);
}

uint64_t diag_elapsed_ms(void) {
    if (!diag_initialized) {
        return 0;
    }
    return ticks_to_ms(svcGetSystemTick() - diag_start_tick);
}

uint64_t diag_phase_elapsed_ms(void) {
    u64 phase_tick;
    LightLock_Lock(&diag_lock);
    phase_tick = diag_phase_tick;
    LightLock_Unlock(&diag_lock);
    return ticks_to_ms(svcGetSystemTick() - phase_tick);
}

static void format_wall_time(char *out, size_t capacity) {
    u64 wall_ms = osGetTime();
    time_t unix_seconds = (time_t)((wall_ms - UNIX_EPOCH_OFFSET_MS) / 1000ULL);
    struct tm wall_tm;
    if (gmtime_r(&unix_seconds, &wall_tm) == NULL) {
        snprintf(out, capacity, "wall-time-unavailable");
        return;
    }
    snprintf(out, capacity, "%04d-%02d-%02dT%02d:%02d:%02d.%03lluZ",
             wall_tm.tm_year + 1900, wall_tm.tm_mon + 1, wall_tm.tm_mday,
             wall_tm.tm_hour, wall_tm.tm_min, wall_tm.tm_sec,
             (unsigned long long)(wall_ms % 1000ULL));
}

void diag_log(const char *event, const char *format, ...) {
    char detail[3072];
    char line[DIAG_FORMATTED_LINE_MAX];
    char wall_time[40];
    va_list args;
    va_start(args, format);
    vsnprintf(detail, sizeof(detail), format ? format : "", args);
    va_end(args);

    format_wall_time(wall_time, sizeof(wall_time));
    snprintf(line, sizeof(line), "%s +%08llums %-18s %s", wall_time,
             (unsigned long long)diag_elapsed_ms(), event ? event : "EVENT",
             detail);

    LightLock_Lock(&diag_lock);
    snprintf(diag_recent[diag_recent_next], DIAG_LOG_LINE_MAX, "%s", line);
    diag_recent_next = (diag_recent_next + 1) % DIAG_RECENT_LINE_COUNT;
    if (diag_recent_count < DIAG_RECENT_LINE_COUNT) {
        diag_recent_count++;
    }
    if (diag_file != NULL) {
        fputs(line, diag_file);
        fputc('\n', diag_file);
        fflush(diag_file);
        fsync(fileno(diag_file));
    }
    LightLock_Unlock(&diag_lock);
}

void diag_init(void) {
    LightLock_Init(&diag_lock);
    diag_start_tick = svcGetSystemTick();
    diag_phase_tick = diag_start_tick;
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/moonlight", 0777);
    diag_file = fopen(DIAG_LOG_PATH, "a");
    diag_initialized = true;
    diag_log("LOGGER_OPEN", "path=sd:/3ds/moonlight/moonlight.log opened=%s flush=fflush+fsync",
             diag_file != NULL ? "yes" : "no");
    diag_set_phase("app.startup");
}

void diag_close(void) {
    LightLock_Lock(&diag_lock);
    if (diag_file != NULL) {
        fflush(diag_file);
        fsync(fileno(diag_file));
        fclose(diag_file);
        diag_file = NULL;
    }
    LightLock_Unlock(&diag_lock);
}

void diag_set_phase(const char *phase) {
    uint32_t free_bytes = osGetMemRegionFree(MEMREGION_APPLICATION);
    LightLock_Lock(&diag_lock);
    snprintf(diag_phase, sizeof(diag_phase), "%s", phase ? phase : "unknown");
    diag_phase_tick = svcGetSystemTick();
    diag_watchdog_reported = false;
    LightLock_Unlock(&diag_lock);
    diag_log("PHASE", "name=%s free_application_bytes=%lu",
             phase ? phase : "unknown", (unsigned long)free_bytes);
}

void diag_phase_outcome(const char *phase, const char *outcome, int result) {
    uint32_t free_bytes = osGetMemRegionFree(MEMREGION_APPLICATION);
    diag_log("PHASE_OUTCOME", "name=%s outcome=%s result=%d free_application_bytes=%lu",
             phase ? phase : "unknown", outcome ? outcome : "unknown", result,
             (unsigned long)free_bytes);
}

void diag_http_begin(const char *method, const char *safe_url) {
    LightLock_Lock(&diag_lock);
    diag_http_active = true;
    snprintf(diag_http_method, sizeof(diag_http_method), "%s",
             method ? method : "?");
    snprintf(diag_http_call, sizeof(diag_http_call), "%s %s",
             method ? method : "?", safe_url ? safe_url : "?");
    LightLock_Unlock(&diag_lock);
    diag_log("HTTP_BEGIN", "method=%s url=%s", method ? method : "?",
             safe_url ? safe_url : "?");
}

void diag_http_end(long status_code, int transport_result) {
    char active_call[DIAG_LOG_LINE_MAX];
    char method[sizeof(diag_http_method)];
    LightLock_Lock(&diag_lock);
    snprintf(method, sizeof(method), "%s", diag_http_method);
    snprintf(active_call, sizeof(active_call), "%s", diag_http_call);
    diag_http_active = false;
    diag_http_method[0] = '\0';
    diag_http_call[0] = '\0';
    LightLock_Unlock(&diag_lock);
    diag_log("HTTP_STATUS", "method=%s status=%ld transport_result=%d",
             method[0] ? method : "GET", status_code, transport_result);
    diag_log("HTTP_END", "request=%s status=%ld transport_result=%d",
             active_call, status_code, transport_result);
}

void diag_get_active_http(char *out, unsigned int capacity, bool *active) {
    LightLock_Lock(&diag_lock);
    if (active != NULL) {
        *active = diag_http_active;
    }
    if (out != NULL && capacity > 0) {
        snprintf(out, capacity, "%s", diag_http_call);
    }
    LightLock_Unlock(&diag_lock);
}

void diag_cancel_reset(void) {
    LightLock_Lock(&diag_lock);
    diag_cancel = false;
    LightLock_Unlock(&diag_lock);
}

void diag_request_cancel(void) {
    LightLock_Lock(&diag_lock);
    diag_cancel = true;
    LightLock_Unlock(&diag_lock);
    diag_log("CANCEL_REQUESTED", "watchdog user pressed B");
}

bool diag_cancel_requested(void) {
    bool requested;
    LightLock_Lock(&diag_lock);
    requested = diag_cancel;
    LightLock_Unlock(&diag_lock);
    return requested;
}

bool diag_watchdog_poll(void) {
    bool should_report = false;
    char phase[sizeof(diag_phase)];
    uint64_t elapsed;
    uint32_t free_bytes = osGetMemRegionFree(MEMREGION_APPLICATION);
    u64 now = svcGetSystemTick();

    LightLock_Lock(&diag_lock);
    elapsed = ticks_to_ms(now - diag_phase_tick);
    if (elapsed >= DIAG_WATCHDOG_LIMIT_MS && !diag_watchdog_reported) {
        diag_watchdog_reported = true;
        snprintf(phase, sizeof(phase), "%s", diag_phase);
        should_report = true;
    }
    LightLock_Unlock(&diag_lock);

    if (should_report) {
        diag_log("WATCHDOG", "phase=%s elapsed_ms=%llu free_application_bytes=%lu",
                 phase, (unsigned long long)elapsed, (unsigned long)free_bytes);
    }
    return should_report;
}

void diag_get_state(char *phase_out, unsigned int phase_capacity,
                    char lines_out[DIAG_RECENT_LINE_COUNT][DIAG_LOG_LINE_MAX],
                    unsigned int *line_count, bool *watchdog_expired) {
    unsigned int count;
    unsigned int first;
    unsigned int i;
    LightLock_Lock(&diag_lock);
    if (phase_out != NULL && phase_capacity > 0) {
        snprintf(phase_out, phase_capacity, "%s", diag_phase);
    }
    count = diag_recent_count;
    first = (diag_recent_next + DIAG_RECENT_LINE_COUNT - count) % DIAG_RECENT_LINE_COUNT;
    if (lines_out != NULL) {
        for (i = 0; i < count; i++) {
            snprintf(lines_out[i], DIAG_LOG_LINE_MAX, "%s",
                     diag_recent[(first + i) % DIAG_RECENT_LINE_COUNT]);
        }
    }
    if (line_count != NULL) {
        *line_count = count;
    }
    if (watchdog_expired != NULL) {
        *watchdog_expired = diag_watchdog_reported;
    }
    LightLock_Unlock(&diag_lock);
}

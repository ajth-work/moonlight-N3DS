#include "diagnostics.h"
#include "diagnostic_spool.h"

#include <3ds.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/sha.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#define DIAG_LOG_PATH "sdmc:/3ds/moonlight/moonlight.log"
#define DIAG_SESSION_DIR "sdmc:/3ds/moonlight/sessions"
#define UNIX_EPOCH_OFFSET_MS 2208988800000ULL
#define DIAG_FORMATTED_LINE_MAX 4096
#define DIAG_STACK_RECORD_CAPACITY 512
#define DIAG_SESSION_MAX_BYTES (256U * 1024U) /* individual rotated segment */
#define DIAG_SESSION_PARTS 29U
#define DIAG_CRITICAL_BYTES (64U * 1024U)
#define DIAG_SESSION_TAIL_COUNT 16

static LightLock diag_lock;
/* Protected by diag_lock, including formatting and file I/O. No logger calls
 * while holding this lock; it is never held across a network operation. */
static char diag_detail[3072];
static char diag_line[DIAG_FORMATTED_LINE_MAX];
static char diag_wall_time[40];
#define DIAG_HTTP_FACT_COUNT 8
typedef struct {
    char call[DIAG_LOG_LINE_MAX];
    long status;
    int transport;
    bool complete;
} DiagHttpFact;
static DiagHttpFact diag_http_facts[DIAG_HTTP_FACT_COUNT];
static unsigned int diag_http_fact_count;
static unsigned int diag_http_dropped;
static bool diag_http_deferred;
static FILE *diag_file = NULL;
static FILE *diag_session_file = NULL;
static char diag_session_id[48];
static char diag_session_path[128];
static unsigned long long diag_session_begin_epoch;
static size_t diag_session_bytes;
static bool diag_session_capped;
static unsigned int diag_session_part;
static bool diag_session_stream;
static bool diag_storage_pressure;
static unsigned int diag_record_sequence;
static volatile uint32_t diag_wire_sent, diag_wire_total;

static char diag_session_tail[DIAG_SESSION_TAIL_COUNT][DIAG_FORMATTED_LINE_MAX];
static unsigned int diag_session_tail_count;
static unsigned int diag_session_sequence;
static bool diag_initialized = false;
static volatile bool diag_logging_enabled = true;
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
static MemInfo diag_stack_meminfo;
static PageInfo diag_stack_pageinfo;
static char diag_stack_record[DIAG_STACK_RECORD_CAPACITY];
static size_t diag_stack_record_length = 0;
static char diag_ui_top_state[32];
static char diag_ui_bottom_state[32];
static volatile uint32_t diag_video_decoded_frames = 0;
static volatile uint32_t diag_video_top_draw_calls = 0;
static volatile uint32_t diag_video_completed_presentations = 0;
static volatile uint32_t diag_video_decoder_submitted = 0;
static volatile uint32_t diag_video_decoder_accepted = 0;
static volatile uint32_t diag_video_decoder_failed = 0;
static volatile int diag_video_decoder_last_failure = 0;
static u64 diag_video_last_presentation_ms = 0;
static bool diag_video_freeze_reported = false;

static bool session_tail_event(const char *event) {
    return event != NULL &&
           (strcmp(event, "STREAM_END") == 0 ||
            strcmp(event, "STREAM_STOP") == 0 ||
            strcmp(event, "FREEZE_DETECTED") == 0 ||
            strcmp(event, "FINAL_PRESENTATION") == 0 ||
            strcmp(event, "SESSION_END") == 0 ||
            strcmp(event, "SESSION_PHASE") == 0 ||
            strcmp(event, "SESSION_BEGIN") == 0 ||
            strcmp(event, "STREAM_STARTED") == 0 ||
            strcmp(event, "TOP_PRESENTATION_DIAG") == 0 ||
            strstr(event, "_ERROR") != NULL);
}

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

bool diag_tls_context_checkpoint(const unsigned char *certificate_der,
                                size_t certificate_der_length,
                                const char *source) {
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[SHA256_DIGEST_LENGTH];
    char fingerprint[DIAG_CERT_FINGERPRINT_BUFFER_SIZE];
    unsigned int i;

    if (certificate_der == NULL || certificate_der_length == 0) {
        diag_log("CERT_TLS", "sha256_der=unavailable source=%s readback=failed",
                 source != NULL ? source : "unknown");
        return false;
    }

    SHA256(certificate_der, certificate_der_length, digest);
    for (i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        fingerprint[i * 2] = hex[digest[i] >> 4];
        fingerprint[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    fingerprint[DIAG_CERT_FINGERPRINT_HEX_LENGTH] = '\0';

    diag_log("CERT_TLS",
             "sha256_der=%s source=%s readback=mbedtls_ssl_config.key_cert",
             fingerprint, source != NULL ? source : "unknown");
    LightLock_Lock(&diag_lock);
    snprintf(diag_tls_fingerprint, sizeof(diag_tls_fingerprint), "%s",
             fingerprint);
    snprintf(diag_tls_source, sizeof(diag_tls_source), "%s",
             source != NULL ? source : "unknown");
    LightLock_Unlock(&diag_lock);
    return true;
}

bool diag_tls_identity_checkpoint(const unsigned char *certificate_der,
                                  size_t certificate_der_length,
                                  const char *source) {
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[SHA256_DIGEST_LENGTH];
    char fingerprint[DIAG_CERT_FINGERPRINT_BUFFER_SIZE];
    unsigned int i;

    if (certificate_der == NULL || certificate_der_length == 0) {
        diag_log("TLS_IDENTITY", "sha256_der=unavailable source=%s",
                 source != NULL ? source : "unknown");
        return false;
    }

    SHA256(certificate_der, certificate_der_length, digest);
    for (i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        fingerprint[i * 2] = hex[digest[i] >> 4];
        fingerprint[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    fingerprint[DIAG_CERT_FINGERPRINT_HEX_LENGTH] = '\0';

    diag_log("TLS_IDENTITY", "sha256_der=%s source=%s",
             fingerprint, source != NULL ? source : "unknown");
    LightLock_Lock(&diag_lock);
    snprintf(diag_tls_fingerprint, sizeof(diag_tls_fingerprint), "%s",
             fingerprint);
    snprintf(diag_tls_source, sizeof(diag_tls_source), "%s",
             source != NULL ? source : "unknown");
    LightLock_Unlock(&diag_lock);
    return true;
}

void diag_tls_route_attempt(const char *route) {
    LightLock_Lock(&diag_lock);
    snprintf(diag_tls_source, sizeof(diag_tls_source), "%s",
             route != NULL ? route : "unknown");
    LightLock_Unlock(&diag_lock);
}

void diag_log_sha256(const char *event, const char *field,
                     const unsigned char *value, size_t value_length) {
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[SHA256_DIGEST_LENGTH];
    char digest_hex[DIAG_CERT_FINGERPRINT_BUFFER_SIZE];
    static const unsigned char empty_value = 0;
    unsigned int i;

    if (value == NULL && value_length != 0) {
        diag_log(event, "%s_length=%lu %s_sha256=unavailable",
                 field != NULL ? field : "value",
                 (unsigned long)value_length,
                 field != NULL ? field : "value");
        return;
    }

    SHA256(value != NULL ? value : &empty_value, value_length, digest);
    for (i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        digest_hex[i * 2] = hex[digest[i] >> 4];
        digest_hex[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    digest_hex[DIAG_CERT_FINGERPRINT_HEX_LENGTH] = '\0';
    diag_log(event, "%s_length=%lu %s_sha256=%s",
             field != NULL ? field : "value",
             (unsigned long)value_length,
             field != NULL ? field : "value", digest_hex);
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

void diag_video_counters_reset(void) {
    __atomic_store_n(&diag_video_decoded_frames, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&diag_video_top_draw_calls, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&diag_video_completed_presentations, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&diag_video_decoder_submitted, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&diag_video_decoder_accepted, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&diag_video_decoder_failed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&diag_video_decoder_last_failure, 0, __ATOMIC_RELEASE);
    diag_video_last_presentation_ms = osGetTime();
    diag_video_freeze_reported = false;
}

void diag_video_frame_decoded(void) {
    __atomic_add_fetch(&diag_video_decoded_frames, 1, __ATOMIC_RELAXED);
}

void diag_video_top_draw_call(void) {
    __atomic_add_fetch(&diag_video_top_draw_calls, 1, __ATOMIC_RELAXED);
}

void diag_video_presentation_completed(void) {
    __atomic_add_fetch(&diag_video_completed_presentations, 1, __ATOMIC_RELAXED);
    diag_video_last_presentation_ms = osGetTime();
    diag_video_freeze_reported = false;
}

void diag_video_decoder_submission(int result, bool accepted) {
    __atomic_add_fetch(&diag_video_decoder_submitted, 1, __ATOMIC_RELAXED);
    if (accepted) {
        __atomic_add_fetch(&diag_video_decoder_accepted, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&diag_video_decoder_failed, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&diag_video_decoder_last_failure, result, __ATOMIC_RELEASE);
        diag_log("DECODER_SUBMIT_FAILED", "result=%d", result);
    }
    const uint32_t decoded = __atomic_load_n(&diag_video_decoded_frames, __ATOMIC_ACQUIRE);
    const uint32_t presented = __atomic_load_n(&diag_video_completed_presentations, __ATOMIC_ACQUIRE);
    const u64 now_ms = osGetTime();
    if (!diag_video_freeze_reported && decoded > presented &&
        now_ms - diag_video_last_presentation_ms >= 3000) {
        diag_video_freeze_reported = true;
        diag_log("FREEZE_DETECTED", "decoded=%u presented=%u submitted=%u accepted=%u failed=%u last_failure=%d",
                 decoded, presented,
                 __atomic_load_n(&diag_video_decoder_submitted, __ATOMIC_ACQUIRE),
                 __atomic_load_n(&diag_video_decoder_accepted, __ATOMIC_ACQUIRE),
                 __atomic_load_n(&diag_video_decoder_failed, __ATOMIC_ACQUIRE),
                 __atomic_load_n(&diag_video_decoder_last_failure, __ATOMIC_ACQUIRE));
    }
}

void diag_video_counters_snapshot(DIAG_VIDEO_COUNTERS *counters) {
    if (counters == NULL) return;
    counters->decoded_frames =
        __atomic_load_n(&diag_video_decoded_frames, __ATOMIC_ACQUIRE);
    counters->top_renderer_draw_calls =
        __atomic_load_n(&diag_video_top_draw_calls, __ATOMIC_ACQUIRE);
    counters->completed_presentations =
        __atomic_load_n(&diag_video_completed_presentations, __ATOMIC_ACQUIRE);
    counters->decoder_submitted =
        __atomic_load_n(&diag_video_decoder_submitted, __ATOMIC_ACQUIRE);
    counters->decoder_accepted =
        __atomic_load_n(&diag_video_decoder_accepted, __ATOMIC_ACQUIRE);
    counters->decoder_failed =
        __atomic_load_n(&diag_video_decoder_failed, __ATOMIC_ACQUIRE);
    counters->decoder_last_failure =
        __atomic_load_n(&diag_video_decoder_last_failure, __ATOMIC_ACQUIRE);
}


void diag_upload_wire_progress(unsigned long long sent, unsigned long long total) {
    __atomic_store_n(&diag_wire_sent, (uint32_t)sent, __ATOMIC_RELAXED);
    __atomic_store_n(&diag_wire_total, (uint32_t)total, __ATOMIC_RELAXED);
}
void diag_upload_wire_snapshot(unsigned long long *sent, unsigned long long *total) {
    if (sent) *sent = __atomic_load_n(&diag_wire_sent, __ATOMIC_RELAXED);
    if (total) *total = __atomic_load_n(&diag_wire_total, __ATOMIC_RELAXED);
}
void diag_session_snapshot_lock(void) { LightLock_Lock(&diag_lock); }
void diag_session_snapshot_unlock(void) { LightLock_Unlock(&diag_lock); }
bool diag_session_path_active(const char *base) {
    bool active;
    LightLock_Lock(&diag_lock);
    active = base != NULL && diag_session_file != NULL && strcmp(base, diag_session_path) == 0;
    LightLock_Unlock(&diag_lock);
    return active;
}
static void session_write_meta(bool ended) {
    if (!diag_session_path[0]) return;
    char path[180], temporary[184];
    snprintf(path, sizeof(path), "%s.meta", diag_session_path);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *file = fopen(temporary, "w");
    if (!file) return;
    fprintf(file, "version=2\nbegin=%llu\nend=%llu\nactive=%d\ngaps=%d\nstream=%d\npart=%u\nbytes=%llu\n",
            diag_session_begin_epoch, ended ? (unsigned long long)time(NULL) : 0,
            !ended, diag_session_capped, diag_session_stream, diag_session_part,
            diag_spool_retained_size(diag_session_path));
    const bool ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
    fclose(file);
    if (ok) rename(temporary, path); else unlink(temporary);
}
static bool session_rotate(void) {
    char path[180];
    if (diag_spool_has_pending(diag_session_path)) return false;
    const unsigned int next = diag_session_part + 1;
    if (next > DIAG_SESSION_PARTS) {
        snprintf(path, sizeof(path), "%s.p%08u", diag_session_path, next - DIAG_SESSION_PARTS);
        unlink(path);
        diag_session_capped = true;
    }
    if (!diag_spool_allow_growth(diag_session_path, DIAG_SESSION_MAX_BYTES)) return false;
    snprintf(path, sizeof(path), "%s.p%08u", diag_session_path, next);
    FILE *next_file = fopen(path, "w");
    if (!next_file) return false;
    fflush(diag_session_file); fsync(fileno(diag_session_file)); fclose(diag_session_file);
    diag_session_file = next_file;
    diag_session_part = next;
    diag_session_bytes = 0;
    session_write_meta(false);
    return true;
}
static void session_critical_write(void) {
    char path[180]; snprintf(path, sizeof(path), "%s.tail", diag_session_path);
    FILE *file = fopen(path, "a"); if (!file) return;
    if (ftell(file) + (long)strlen(diag_line) + 1 > DIAG_CRITICAL_BYTES) {
        fclose(file); file = fopen(path, "w"); if (!file) return;
        for (unsigned int i = 0; i < diag_session_tail_count; ++i) fprintf(file, "%s\n", diag_session_tail[i]);
    } else fprintf(file, "%s\n", diag_line);
    fflush(file); fsync(fileno(file)); fclose(file);
    session_write_meta(false);
}
static void prepare_log_mirror(void) {
    FILE *marker = fopen("sdmc:/3ds/moonlight/.log-v2", "r");
    if (marker) { fclose(marker); return; }
    char archive[160];
    snprintf(archive, sizeof(archive), "sdmc:/3ds/moonlight/moonlight-legacy-%llu.log", (unsigned long long)time(NULL));
    rename(DIAG_LOG_PATH, archive);
    snprintf(archive, sizeof(archive), "sdmc:/3ds/moonlight/moonlight-legacy-previous-%llu.log", (unsigned long long)time(NULL));
    rename(DIAG_LOG_PATH ".1", archive);
    marker = fopen("sdmc:/3ds/moonlight/.log-v2", "w");
    if (marker) { fputs("bounded-mirror-v2\n", marker); fflush(marker); fsync(fileno(marker)); fclose(marker); }
}

void diag_log(const char *event, const char *format, ...) {
    va_list args;
    bool session_write_failed = false;
    LightLock_Lock(&diag_lock);
    va_start(args, format);
    vsnprintf(diag_detail, sizeof(diag_detail), format ? format : "", args);
    va_end(args);

    format_wall_time(diag_wall_time, sizeof(diag_wall_time));
    snprintf(diag_line, sizeof(diag_line), "%s +%08llums %-18s %s record_seq=%u", diag_wall_time,
             (unsigned long long)diag_elapsed_ms(), event ? event : "EVENT",
             diag_detail, ++diag_record_sequence);

    snprintf(diag_recent[diag_recent_next], DIAG_LOG_LINE_MAX, "%s", diag_line);
    diag_recent_next = (diag_recent_next + 1) % DIAG_RECENT_LINE_COUNT;
    if (diag_recent_count < DIAG_RECENT_LINE_COUNT) {
        diag_recent_count++;
    }
    if (__atomic_load_n(&diag_logging_enabled, __ATOMIC_ACQUIRE) && diag_file != NULL) {
        if (ftell(diag_file) >= 4L * 1024L * 1024L) {
            fflush(diag_file); fsync(fileno(diag_file)); fclose(diag_file);
            unlink(DIAG_LOG_PATH ".1"); rename(DIAG_LOG_PATH, DIAG_LOG_PATH ".1");
            diag_file = fopen(DIAG_LOG_PATH, "w");
        }
        const bool mirror_room = diag_file && (!diag_storage_pressure || ftell(diag_file) < 65536L);
        if (diag_file && mirror_room) fputs(diag_line, diag_file);
        if (diag_file && mirror_room) { fputc('\n', diag_file); fflush(diag_file); fsync(fileno(diag_file)); }
    }
    if (__atomic_load_n(&diag_logging_enabled, __ATOMIC_ACQUIRE) &&
        diag_session_file != NULL) {
        const size_t line_bytes = strlen(diag_line) + 1;
        if (event != NULL && (strcmp(event, "STREAM_STARTED") == 0 || strcmp(event, "STREAM_CONFIG") == 0)) {
            diag_session_stream = true;
        }
        if (session_tail_event(event)) {
            unsigned int index;
            if (diag_session_tail_count == DIAG_SESSION_TAIL_COUNT) {
                memmove(diag_session_tail, diag_session_tail + 1, sizeof(diag_session_tail[0]) * (DIAG_SESSION_TAIL_COUNT - 1));
                index = DIAG_SESSION_TAIL_COUNT - 1;
            } else index = diag_session_tail_count++;
            snprintf(diag_session_tail[index], DIAG_FORMATTED_LINE_MAX, "%s", diag_line);
            session_critical_write();
        }
        const bool room = !diag_storage_pressure && (diag_session_bytes + line_bytes <= DIAG_SESSION_MAX_BYTES || session_rotate());
        if (room) {
            if (fputs(diag_line, diag_session_file) == EOF ||
            fputc('\n', diag_session_file) == EOF ||
            fflush(diag_session_file) != 0 ||
            fsync(fileno(diag_session_file)) != 0) {
            fclose(diag_session_file);
            diag_session_file = NULL;
            diag_session_id[0] = '\0';
            session_write_failed = true;
            } else {
                diag_session_bytes += line_bytes;
            }
        } else {
            if (!diag_session_capped) { diag_session_capped = true; session_write_meta(false); }
        }
    }
    LightLock_Unlock(&diag_lock);
    if (session_write_failed)
        diag_log("SESSION_SINK_ERROR", "reason=write_or_sync_failed");
}

bool diag_session_begin(void) {
    char path[128];
    int fd = -1;
    unsigned int attempt;
    if (!diag_is_enabled()) return false;
    if (!diag_spool_allow_growth(DIAG_SESSION_DIR "/session-budget.log", DIAG_CRITICAL_BYTES + 8192)) {
        diag_storage_pressure = true;
        diag_log("STORAGE_PRESSURE", "new_recording=false reason=local_log_budget_full preserve_unsent=true");
        return false;
    }
    LightLock_Lock(&diag_lock);
    if (diag_session_file != NULL) {
        LightLock_Unlock(&diag_lock);
        diag_log("SESSION_SINK_ERROR", "reason=previous_session_still_open");
        return false;
    }
    mkdir(DIAG_SESSION_DIR, 0777);
    for (attempt = 0; attempt < 32; ++attempt) {
        snprintf(diag_session_id, sizeof(diag_session_id), "%08llx-%04x",
                 (unsigned long long)svcGetSystemTick(),
                 (unsigned int)(++diag_session_sequence & 0xffff));
        snprintf(path, sizeof(path), "%s/session-%s.log", DIAG_SESSION_DIR,
                 diag_session_id);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0) break;
    }
    if (fd >= 0) diag_session_file = fdopen(fd, "w");
    if (fd >= 0 && diag_session_file == NULL) close(fd);
    if (diag_session_file == NULL) {
        if (fd >= 0) unlink(path);
        diag_session_id[0] = '\0';
        diag_session_path[0] = '\0';
        diag_session_begin_epoch = 0;
    } else {
        snprintf(diag_session_path, sizeof(diag_session_path), "%s", path);
        diag_session_begin_epoch = (unsigned long long)time(NULL);
        diag_session_bytes = 0;
        diag_storage_pressure = !diag_spool_allow_growth(path, DIAG_SESSION_MAX_BYTES + DIAG_CRITICAL_BYTES + 8192);
        diag_session_capped = diag_storage_pressure;
        diag_record_sequence = 0;
        diag_session_part = 0;
        diag_session_stream = false;
        session_write_meta(false);
        diag_session_tail_count = 0;
        memset(diag_session_tail, 0, sizeof(diag_session_tail));
    }
    LightLock_Unlock(&diag_lock);
    if (diag_session_file == NULL) {
        diag_log("SESSION_SINK_ERROR", "reason=create_failed");
        return false;
    }
    diag_log("SESSION_BEGIN", "scope=connection session_id=%s", diag_session_id);
    if (diag_storage_pressure) diag_log("STORAGE_PRESSURE", "routine_paused=true critical_only=true reason=local_log_budget_exhausted");
    diag_log("SESSION_PHASE", "name=host_contact session_id=%s", diag_session_id);
    return true;
}

bool diag_session_available(void) {
    bool available;
    LightLock_Lock(&diag_lock);
    available = diag_session_file != NULL;
    LightLock_Unlock(&diag_lock);
    return available;
}

bool diag_session_get_info(DIAG_SESSION *session) {
    bool active;
    if (session == NULL) return false;
    LightLock_Lock(&diag_lock);
    active = diag_session_file != NULL && diag_session_id[0] != '\0' &&
             diag_session_path[0] != '\0';
    if (active) {
        fflush(diag_session_file);
        fsync(fileno(diag_session_file));
        memset(session, 0, sizeof(*session));
        snprintf(session->id, sizeof(session->id), "%s", diag_session_id);
        snprintf(session->scope, sizeof(session->scope), "connection");
        snprintf(session->host_identity, sizeof(session->host_identity), "paired-host");
        snprintf(session->path, sizeof(session->path), "%s", diag_session_path);
        session->begin_epoch = diag_session_begin_epoch;
        session->end_epoch = (unsigned long long)time(NULL);
    }
    LightLock_Unlock(&diag_lock);
    return active;
}

void diag_session_record_upload(const DIAG_SESSION *session, bool accepted,
                                const DIAG_UPLOAD_INFO *upload_info) {
    FILE *file;
    char wall_time[40];
    const char *upload_id = upload_info != NULL ? upload_info->upload_id : "";
    const unsigned long long source_bytes = upload_info != NULL ? upload_info->source_bytes : 0;
    const unsigned long long filtered_bytes = upload_info != NULL ? upload_info->filtered_bytes : 0;
    const unsigned long long request_bytes = upload_info != NULL ? upload_info->request_bytes : 0;
    bool active = false;
    if (session == NULL || session->id[0] == '\0' || session->path[0] == '\0')
        return;
    LightLock_Lock(&diag_lock);
    active = diag_session_file != NULL &&
             strcmp(diag_session_id, session->id) == 0;
    LightLock_Unlock(&diag_lock);
    diag_log("DIAG_UPLOAD", "scope=%s session_id=%s upload_id=%s state=%s truncated=%s source_bytes=%llu filtered_bytes=%llu request_bytes=%llu",
             session->scope, session->id, upload_id,
             accepted ? "accepted" : "failed", upload_info && upload_info->truncated ? "true" : "false", source_bytes, filtered_bytes,
             request_bytes);
    if (active) return;
    file = fopen(session->path, "a");
    if (file == NULL) return;
    format_wall_time(wall_time, sizeof(wall_time));
    if (fprintf(file, "%s +%08llums DIAG_UPLOAD scope=%s session_id=%s upload_id=%s state=%s truncated=%s source_bytes=%llu filtered_bytes=%llu request_bytes=%llu\n",
                wall_time, (unsigned long long)diag_elapsed_ms(), session->scope,
                session->id, upload_id, accepted ? "accepted" : "failed",
                upload_info && upload_info->truncated ? "true" : "false", source_bytes, filtered_bytes, request_bytes) >= 0) {
        fflush(file);
        fsync(fileno(file));
    }
    fclose(file);
}

void diag_session_end(const char *outcome, const char *reason) {
    bool active;
    LightLock_Lock(&diag_lock);
    active = diag_session_file != NULL;
    LightLock_Unlock(&diag_lock);
    if (!active) return;
    DIAG_VIDEO_COUNTERS final_counters = {};
    diag_video_counters_snapshot(&final_counters);
    diag_log("FINAL_PRESENTATION", "decoded=%u draw_calls=%u presented=%u submitted=%u accepted=%u failed=%u last_failure=%d",
             final_counters.decoded_frames, final_counters.top_renderer_draw_calls,
             final_counters.completed_presentations, final_counters.decoder_submitted,
             final_counters.decoder_accepted, final_counters.decoder_failed,
             final_counters.decoder_last_failure);
    diag_log("SESSION_END", "scope=connection session_id=%s outcome=%s reason=%s",
             diag_session_id, outcome ? outcome : "failed",
             reason ? reason : "unspecified");
    LightLock_Lock(&diag_lock);
    if (diag_session_file != NULL) {
        session_write_meta(true);
        fflush(diag_session_file);
        fsync(fileno(diag_session_file));
        fclose(diag_session_file);
        diag_session_file = NULL;
    }
    diag_session_id[0] = '\0';
    diag_session_path[0] = '\0';
    diag_session_begin_epoch = 0;
    LightLock_Unlock(&diag_lock);
}

void diag_ui_state(const char *screen, const char *state, bool graphical) {
    char *previous;
    if (screen == NULL || state == NULL) return;
    if (strcmp(screen, "TOP") == 0) previous = diag_ui_top_state;
    else if (strcmp(screen, "BOTTOM") == 0) previous = diag_ui_bottom_state;
    else return;
    if (strcmp(previous, state) == 0) return;
    snprintf(previous, 32, "%s", state);
    diag_log("UI_STATE", "screen=%s state=%s graphical=%d", screen, state,
             graphical ? 1 : 0);
}

void diag_init(void) {
    LightLock_Init(&diag_lock);
    diag_start_tick = svcGetSystemTick();
    diag_phase_tick = diag_start_tick;
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/moonlight", 0777);
    prepare_log_mirror();
    diag_file = fopen(DIAG_LOG_PATH, "a");
    diag_initialized = true;
    diag_log("LOGGER_OPEN", "path=sd:/3ds/moonlight/moonlight.log opened=%s flush=fflush+fsync",
             diag_file != NULL ? "yes" : "no");
    diag_set_phase("app.startup");
}

void diag_set_enabled(bool enabled) {
    __atomic_store_n(&diag_logging_enabled, enabled, __ATOMIC_RELEASE);
}

bool diag_is_enabled(void) {
    return __atomic_load_n(&diag_logging_enabled, __ATOMIC_ACQUIRE);
}

void diag_close(void) {
    LightLock_Lock(&diag_lock);
    if (diag_session_file != NULL) {
        fflush(diag_session_file);
        fsync(fileno(diag_session_file));
        fclose(diag_session_file);
        diag_session_file = NULL;
    }
    if (diag_file != NULL) {
        fflush(diag_file);
        fsync(fileno(diag_file));
        fclose(diag_file);
        diag_file = NULL;
    }
    LightLock_Unlock(&diag_lock);
}

static void diag_stack_append_text(const char *text) {
    while (*text != '\0' &&
           diag_stack_record_length + 1 < sizeof(diag_stack_record)) {
        diag_stack_record[diag_stack_record_length++] = *text++;
    }
}

static void diag_stack_append_hex(uint32_t value) {
    static const char digits[] = "0123456789abcdef";
    int shift;
    diag_stack_append_text("0x");
    for (shift = 28; shift >= 0; shift -= 4) {
        if (diag_stack_record_length + 1 >= sizeof(diag_stack_record)) {
            return;
        }
        diag_stack_record[diag_stack_record_length++] =
            digits[(value >> shift) & 0x0f];
    }
}

void diag_stack_checkpoint(const char *boundary, uintptr_t stack_pointer) {
    const uint32_t sp = (uint32_t)stack_pointer;
    Result query_result;
    uint64_t region_end;
    bool mapping_contains_sample;
    size_t written = 0;
    int fd;

    if (!diag_initialized) {
        return;
    }

    LightLock_Lock(&diag_lock);
    memset(&diag_stack_meminfo, 0, sizeof(diag_stack_meminfo));
    memset(&diag_stack_pageinfo, 0, sizeof(diag_stack_pageinfo));
    query_result =
        svcQueryMemory(&diag_stack_meminfo, &diag_stack_pageinfo, sp);
    region_end =
        (uint64_t)diag_stack_meminfo.base_addr + diag_stack_meminfo.size;
    mapping_contains_sample =
        R_SUCCEEDED(query_result) &&
        region_end <= (uint64_t)UINT32_MAX + 1ULL &&
        sp >= diag_stack_meminfo.base_addr && (uint64_t)sp < region_end;
    diag_stack_record_length = 0;
    diag_stack_append_text("STACKDIAG version=");
    diag_stack_append_text(DIAG_BUILD_VERSION);
    diag_stack_append_text(" boundary=");
    diag_stack_append_text(boundary ? boundary : "unknown");
    diag_stack_append_text(" sp=");
    diag_stack_append_hex(sp);
    diag_stack_append_text(
        " stack_bounds=unavailable reason=svcQueryMemory_not_stack_metadata");
    diag_stack_append_text(" point_headroom_bytes=unavailable");
    diag_stack_append_text(
        " stack_request_source=app.rsf:SystemControlInfo.StackSize stack_request=");
    diag_stack_append_hex(DIAG_MAIN_STACK_REQUESTED_SIZE);
    if (R_SUCCEEDED(query_result)) {
        diag_stack_append_text(" map_base=");
        diag_stack_append_hex(diag_stack_meminfo.base_addr);
        diag_stack_append_text(" map_size=");
        diag_stack_append_hex(diag_stack_meminfo.size);
        diag_stack_append_text(" map_state=");
        diag_stack_append_hex(diag_stack_meminfo.state);
        diag_stack_append_text(" map_perm=");
        diag_stack_append_hex(diag_stack_meminfo.perm);
        diag_stack_append_text(" map_offset_bytes=");
        if (mapping_contains_sample) {
            diag_stack_append_hex(sp - diag_stack_meminfo.base_addr);
        } else {
            diag_stack_append_text("unavailable");
        }
    } else {
        diag_stack_append_text(
            " map_base=unavailable map_size=unavailable map_state=unavailable");
        diag_stack_append_text(
            " map_perm=unavailable map_offset_bytes=unavailable");
    }
    diag_stack_append_text(" query_result=");
    diag_stack_append_hex((uint32_t)query_result);
    diag_stack_append_text(" high_water=not_measured\n");

    fd = (__atomic_load_n(&diag_logging_enabled, __ATOMIC_ACQUIRE) &&
          diag_file != NULL) ? fileno(diag_file) : -1;
    if (fd >= 0 && fflush(diag_file) == 0) {
        while (written < diag_stack_record_length) {
            const ssize_t result =
                write(fd, diag_stack_record + written,
                      diag_stack_record_length - written);
            if (result <= 0) {
                break;
            }
            written += (size_t)result;
        }
        if (written == diag_stack_record_length) {
            fsync(fd);
        }
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

/* A fixed breadcrumb avoids printf-family formatting on the request stack.
 * Caller holds diag_lock. No callback into the logger from this helper. */
static void diag_http_breadcrumb_locked(const char *text) {
    if (__atomic_load_n(&diag_logging_enabled, __ATOMIC_ACQUIRE) &&
        diag_file != NULL) {
        fputs(text, diag_file);
        if (diag_file) { fputc('\n', diag_file); fflush(diag_file); fsync(fileno(diag_file)); }
    }
}

static void diag_copy_text(char *out, size_t capacity, const char *text) {
    size_t count = 0;
    if (capacity == 0) return;
    if (text != NULL) {
        while (count + 1 < capacity && text[count] != '\0') {
            out[count] = text[count];
            count++;
        }
    }
    out[count] = '\0';
}

void diag_http_summary_begin(void) {
    LightLock_Lock(&diag_lock);
    diag_http_fact_count = 0;
    diag_http_dropped = 0;
    diag_http_deferred = true;
    LightLock_Unlock(&diag_lock);
}

void diag_http_summary_flush(int result, bool cancelled) {
    /* Snapshot one record at a time. Never invoke diag_log with diag_lock held.
     * Host init/request producer has returned before this boundary. */
    DiagHttpFact fact;
    unsigned int count, dropped, i;
    LightLock_Lock(&diag_lock);
    diag_http_deferred = false;
    count = diag_http_fact_count;
    dropped = diag_http_dropped;
    LightLock_Unlock(&diag_lock);
    diag_log("HOST_INIT_SUMMARY", "result=%d cancelled=%s requests=%u dropped=%u",
             result, cancelled ? "yes" : "no", count, dropped);
    for (i = 0; i < count; i++) {
        LightLock_Lock(&diag_lock);
        fact = diag_http_facts[i];
        LightLock_Unlock(&diag_lock);
        diag_log("HTTP_SUMMARY",
                 "request=%u call=%s status=%ld transport_result=%d complete=%s",
                 i + 1, fact.call, fact.status, fact.transport,
                 fact.complete ? "yes" : "no");
    }
}

void diag_http_begin(const char *method, const char *safe_url) {
    bool deferred;
    char breadcrumb[64];
    size_t used;
    LightLock_Lock(&diag_lock);
    diag_http_active = true;
    diag_copy_text(diag_http_method, sizeof(diag_http_method), method);
    diag_copy_text(diag_http_call, sizeof(diag_http_call), method);
    used = strlen(diag_http_call);
    if (used + 1 < sizeof(diag_http_call)) {
        diag_http_call[used++] = ' ';
        diag_copy_text(diag_http_call + used, sizeof(diag_http_call) - used,
                       safe_url);
    }
    deferred = diag_http_deferred;
    if (deferred) {
        if (diag_http_fact_count < DIAG_HTTP_FACT_COUNT) {
            DiagHttpFact *fact = &diag_http_facts[diag_http_fact_count++];
            diag_copy_text(fact->call, sizeof(fact->call), diag_http_call);
            fact->status = 0;
            fact->transport = 0;
            fact->complete = false;
        } else {
            diag_http_dropped++;
        }
    }
    snprintf(breadcrumb, sizeof(breadcrumb),
             "HTTP_BEGIN method=%s details=bounded", method ? method : "?");
    diag_http_breadcrumb_locked(breadcrumb);
    LightLock_Unlock(&diag_lock);
    if (!deferred) {
        diag_log("HTTP_REQUEST", "method=%s url=%s", method ? method : "?",
                 safe_url ? safe_url : "?");
    }
}

void diag_http_end(long status_code, int transport_result) {
    bool deferred;
    char method[16];
    LightLock_Lock(&diag_lock);
    deferred = diag_http_deferred;
    snprintf(method, sizeof(method), "%s",
             diag_http_method[0] != '\0' ? diag_http_method : "?");
    if (deferred && diag_http_fact_count > 0 && diag_http_dropped == 0) {
        DiagHttpFact *fact = &diag_http_facts[diag_http_fact_count - 1];
        fact->status = status_code;
        fact->transport = transport_result;
        fact->complete = true;
    }
    diag_http_active = false;
    diag_http_method[0] = '\0';
    diag_http_call[0] = '\0';
    diag_http_breadcrumb_locked("HTTP_END details=bounded");
    LightLock_Unlock(&diag_lock);
    if (!deferred) {
        diag_log("HTTP_STATUS", "method=%s status=%ld transport_result=%d",
                 method, status_code, transport_result);
    }
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
    u64 now;

    LightLock_Lock(&diag_lock);
    /* Sample after locking: a concurrent phase reset must not make now older
     * than diag_phase_tick and produce an unsigned watchdog underflow. */
    now = svcGetSystemTick();
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

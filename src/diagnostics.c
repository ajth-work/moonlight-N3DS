#include "diagnostics.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
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
static char diag_recent[DIAG_RECENT_LINE_COUNT][DIAG_LOG_LINE_MAX];
static unsigned int diag_recent_count = 0;
static unsigned int diag_recent_next = 0;
static u64 diag_start_tick = 0;
static u64 diag_phase_tick = 0;

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

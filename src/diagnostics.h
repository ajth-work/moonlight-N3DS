#ifndef MOONLIGHT_DIAGNOSTICS_H
#define MOONLIGHT_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_WATCHDOG_LIMIT_MS 30000ULL
#define DIAG_BUILD_VERSION "3.0.1-diag1"
#define DIAG_LOG_LINE_MAX 512
#define DIAG_RECENT_LINE_COUNT 8

void diag_init(void);
void diag_close(void);
void diag_log(const char *event, const char *format, ...);
void diag_set_phase(const char *phase);
void diag_phase_outcome(const char *phase, const char *outcome, int result);
void diag_http_begin(const char *method, const char *safe_url);
void diag_http_end(long status_code, int transport_result);
void diag_get_active_http(char *out, unsigned int capacity, bool *active);
void diag_cancel_reset(void);
void diag_request_cancel(void);
bool diag_cancel_requested(void);
bool diag_watchdog_poll(void);
uint64_t diag_elapsed_ms(void);
uint64_t diag_phase_elapsed_ms(void);
void diag_get_state(char *phase_out, unsigned int phase_capacity,
                    char lines_out[DIAG_RECENT_LINE_COUNT][DIAG_LOG_LINE_MAX],
                    unsigned int *line_count, bool *watchdog_expired);

#ifdef __cplusplus
}
#endif

#endif

#ifndef MOONLIGHT_DIAGNOSTICS_H
#define MOONLIGHT_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <openssl/x509.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_WATCHDOG_LIMIT_MS 30000ULL
#define DIAG_BUILD_VERSION "3.6.0-gui12"
#define DIAG_MAIN_STACK_REQUESTED_SIZE 0x40000U
/* app.rsf SystemControlInfo.StackSize */
#define DIAG_LOG_LINE_MAX 512
#define DIAG_RECENT_LINE_COUNT 8
#define DIAG_CERT_FINGERPRINT_HEX_LENGTH 64
#define DIAG_CERT_FINGERPRINT_BUFFER_SIZE (DIAG_CERT_FINGERPRINT_HEX_LENGTH + 1)
#define DIAG_CERT_SOURCE_MAX 16

typedef enum {
    DIAG_CERT_STARTUP,
    DIAG_CERT_PAIRING,
    DIAG_CERT_TLS
} DIAG_CERT_STAGE;

void diag_init(void);
void diag_close(void);
void diag_set_enabled(bool enabled);
bool diag_is_enabled(void);
void diag_log(const char *event, const char *format, ...);
void diag_ui_state(const char *screen, const char *state, bool graphical);
void diag_stack_checkpoint(const char *boundary, uintptr_t stack_pointer);
void diag_set_phase(const char *phase);
void diag_phase_outcome(const char *phase, const char *outcome, int result);
/* Begin before host initialization; flush only after its call stack unwinds. */
void diag_http_summary_begin(void);
void diag_http_summary_flush(int result, bool cancelled);
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
bool diag_cert_fingerprint(X509 *certificate, char *out,
                           unsigned int out_capacity);
bool diag_cert_checkpoint(DIAG_CERT_STAGE stage, X509 *certificate,
                          const char *source, const char *certificate_path,
                          const char *key_path, const char *p12_path);
bool diag_tls_context_checkpoint(const unsigned char *certificate_der,
                                size_t certificate_der_length,
                                const char *source);
bool diag_tls_identity_checkpoint(const unsigned char *certificate_der,
                                  size_t certificate_der_length,
                                  const char *source);
void diag_tls_route_attempt(const char *route);
void diag_log_sha256(const char *event, const char *field,
                     const unsigned char *value, size_t value_length);
void diag_get_cert_panel_state(
    char *pair_fingerprint, unsigned int pair_fingerprint_capacity,
    char *pair_source, unsigned int pair_source_capacity,
    char *tls_fingerprint, unsigned int tls_fingerprint_capacity,
    char *tls_source, unsigned int tls_source_capacity);

#ifdef __cplusplus
}
#endif

#endif

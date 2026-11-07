#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
bool diag_spool_allow_growth(const char *current_base, size_t amount);
bool diag_spool_has_pending(const char *base);
bool diag_session_path_active(const char *base);
void diag_session_snapshot_lock(void);
void diag_session_snapshot_unlock(void);
#ifdef __cplusplus
}
#endif

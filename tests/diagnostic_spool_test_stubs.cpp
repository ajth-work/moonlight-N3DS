#include "diagnostic_spool.h"
extern "C" bool diag_session_path_active(const char *) { return false; }
extern "C" void diag_session_snapshot_lock(void) {}
extern "C" void diag_session_snapshot_unlock(void) {}

#include "../src/diagnostic_inventory.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using DiagnosticInventory::Entry;

int main() {
    setenv("TZ", "America/Los_Angeles", 1);
    tzset();
    assert(DiagnosticInventory::format_local_timestamp("2026-01-01T00:30:00Z") ==
           "2025-12-31 16:30:00 -0800");
    assert(DiagnosticInventory::format_local_timestamp("bad") == "unknown");

    Entry missing;
    DiagnosticInventory::parse_line(missing, "ordinary line\n");
    assert(missing.session == "unknown" && missing.start_local == "unknown");
    assert(missing.partial == "unknown" && missing.upload_state == "unknown");

    Entry recorded;
    DiagnosticInventory::parse_line(recorded,
        "2026-01-01T00:30:00.000Z +00000000ms SESSION_BEGIN scope=stream session_id=abc\n");
    DiagnosticInventory::parse_line(recorded,
        "2026-01-01T00:30:02.000Z +00002000ms SESSION_END scope=stream session_id=abc duration_ms=2000\n");
    DiagnosticInventory::parse_line(recorded,
        "2026-01-01T00:30:02.000Z DIAG_UPLOAD state=accepted truncated=1\n");
    assert(recorded.scope == "stream" && recorded.session == "abc");
    assert(recorded.start_local == "2026-01-01T00:30:00Z");
    assert(recorded.end_local == "2026-01-01T00:30:02Z");
    assert(recorded.partial == "yes" && recorded.upload_state == "accepted");

    char temp[] = "/tmp/moonlight-inventory-XXXXXX";
    char *root = mkdtemp(temp);
    assert(root != NULL);
    std::string path = std::string(root) + "/moonlight.log";
    { std::ofstream out(path);
      out << "2026-01-01T00:30:00.000Z +0 SESSION_BEGIN scope=stream session_id=abc\n";
      out << "2026-01-01T00:30:02.000Z DIAG_UPLOAD state=accepted\n"; }
    struct stat st;
    assert(stat(path.c_str(), &st) == 0);
    const std::vector<Entry> entries = DiagnosticInventory::list(root);
    assert(entries.size() == 1 && entries[0].filename == "moonlight.log");
    assert(entries[0].size == static_cast<uint64_t>(st.st_size));
    assert(entries[0].scope == "application log");
    assert(entries[0].session == "unknown" && entries[0].upload_state == "unknown");
    assert(entries[0].start_local == "unknown" && entries[0].end_local == "unknown");
    assert(entries[0].modified_local != "unknown");
    assert(mkdir((std::string(root) + "/sessions").c_str(), 0700) == 0);
    const std::string snapshot = std::string(root) + "/sessions/stream-abc.log";
    { std::ofstream out(snapshot); out << "2026-01-01T00:30:00.000Z +0 SESSION_BEGIN scope=stream session_id=abc\n";
      out << "2026-01-01T00:30:02.000Z +2 SESSION_END scope=stream session_id=abc\n"; }
    const std::vector<Entry> with_snapshot = DiagnosticInventory::list(root);
    assert(with_snapshot.size() == 2);
    assert(with_snapshot[1].filename == "sessions/stream-abc.log");
    assert(with_snapshot[1].session == "abc" && with_snapshot[1].scope == "stream");
    assert(with_snapshot[1].duration == "2 s");
    const std::string interrupted = std::string(root) + "/sessions/session-cut.log";
    { std::ofstream out(interrupted); out << "2026-01-01T00:30:00.000Z +0 SESSION_BEGIN scope=connection session_id=cut\n"; }
    const std::vector<Entry> with_interrupted = DiagnosticInventory::list(root);
    assert(with_interrupted.size() == 3);
    bool found_interrupted = false;
    for (const Entry &entry : with_interrupted) {
        if (entry.filename == "sessions/session-cut.log") {
            found_interrupted = true;
            assert(entry.completion == "interrupted/incomplete");
            assert(entry.partial == "yes");
        }
    }
    assert(found_interrupted);
    unlink(path.c_str());
    unlink(snapshot.c_str());
    unlink(interrupted.c_str());
    rmdir((std::string(root) + "/sessions").c_str());
    rmdir(root);
}

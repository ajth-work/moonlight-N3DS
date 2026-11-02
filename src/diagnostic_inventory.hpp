#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace DiagnosticInventory {

struct Entry {
    std::string filename;
    std::string scope = "unknown";
    std::string session = "unknown";
    uint64_t size = 0;
    std::string modified_local = "unknown";
    std::string start_local = "unknown";
    std::string end_local = "unknown";
    std::string duration = "unknown";
    std::string completion = "unknown";
    std::string partial = "unknown";
    std::string upload_state = "unknown";
};

/* Parse only explicitly recorded diagnostic facts. */
void parse_line(Entry &entry, const std::string &line);
std::string format_local_timestamp(const std::string &utc_timestamp);
/* Lists regular files in root and its sessions subdirectory, capped at 64. */
std::vector<Entry> list(const std::string &root, size_t limit = 64,
                        size_t *total_count = nullptr);

} // namespace DiagnosticInventory

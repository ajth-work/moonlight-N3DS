#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace DiagnosticTransfer {
constexpr std::size_t ChunkLimit = 240 * 1024;
constexpr std::size_t SessionLimit = 8 * 1024 * 1024;
struct Response {
    bool transport_ok = false;
    long status = 0;
    std::string body;
};
struct Progress {
    std::string phase;
    uint64_t acknowledged = 0, total = 0, prepared = 0, source = 0;
    uint64_t local_entries_checked = 0, local_retained_bytes = 0,
             local_snapshot_bytes = 0;
    unsigned part = 0, parts = 0;
};
struct Capture {
    std::string path, session_id, build;
    uint64_t begin = 0, end = 0;
};
struct Result {
    bool accepted = false, complete = false, unclean = false, unsupported = false,
         cancelled = false;
    std::string capture_id, error;
    uint64_t source_bytes = 0, content_bytes = 0, request_bytes = 0;
};
using Transport = std::function<Response(const std::string &, const std::string &, bool)>;
using Notify = std::function<void(const Progress &)>;
using Cancel = std::function<bool()>;
Result upload(const Capture &capture, const Transport &transport, const Notify &notify,
              const Cancel &cancel);
std::map<std::string, std::string> parse_response(const std::string &body);
std::map<std::string, std::string> read_meta(const std::string &path);
uint64_t retained_size(const std::string &base);
bool is_sidecar(const std::string &name);
void record_receipt(const std::string &base, const Result &result);
} // namespace DiagnosticTransfer

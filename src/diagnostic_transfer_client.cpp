#include "diagnostic_transfer_client.hpp"
#include "diagnostic_spool.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif
#include <openssl/evp.h>
#include <vector>

namespace DiagnosticTransfer {
namespace {
struct Source {
    std::string path;
    uint64_t size = 0;
};
struct Frozen {
    std::vector<Source> sources;
    bool complete = false, unclean = false;
    std::string build;
    uint64_t begin = 0, end = 0;
};
uint64_t size_of(const std::string &path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0
               ? static_cast<uint64_t>(st.st_size)
               : 0;
}
bool exists(const std::string &path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
std::string dirname(const std::string &p) {
    const auto n = p.find_last_of('/');
    return n == std::string::npos ? "." : p.substr(0, n);
}
std::string basename(const std::string &p) { return p.substr(p.find_last_of('/') + 1); }
void durable(FILE *f) {
    if (fflush(f) != 0 ||
#ifdef _WIN32
        _commit(_fileno(f)) != 0
#else
        fsync(fileno(f)) != 0
#endif
    )
        throw std::runtime_error("SD sync failed");
}
void atomic_text(const std::string &path, const std::string &text) {
    const auto temp = path + ".tmp";
    FILE *f = fopen(temp.c_str(), "wb");
    if (!f)
        throw std::runtime_error("SD metadata unavailable");
    try {
        if (fwrite(text.data(), 1, text.size(), f) != text.size())
            throw std::runtime_error("SD metadata write failed");
        durable(f);
        fclose(f);
        f = nullptr;
#ifdef _WIN32
        if (!MoveFileExA(temp.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
#else
        if (rename(temp.c_str(), path.c_str()) != 0)
#endif
            throw std::runtime_error("SD metadata commit failed");
    } catch (...) {
        if (f)
            fclose(f);
        unlink(temp.c_str());
        throw;
    }
}
struct Hash {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    Hash() {
        if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("Hash unavailable");
    }
    void add(const std::string &s) {
        if (EVP_DigestUpdate(ctx.get(), s.data(), s.size()) != 1)
            throw std::runtime_error("Hash failed");
    }
    std::string finish() {
        unsigned char bytes[EVP_MAX_MD_SIZE];
        unsigned length = 0;
        if (EVP_DigestFinal_ex(ctx.get(), bytes, &length) != 1 || length != 32)
            throw std::runtime_error("Hash failed");
        static const char hex[] = "0123456789abcdef";
        std::string out;
        for (unsigned i = 0; i < length; ++i) {
            out += hex[bytes[i] >> 4];
            out += hex[bytes[i] & 15];
        }
        return out;
    }
};
std::string hash(const std::string &s) {
    Hash h;
    h.add(s);
    return h.finish();
}
std::string quote(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c >= 32)
            out += static_cast<char>(c);
    }
    return out + '"';
}
bool allowed_source(const std::string &base, const std::string &source) {
    if (source == base)
        return true;
    if (source.compare(0, base.size() + 1, base + ".") != 0)
        return false;
    return source.substr(base.size() + 1).find_first_of("/\\") == std::string::npos;
}
std::vector<std::string> parts(const std::string &base) {
    std::vector<std::string> paths;
    const auto directory = dirname(base), prefix = basename(base) + ".p";
    DIR *dir = opendir(directory.c_str());
    if (!dir)
        return paths;
    while (auto *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() == prefix.size() + 8 && name.compare(0, prefix.size(), prefix) == 0 &&
            std::all_of(name.begin() + prefix.size(), name.end(),
                        [](unsigned char c) { return c >= '0' && c <= '9'; }) &&
            exists(directory + "/" + name))
            paths.push_back(directory + "/" + name);
    }
    closedir(dir);
    std::sort(paths.begin(), paths.end());
    return paths;
}
void copy_prefix(const std::string &source, const std::string &dest, uint64_t bytes,
                 const Cancel &cancel) {
    FILE *in = fopen(source.c_str(), "rb"), *out = fopen((dest + ".tmp").c_str(), "wb");
    if (!in || !out) {
        if (in)
            fclose(in);
        if (out)
            fclose(out);
        throw std::runtime_error("SD snapshot unavailable");
    }
    std::array<char, 16384> buffer;
    try {
        while (bytes) {
            if (cancel())
                throw std::runtime_error("Cancelled");
            const auto n = static_cast<size_t>(std::min<uint64_t>(bytes, buffer.size()));
            if (fread(buffer.data(), 1, n, in) != n || fwrite(buffer.data(), 1, n, out) != n)
                throw std::runtime_error("SD snapshot incomplete");
            bytes -= n;
        }
        durable(out);
        fclose(in);
        in = nullptr;
        fclose(out);
        out = nullptr;
        if (rename((dest + ".tmp").c_str(), dest.c_str()) != 0)
            throw std::runtime_error("SD snapshot commit failed");
    } catch (...) {
        if (in)
            fclose(in);
        if (out)
            fclose(out);
        unlink((dest + ".tmp").c_str());
        throw;
    }
}
Frozen freeze(const Capture &c, const Cancel &cancel) {
    Frozen f;
    if (exists(c.path + ".capture")) {
        std::ifstream input(c.path + ".capture");
        std::string magic;
        input >> magic >> std::quoted(f.build) >> f.begin >> f.end >> f.complete >> f.unclean;
        if (magic != "LOGCAP2" || f.build.size() > 64)
            throw std::runtime_error("Invalid pending capture");
        Source s;
        while (input >> std::quoted(s.path) >> s.size) {
            if (!allowed_source(c.path, s.path) || s.size > SessionLimit ||
                size_of(s.path) < s.size || f.sources.size() >= 34)
                throw std::runtime_error("Pending capture source missing");
            f.sources.push_back(s);
        }
        if (f.sources.empty())
            throw std::runtime_error("Pending capture empty");
        return f;
    }
    const bool active = diag_session_path_active(c.path.c_str());
    struct SnapshotLock {
        SnapshotLock() { diag_session_snapshot_lock(); }
        ~SnapshotLock() { diag_session_snapshot_unlock(); }
    } snapshot_lock;
    const auto meta = read_meta(c.path + ".meta");
    atomic_text(c.path + ".pin", "preparing\n");
    const bool managed = meta.count("version") && meta.at("version") == "2";
    f.complete = managed && meta.at("active") == "0" && meta.at("gaps") == "0";
    f.unclean = managed && meta.at("active") == "1" && !active;
    f.build = c.build;
    f.begin = c.begin;
    f.end = std::max(c.begin, c.end);
    if (managed && meta.count("end") && meta.at("end") != "0")
        f.end = std::max<uint64_t>(f.begin, std::strtoull(meta.at("end").c_str(), nullptr, 10));
    auto paths = parts(c.path);
    if (managed && meta.count("part") &&
        paths.size() != std::strtoul(meta.at("part").c_str(), nullptr, 10))
        f.complete = false;
    paths.insert(paths.begin(), c.path);
    uint64_t total = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        uint64_t bytes = size_of(paths[i]);
        if (!bytes)
            continue;
        if (bytes > SessionLimit - 128 * 1024) {
            bytes = SessionLimit - 128 * 1024;
            f.complete = false;
        }
        if (total + bytes > SessionLimit - 128 * 1024) {
            f.complete = false;
            break;
        }
        if (i + 1 == paths.size()) {
            if (!diag_spool_allow_growth(c.path.c_str(), static_cast<size_t>(bytes)))
                throw std::runtime_error("SD log storage full");
            copy_prefix(paths[i], c.path + ".snapshot", bytes, cancel);
            f.sources.push_back({c.path + ".snapshot", bytes});
        } else
            f.sources.push_back({paths[i], bytes});
        total += bytes;
    }
    const auto tail = size_of(c.path + ".tail");
    if (tail && tail <= 64 * 1024) {
        if (!diag_spool_allow_growth(c.path.c_str(), static_cast<size_t>(tail)))
            throw std::runtime_error("SD log storage full");
        copy_prefix(c.path + ".tail", c.path + ".snapshot-tail", tail, cancel);
        f.sources.push_back({c.path + ".snapshot-tail", tail});
    }
    std::ostringstream out;
    out << "LOGCAP2 " << std::quoted(f.build) << ' ' << f.begin << ' ' << f.end << ' ' << f.complete
        << ' ' << f.unclean << '\n';
    for (const auto &source : f.sources)
        out << std::quoted(source.path) << ' ' << source.size << '\n';
    atomic_text(c.path + ".capture", out.str());
    unlink((c.path + ".pin").c_str());
    return f;
}
bool sensitive(const std::string &line) {
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char *word : {"pin", "secret", "private", "credential", "auth", "cert", "key",
                             "pairing", "uniqueid", "uuid=", "?"})
        if (lower.find(word) != std::string::npos)
            return true;
    return false;
}
// One bounded source read and one request-sized chunk; no whole-session buffers.
void chunks(const Frozen &f, const std::function<void(const std::string &)> &emit,
            const std::function<void(uint64_t)> &progress, const Cancel &cancel) {
    std::string chunk;
    chunk.reserve(ChunkLimit);
    auto line = [&](const std::string &s) {
        if (sensitive(s))
            return;
        if (chunk.size() + s.size() + 1 > ChunkLimit) {
            emit(chunk);
            chunk.clear();
        }
        chunk += s;
        chunk += '\n';
    };
    line(std::string("CAPTURE_CONTEXT recording_complete=") + (f.complete ? "true" : "false") +
         " unclean_end=" + (f.unclean ? "true" : "false") +
         " critical_journal_may_repeat_events=true");
    if (!f.complete)
        line("CAPTURE_GAP retained history may omit older records or an unclean final interval");
    uint64_t processed = 0;
    std::array<char, 16384> buffer;
    for (const auto &source : f.sources) {
        FILE *file = fopen(source.path.c_str(), "rb");
        if (!file)
            throw std::runtime_error("SD capture source unavailable");
        std::string current;
        bool overflow = false;
        uint64_t remaining = source.size;
        try {
            while (remaining) {
                if (cancel())
                    throw std::runtime_error("Cancelled");
                const auto n = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
                if (fread(buffer.data(), 1, n, file) != n)
                    throw std::runtime_error("SD capture source changed");
                remaining -= n;
                processed += n;
                for (size_t i = 0; i < n; ++i) {
                    if (buffer[i] == '\n') {
                        if (!overflow)
                            line(current);
                        current.clear();
                        overflow = false;
                    } else if (current.size() < 4096)
                        current += buffer[i];
                    else
                        overflow = true;
                }
                progress(processed);
            }
            fclose(file);
            file = nullptr;
        } catch (...) {
            if (file)
                fclose(file);
            throw;
        }
        // A torn final record is never combined with the next source file.
    }
    if (!chunk.empty())
        emit(chunk);
}
uint64_t number(const std::map<std::string, std::string> &m, const std::string &key) {
    const auto i = m.find(key);
    if (i == m.end() || i->second.empty() || i->second.size() > 20 ||
        !std::all_of(i->second.begin(), i->second.end(),
                     [](unsigned char c) { return c >= '0' && c <= '9'; }))
        throw std::runtime_error("Invalid host receipt");
    return std::strtoull(i->second.c_str(), nullptr, 10);
}
} // namespace

std::map<std::string, std::string> read_meta(const std::string &path) {
    std::map<std::string, std::string> out;
    if (size_of(path) > 64 * 1024)
        return out;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        const auto equal = line.find('=');
        if (equal != std::string::npos)
            out[line.substr(0, equal)] = line.substr(equal + 1);
    }
    return out;
}
bool is_sidecar(const std::string &name) {
    return name.find(".log.") != std::string::npos || name == ".log-v2" ||
           name == "moonlight.log.1";
}
uint64_t retained_size(const std::string &base) {
    uint64_t total = size_of(base) + size_of(base + ".tail");
    for (const auto &p : parts(base))
        total += size_of(p);
    return total;
}
std::map<std::string, std::string> parse_response(const std::string &body) {
    if (body.size() > 1024)
        throw std::runtime_error("Host response exceeds limit");
    std::map<std::string, std::string> fields;
    size_t p = 0;
    auto ws = [&] {
        while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p])))
            ++p;
    };
    auto str = [&]() -> std::string {
        if (p >= body.size() || body[p++] != '"')
            throw std::runtime_error("Invalid host JSON");
        std::string s;
        while (p < body.size() && body[p] != '"') {
            if (body[p] == '\\' || static_cast<unsigned char>(body[p]) < 32)
                throw std::runtime_error("Unsupported host JSON");
            s += body[p++];
        }
        if (p >= body.size())
            throw std::runtime_error("Invalid host JSON");
        ++p;
        return s;
    };
    ws();
    if (p >= body.size() || body[p++] != '{')
        throw std::runtime_error("Invalid host JSON");
    ws();
    while (p < body.size() && body[p] != '}') {
        auto key = str();
        ws();
        if (p >= body.size() || body[p++] != ':')
            throw std::runtime_error("Invalid host JSON");
        ws();
        std::string value;
        if (p < body.size() && body[p] == '"')
            value = str();
        else {
            const auto start = p;
            while (p < body.size() && body[p] != ',' && body[p] != '}' &&
                   !std::isspace(static_cast<unsigned char>(body[p])))
                ++p;
            value = body.substr(start, p - start);
            if (value.empty() ||
                (!std::all_of(value.begin(), value.end(),
                              [](unsigned char c) { return c >= '0' && c <= '9'; }) &&
                 value != "true" && value != "false"))
                throw std::runtime_error("Invalid host JSON value");
        }
        if (!fields.emplace(key, value).second)
            throw std::runtime_error("Duplicate host JSON field");
        ws();
        if (p < body.size() && body[p] == ',') {
            ++p;
            ws();
            if (p == body.size() || body[p] == '}')
                throw std::runtime_error("Invalid host JSON");
        } else
            break;
    }
    if (p >= body.size() || body[p++] != '}')
        throw std::runtime_error("Invalid host JSON");
    ws();
    if (p != body.size())
        throw std::runtime_error("Invalid host JSON");
    return fields;
}
void record_receipt(const std::string &base, const Result &result) {
    std::ostringstream out;
    out << "state="
        << (result.accepted    ? "accepted"
            : result.cancelled ? "paused"
                               : "failed")
        << "\ncapture=" << result.capture_id << "\nrecording_complete=" << result.complete
        << "\nunclean=" << result.unclean << "\ncontent_bytes=" << result.content_bytes
        << "\nsource_bytes=" << result.source_bytes << '\n';
    atomic_text(base + ".receipt", out.str());
    if (result.accepted) {
        unlink((base + ".capture").c_str());
        unlink((base + ".snapshot").c_str());
        unlink((base + ".snapshot-tail").c_str());
        unlink((base + ".pin").c_str());
    }
}
Result upload(const Capture &capture, const Transport &transport, const Notify &notify,
              const Cancel &cancel) {
    Result result;
    Progress progress;
    auto request = [&](const std::string &path, const std::string &body, bool binary) -> Response {
        if (cancel())
            throw std::runtime_error("Cancelled");
        result.request_bytes += body.size();
        auto response = transport(path, body, binary);
        if (!response.transport_ok && (response.status == 0 || response.status >= 500) &&
            !cancel()) {
            result.request_bytes += body.size();
            response = transport(path, body, binary);
        }
        return response;
    };
    try {
        progress.phase = "Connecting to Sunshine";
        notify(progress);
        auto cap = request("/client-diagnostics/v2/capabilities", "{}", false);
        if (cap.status == 404) {
            result.unsupported = true;
            return result;
        }
        if (!cap.transport_ok || cap.status != 200)
            throw std::runtime_error("Sunshine capability request failed");
        const auto capability = parse_response(cap.body);
        if (capability.at("version") != "2" || number(capability, "chunk_limit") < ChunkLimit)
            throw std::runtime_error("Unsupported host protocol");
        progress.phase = "Preparing local SD log";
        notify(progress);
        const auto frozen = freeze(capture, cancel);
        result.complete = frozen.complete;
        result.unclean = frozen.unclean;
        for (const auto &source : frozen.sources)
            result.source_bytes += source.size;
        if (result.source_bytes > SessionLimit)
            throw std::runtime_error("Capture exceeds local limit");
        progress.source = result.source_bytes;
        std::vector<uint64_t> sizes;
        std::vector<std::string> hashes;
        Hash full;
        chunks(
            frozen,
            [&](const std::string &part) {
                full.add(part);
                sizes.push_back(part.size());
                hashes.push_back(hash(part));
                result.content_bytes += part.size();
            },
            [&](uint64_t done) {
                progress.prepared = done;
                notify(progress);
            },
            cancel);
        if (sizes.empty() || sizes.size() > 128 || result.content_bytes > SessionLimit)
            throw std::runtime_error("Capture chunk limit exceeded");
        const auto full_hash = full.finish();
        result.capture_id =
            hash(capture.session_id + "|" + frozen.build + "|" + std::to_string(frozen.begin) +
                 "|" + std::to_string(frozen.end) + "|" + full_hash)
                .substr(0, 32);
        std::ostringstream manifest;
        manifest << "{\"version\":2,\"capture_id\":" << quote(result.capture_id)
                 << ",\"session_id\":" << quote(capture.session_id)
                 << ",\"scope\":\"host\",\"parent_id\":\"\",\"build\":" << quote(frozen.build)
                 << ",\"begin\":" << frozen.begin << ",\"end\":" << frozen.end
                 << ",\"source_bytes\":" << result.source_bytes
                 << ",\"content_bytes\":" << result.content_bytes
                 << ",\"content_sha256\":" << quote(full_hash)
                 << ",\"recording_complete\":" << (result.complete ? "true" : "false")
                 << ",\"unclean_end\":" << (result.unclean ? "true" : "false")
                 << ",\"chunk_sizes\":[";
        for (size_t i = 0; i < sizes.size(); ++i) {
            if (i)
                manifest << ',';
            manifest << sizes[i];
        }
        manifest << "],\"chunk_sha256\":[";
        for (size_t i = 0; i < hashes.size(); ++i) {
            if (i)
                manifest << ',';
            manifest << quote(hashes[i]);
        }
        manifest << "]}";
        const auto base = "/client-diagnostics/v2/" + result.capture_id;
        auto init = request(base + "/init", manifest.str(), false);
        if (!init.transport_ok || init.status != 200)
            throw std::runtime_error(init.status == 507 ? "Sunshine pending-log storage full"
                                                        : "Sunshine rejected capture manifest");
        auto status = parse_response(init.body);
        const auto bitmap = status.at("bitmap");
        uint64_t expected_ack = 0;
        for (size_t i = 0; i < bitmap.size() && i < sizes.size(); ++i)
            if (bitmap[i] == '1')
                expected_ack += sizes[i];
        if (status.at("capture_id") != result.capture_id ||
            number(status, "acknowledged_bytes") != expected_ack || bitmap.size() != sizes.size() ||
            bitmap.find_first_not_of("01") != std::string::npos)
            throw std::runtime_error("Invalid host resume bitmap");
        progress.phase = "Sending to Sunshine";
        progress.total = result.content_bytes;
        progress.parts = sizes.size();
        progress.acknowledged = number(status, "acknowledged_bytes");
        notify(progress);
        unsigned index = 0;
        chunks(
            frozen,
            [&](const std::string &part) {
                if (index >= hashes.size() || part.size() != sizes[index] ||
                    hash(part) != hashes[index])
                    throw std::runtime_error("Local capture changed; upload paused");
                progress.part = index + 1;
                notify(progress);
                if (bitmap[index] != '1') {
                    auto sent = request(base + "/part/" + std::to_string(index), part, true);
                    if (!sent.transport_ok || sent.status != 200)
                        throw std::runtime_error("Part not acknowledged; upload can resume");
                    auto reply = parse_response(sent.body);
                    if (reply.at("capture_id") != result.capture_id ||
                        reply.at("bitmap").size() != sizes.size() ||
                        reply.at("bitmap")[index] != '1')
                        throw std::runtime_error("Invalid part receipt");
                    progress.acknowledged = number(reply, "acknowledged_bytes");
                    notify(progress);
                    atomic_text(capture.path + ".ack",
                                "capture=" + result.capture_id +
                                    "\nbytes=" + std::to_string(progress.acknowledged) + "\n");
                }
                ++index;
            },
            [](uint64_t) {}, cancel);
        progress.phase = "Verifying full capture";
        notify(progress);
        auto done = request(base + "/finish", "{}", false);
        if (!done.transport_ok || done.status != 200)
            throw std::runtime_error("Sunshine has not verified the full capture");
        auto receipt = parse_response(done.body);
        if (receipt.at("capture_id") != result.capture_id ||
            receipt.at("transfer_complete") != "true" ||
            receipt.at("content_sha256") != full_hash ||
            number(receipt, "content_bytes") != result.content_bytes ||
            receipt.at("recording_complete") != (result.complete ? "true" : "false"))
            throw std::runtime_error("Capture receipt mismatch");
        result.accepted = true;
        progress.acknowledged = result.content_bytes;
        progress.phase = result.complete ? "Complete capture accepted"
                                         : "Retained log accepted; partial history";
        notify(progress);
        record_receipt(capture.path, result);
    } catch (const std::exception &error) {
        result.cancelled = cancel();
        result.error = result.cancelled ? "Paused; resume from Check Logs" : error.what();
        if (!exists(capture.path + ".capture"))
            unlink((capture.path + ".pin").c_str());
        try {
            record_receipt(capture.path, result);
        } catch (...) {
        }
    }
    return result;
}
} // namespace DiagnosticTransfer

extern "C" bool diag_spool_has_pending(const char *base) {
    return base && (access((std::string(base) + ".capture").c_str(), F_OK) == 0 ||
                    access((std::string(base) + ".pin").c_str(), F_OK) == 0);
}
extern "C" unsigned long long diag_spool_retained_size(const char *base) {
    return base ? DiagnosticTransfer::retained_size(base) : 0;
}
extern "C" bool diag_spool_allow_growth(const char *current_base, size_t amount) {
    using namespace DiagnosticTransfer;
    if (!current_base)
        return false;
    const std::string base = current_base;
    auto root = base.find("/sessions/") != std::string::npos
                    ? base.substr(0, base.find("/sessions/"))
                    : base.substr(0, base.find_last_of('/'));
    uint64_t bytes = 0;
    std::vector<std::pair<time_t, std::string>> removable;
    for (const auto &dirpath : {root, root + "/sessions"}) {
        DIR *dir = opendir(dirpath.c_str());
        if (!dir)
            continue;
        while (auto *entry = readdir(dir)) {
            const std::string name = entry->d_name, path = dirpath + "/" + name;
            if (name[0] == '.' ||
                (name.rfind("session-", 0) != 0 && name.rfind("moonlight", 0) != 0))
                continue;
            struct stat st;
            if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            bytes += static_cast<uint64_t>(st.st_size);
            if (name.size() > 4 && name.substr(name.size() - 4) == ".log" && path != base &&
                !diag_spool_has_pending(path.c_str())) {
                auto receipt = read_meta(path + ".receipt");
                auto meta = read_meta(path + ".meta");
                if (receipt["state"] == "accepted" && meta["active"] == "0")
                    removable.emplace_back(st.st_mtime, path);
            }
        }
        closedir(dir);
    }
    std::sort(removable.begin(), removable.end());
    for (const auto &candidate : removable) {
        if (bytes + amount <= StorageLimit)
            break;
        const auto parent = candidate.second.substr(0, candidate.second.find_last_of('/')),
                   prefix = candidate.second.substr(candidate.second.find_last_of('/') + 1);
        DIR *dir = opendir(parent.c_str());
        if (!dir)
            continue;
        while (auto *entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name == prefix || name.compare(0, prefix.size() + 1, prefix + ".") == 0) {
                const auto path = parent + "/" + name;
                struct stat st;
                if (lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                    unlink(path.c_str()) == 0)
                    bytes -= static_cast<uint64_t>(st.st_size);
            }
        }
        closedir(dir);
    }
    return bytes + amount <= StorageLimit;
}

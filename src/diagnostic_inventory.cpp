#include "diagnostic_inventory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

namespace DiagnosticInventory {
namespace {
const size_t MAX_FILES = 64;
const size_t MAX_SCAN_BYTES = 256 * 1024;
const size_t MAX_LINE = 2048;

std::string value_for(const std::string &line, const char *key) {
    const std::string needle = std::string(key) + "=";
    size_t at = line.find(needle);
    if (at == std::string::npos) return "";
    at += needle.size();
    size_t end = line.find_first_of(" \t\r\n", at);
    return line.substr(at, end == std::string::npos ? end : end - at);
}

bool parse_utc(const std::string &s, struct tm *tm_out) {
    if (s.size() < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' ||
        s[13] != ':' || s[16] != ':' || s.back() != 'Z') return false;
    char base[20];
    memcpy(base, s.data(), 19);
    base[19] = '\0';
    memset(tm_out, 0, sizeof(*tm_out));
    char *end = strptime(base, "%Y-%m-%dT%H:%M:%S", tm_out);
    return end != NULL && *end == '\0';
}

std::string timestamp_prefix(const std::string &line) {
    if (line.size() < 20 || line[19] != '.') return "";
    const size_t z = line.find('Z', 20);
    if (z == std::string::npos || z > 32) return "";
    return line.substr(0, 19) + "Z";
}

void scan_file(const std::string &path, Entry &entry) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return;
    char buffer[MAX_LINE];
    size_t consumed = 0;
    while (consumed < MAX_SCAN_BYTES && fgets(buffer, sizeof(buffer), f)) {
        size_t n = strlen(buffer);
        consumed += n;
        parse_line(entry, std::string(buffer, n));
        if (n == sizeof(buffer) - 1 && buffer[n - 1] != '\n') {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') ++consumed;
        }
    }
    fclose(f);
}

void add_directory(const std::string &path, std::vector<Entry> &out,
                   size_t limit, size_t &total_count) {
    DIR *dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        if (item->d_name[0] == '.') continue;
        const std::string full = path + "/" + item->d_name;
        struct stat st;
        if (lstat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        ++total_count;
        if (out.size() >= limit) continue;
        Entry e;
        e.filename = path.size() >= 9 && path.compare(path.size() - 9, 9, "/sessions") == 0
                         ? std::string("sessions/") + item->d_name
                         : item->d_name;
        e.size = static_cast<uint64_t>(st.st_size);
        if (e.filename == "moonlight.log") e.scope = "application log";
        struct tm modified;
        char modified_text[40];
        if (localtime_r(&st.st_mtime, &modified) != NULL &&
            strftime(modified_text, sizeof(modified_text), "%Y-%m-%d %H:%M:%S %z", &modified) != 0)
            e.modified_local = modified_text;
        /* The append-only application log can contain many sessions; do not
         * misattribute its last session's facts to the whole file. */
        if (e.filename != "moonlight.log") scan_file(full, e);
        if (e.start_local != "unknown" && e.end_local == "unknown") {
            e.completion = "interrupted/incomplete";
            e.partial = "yes";
        } else if (e.end_local != "unknown") {
            e.completion = "complete";
        }
        if (e.start_local != "unknown" && e.end_local != "unknown") {
            struct tm a, b;
            if (parse_utc(e.start_local, &a) && parse_utc(e.end_local, &b)) {
                time_t ta = timegm(&a), tb = timegm(&b);
                if (tb >= ta) e.duration = std::to_string((long long)(tb - ta)) + " s";
            }
        }
        if (e.start_local != "unknown") e.start_local = format_local_timestamp(e.start_local);
        if (e.end_local != "unknown" && e.end_local.find('T') != std::string::npos)
            e.end_local = format_local_timestamp(e.end_local);
        out.push_back(e);
    }
    closedir(dir);
}
} // namespace

void parse_line(Entry &e, const std::string &line) {
    const std::string event = line.find(" SESSION_BEGIN ") != std::string::npos ? "SESSION_BEGIN" :
                              line.find(" SESSION_END ") != std::string::npos ? "SESSION_END" :
                              line.find(" DIAG_UPLOAD ") != std::string::npos ? "DIAG_UPLOAD" : "";
    if (event.empty()) return;
    const std::string ts = timestamp_prefix(line);
    const std::string scope = value_for(line, "scope");
    const std::string session = value_for(line, "session_id").empty()
                                    ? value_for(line, "session")
                                    : value_for(line, "session_id");
    if (!scope.empty()) e.scope = scope;
    if (!session.empty()) e.session = session;
    if (event == "SESSION_BEGIN" && !ts.empty()) e.start_local = ts;
    if (event == "SESSION_END" && !ts.empty()) e.end_local = ts;
    const std::string truncated = value_for(line, "truncated");
    if (!truncated.empty()) e.partial = truncated == "1" || truncated == "true" ? "yes" : "no";
    const std::string state = value_for(line, "state");
    if (event == "DIAG_UPLOAD" && (state == "accepted" || state == "failed"))
        e.upload_state = state;
}

std::string format_local_timestamp(const std::string &utc) {
    struct tm value;
    if (!parse_utc(utc, &value)) return "unknown";
    time_t seconds = timegm(&value);
    struct tm local;
    if (localtime_r(&seconds, &local) == NULL) return "unknown";
    char out[40];
    if (strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S %z", &local) == 0)
        return "unknown";
    return out;
}

std::vector<Entry> list(const std::string &root, size_t limit,
                        size_t *total_count) {
    std::vector<Entry> entries;
    size_t count = 0;
    limit = std::min(limit, MAX_FILES);
    add_directory(root, entries, limit, count);
    add_directory(root + "/sessions", entries, limit, count);
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.filename < b.filename;
    });
    if (total_count) *total_count = count;
    return entries;
}
} // namespace DiagnosticInventory

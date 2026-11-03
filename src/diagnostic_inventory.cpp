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
const size_t MAX_SCAN_BYTES = 64 * 1024;
const size_t MAX_LINE = 2048;

std::string value_for(const std::string &line, const char *key) {
    const std::string needle = std::string(key) + "=";
    size_t at = line.find(needle);
    if (at == std::string::npos) return "";
    at += needle.size();
    size_t end = line.find_first_of(" \t\r\n", at);
    return line.substr(at, end == std::string::npos ? end : end - at);
}

bool parse_digits(const std::string &s, size_t offset, size_t length, int *value) {
    int parsed = 0;
    if (offset + length > s.size()) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = s[offset + i];
        if (c < '0' || c > '9') return false;
        parsed = parsed * 10 + (c - '0');
    }
    *value = parsed;
    return true;
}

long long days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 +
                                 day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 -
                                year_of_era / 100 + day_of_year;
    return static_cast<long long>(era) * 146097 + day_of_era - 719468;
}

bool parse_utc(const std::string &s, time_t *seconds_out) {
    if (s.size() < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' ||
        s[13] != ':' || s[16] != ':' || s.back() != 'Z') return false;
    int year, month, day, hour, minute, second;
    if (!parse_digits(s, 0, 4, &year) || !parse_digits(s, 5, 2, &month) ||
        !parse_digits(s, 8, 2, &day) || !parse_digits(s, 11, 2, &hour) ||
        !parse_digits(s, 14, 2, &minute) || !parse_digits(s, 17, 2, &second))
        return false;
    static const int month_days[] = {31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59)
        return false;
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int max_day = month_days[month - 1] + (month == 2 && leap ? 1 : 0);
    if (day < 1 || day > max_day) return false;
    const long long epoch = days_from_civil(year, static_cast<unsigned>(month),
                                             static_cast<unsigned>(day)) * 86400LL +
                            hour * 3600LL + minute * 60LL + second;
    *seconds_out = static_cast<time_t>(epoch);
    return static_cast<long long>(*seconds_out) == epoch;
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

bool is_diagnostic_filename(const std::string &filename) {
    if (filename == "moonlight.log") return true;
    const size_t slash = filename.find_last_of('/');
    const std::string leaf = slash == std::string::npos ? filename : filename.substr(slash + 1);
    return leaf.size() > 4 && leaf.compare(leaf.size() - 4, 4, ".log") == 0;
}

void scan_upload_state(const std::string &path, Entry &entry) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return;
    char buffer[MAX_LINE];
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    const long file_size = ftell(f);
    if (file_size < 0) { fclose(f); return; }
    const long offset = file_size > static_cast<long>(MAX_SCAN_BYTES)
                            ? file_size - static_cast<long>(MAX_SCAN_BYTES) : 0;
    if (fseek(f, offset, SEEK_SET) != 0) { fclose(f); return; }
    size_t consumed = 0;
    while (consumed < MAX_SCAN_BYTES && fgets(buffer, sizeof(buffer), f)) {
        const size_t n = strlen(buffer);
        consumed += n;
        if (strstr(buffer, " DIAG_UPLOAD ") != NULL)
            parse_line(entry, std::string(buffer, n));
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
        e.app_state = !is_diagnostic_filename(e.filename);
        if (e.filename == "moonlight.log") e.scope = "application log";
        struct tm modified;
        char modified_text[40];
        if (localtime_r(&st.st_mtime, &modified) != NULL &&
            strftime(modified_text, sizeof(modified_text), "%Y-%m-%d %H:%M:%S %z", &modified) != 0)
            e.modified_local = modified_text;
        /* The append-only application log can contain many sessions; do not
         * misattribute its last session's facts to the whole file. */
        if (e.filename == "moonlight.log") scan_upload_state(full, e);
        else if (!e.app_state) scan_file(full, e);
        if (e.start_local != "unknown" && e.end_local == "unknown") {
            e.completion = "interrupted/incomplete";
            e.partial = "yes";
        } else if (e.end_local != "unknown") {
            e.completion = "complete";
        }
        if (e.start_local != "unknown" && e.end_local != "unknown") {
            time_t ta, tb;
            if (parse_utc(e.start_local, &ta) && parse_utc(e.end_local, &tb)) {
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
    if (event == "SESSION_BEGIN" && !ts.empty()) {
        e.start_local = ts;
        time_t epoch;
        if (parse_utc(ts, &epoch)) e.start_epoch = static_cast<unsigned long long>(epoch);
    }
    if (event == "SESSION_END" && !ts.empty()) {
        e.end_local = ts;
        time_t epoch;
        if (parse_utc(ts, &epoch)) e.end_epoch = static_cast<unsigned long long>(epoch);
    }
    const std::string truncated = value_for(line, "truncated");
    if (!truncated.empty()) e.partial = truncated == "1" || truncated == "true" ? "yes" : "no";
    const std::string state = value_for(line, "state");
    if (event == "DIAG_UPLOAD" && (state == "accepted" || state == "failed"))
        e.upload_state = state;
}

std::string format_local_timestamp(const std::string &utc) {
    time_t seconds;
    if (!parse_utc(utc, &seconds)) return "unknown";
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
        if (a.app_state != b.app_state) return !a.app_state;
        return a.filename < b.filename;
    });
    if (total_count) *total_count = count;
    return entries;
}
} // namespace DiagnosticInventory

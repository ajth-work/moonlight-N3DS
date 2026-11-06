#include "diagnostic_inventory.hpp"
#include "diagnostic_spool.h"
#include "diagnostic_transfer_client.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <openssl/sha.h>
#include <set>
#ifdef HOST_RECEIVER_TEST
#include "src/diagnostic_transfer.h"
#endif

static bool active = false;
extern "C" bool diag_session_path_active(const char *) { return active; }
extern "C" void diag_session_snapshot_lock(void) {}
extern "C" void diag_session_snapshot_unlock(void) {}
namespace fs = std::filesystem;
using namespace DiagnosticTransfer;

static std::string digest(const std::string &data) {
    unsigned char bytes[32];
    SHA256((const unsigned char *)data.data(), data.size(), bytes);
    const char *hex = "0123456789abcdef";
    std::string result;
    for (auto b : bytes) {
        result += hex[b >> 4];
        result += hex[b & 15];
    }
    return result;
}
static void write(const std::string &p, const std::string &s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
    assert(out.good());
}
static Capture capture(const fs::path &root, const std::string &name, size_t lines = 14000) {
    Capture c{(root / ("session-" + name + ".log")).generic_string(), digest(name).substr(0, 32),
              "3.6.0-test", 1700000000, 1700000001};
    std::string text;
    for (size_t i = 0; i < lines; ++i)
        text += "2026-11-05 +0 FRAME sample=" + std::to_string(i) + " harmless data\n";
    text += "2026 +0 SECRET password=must-never-leave-device\n";
    write(c.path, text);
    write(c.path + ".meta",
          "version=2\nbegin=1700000000\nend=1700000001\nactive=0\ngaps=0\nstream=1\nbytes=" +
              std::to_string(text.size()) + "\n");
    return c;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    fs::path root = fs::absolute(argv[1]) /
                    ("client-protocol-fixture-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    auto no_cancel = [] { return false; };
    auto notify = [](const Progress &p) { assert(!p.total || p.acknowledged <= p.total); };
    for (const auto &bad : {"{}garbage", "{\"x\":1,}", "{\"x\":1,\"x\":2}", "{\"x\":[]}"}) {
        bool threw = false;
        try {
            parse_response(bad);
        } catch (...) {
            threw = true;
        }
        assert(threw);
    }
    assert(parse_response("{\"version\":2,\"ok\":true}").at("ok") == "true");
    assert(is_sidecar("session-a.log.p00000001") && !is_sidecar("session-a.log"));
    auto legacy = capture(root, "legacy");
    auto unsupported = upload(
        legacy, [](const auto &, const auto &, bool) { return Response{false, 404, "{}"}; }, notify,
        no_cancel);
    assert(unsupported.unsupported && !fs::exists(legacy.path + ".capture"));
    auto cancelled = upload(
        legacy,
        [](const auto &, const auto &, bool) {
            assert(false);
            return Response{};
        },
        notify, [] { return true; });
    assert(cancelled.cancelled && !cancelled.accepted);

#ifdef HOST_RECEIVER_TEST
    const fs::path server_root = root / "receiver";
    diagnostic_transfer::store_t host(server_root);
    unsigned parts_sent = 0;
    bool fail_second = true;
    std::string id;
    auto transport = [&](const std::string &path, const std::string &body, bool binary) {
        assert(body.size() <= (binary ? ChunkLimit : 32768));
        if (path == "/client-diagnostics/v2/capabilities")
            return Response{true, 200, "{\"version\":2,\"chunk_limit\":245760}"};
        const std::string prefix = "/client-diagnostics/v2/";
        const auto slash = path.find('/', prefix.size());
        id = path.substr(prefix.size(), slash - prefix.size());
        const auto action = path.substr(slash + 1);
        if (binary) {
            assert(body.find("must-never-leave-device") == std::string::npos);
            ++parts_sent;
            if (fail_second && action == "part/1")
                return Response{false, 0, ""};
        }
        auto reply = host.handle(id, action, body);
        return Response{reply.status == 200, reply.status, reply.body.dump()};
    };
    auto c = capture(root, "large");
    auto first = upload(c, transport, notify, no_cancel);
    assert(!first.accepted && fs::exists(c.path + ".capture"));
    const auto first_id = first.capture_id;
    const auto sent_before = parts_sent;
    // New activity after a failed send must not change the frozen capture.
    {
        std::ofstream append(c.path, std::ios::app);
        append << "later activity\n";
    }
    fail_second = false;
    auto resumed = upload(c, transport, notify, no_cancel);
    assert(resumed.accepted && resumed.complete && resumed.capture_id == first_id);
    assert(parts_sent - sent_before == 2); // 3 chunks total; durable part zero is reused.
    assert(!fs::exists(c.path + ".capture"));
    assert(fs::file_size(server_root / id / "session.log") == resumed.content_bytes);
    assert(diagnostic_transfer::hash_file(server_root / id / "session.log") ==
           diagnostic_transfer::read_json(server_root / id / "receipt.json")["content_sha256"]
               .get<std::string>());
    assert(resumed.content_bytes > 256 * 1024);

    auto partial = capture(root, "unclean", 50);
    write(partial.path + ".meta",
          "version=2\nbegin=1700000000\nend=0\nactive=1\ngaps=0\nstream=1\nbytes=2000\n");
    auto incomplete = upload(partial, transport, notify, no_cancel);
    assert(incomplete.accepted && !incomplete.complete && incomplete.unclean);

    auto changed = capture(root, "changed");
    fail_second = true;
    auto pending = upload(changed, transport, notify, no_cancel);
    assert(!pending.accepted);
    write(changed.path + ".snapshot", "x");
    fail_second = false;
    auto corrupt = upload(changed, transport, notify, no_cancel);
    assert(!corrupt.accepted && corrupt.error.find("source") != std::string::npos);
    assert(fs::exists(changed.path + ".capture"));

    auto entries = DiagnosticInventory::list(root.generic_string(), 64);
    unsigned logical = 0;
    for (const auto &e : entries) {
        assert(!is_sidecar(e.filename));
        if (e.filename == fs::path(c.path).filename().string()) {
            ++logical;
            assert(e.upload_state == "accepted" && e.partial == "no" && e.has_stream);
        }
        if (e.filename == fs::path(partial.path).filename().string())
            assert(e.upload_state == "accepted" && e.partial == "yes");
    }
    assert(logical == 1);
#endif
    // Sparse legacy evidence counts toward the budget and is never silently discarded.
    auto old = (root / "moonlight-legacy.log").generic_string();
    write(old, "legacy");
    fs::resize_file(old, StorageLimit + 1);
    assert(!diag_spool_allow_growth(legacy.path.c_str(), 4096));
    assert(fs::exists(old) && fs::exists(legacy.path));
    assert(root.parent_path() == fs::absolute(argv[1]) &&
           root.filename().string().rfind("client-protocol-fixture-", 0) == 0);
    fs::remove_all(root);
    std::cout << "Client protocol/parser/cancel/resume/redaction/inventory/quota checks passed\n";
}

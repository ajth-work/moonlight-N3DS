/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2019 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "audio/audio.h"
#include "config.hpp"
#include "diagnostics.h"
#include "diagnostic_inventory.hpp"
#include "diagnostic_transfer_client.hpp"
#include <sys/stat.h>
#include "input/n3ds_input.hpp"
#include "input/touch/TouchHandler.hpp"
#include "system/AtomicVar.hpp"
#include "system/dispatcher.hpp"
#include "system/n3ds_connection.hpp"
#include "system/pair_record.hpp"
#include "pin_store.h"
#include "ui/ui.hpp"
#include "video/video.hpp"
#include "video/renderer/N3dsRenderer.hpp"
#include "video/renderer/N3ds3DPresentation.hpp"

#include <3ds.h>
#include <Limelight.h>

#include <client.h>
#include <discover.h>
#include <http.h>

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

#define SOC_ALIGN 0x1000
// 0x40000 for each enet host (2 hosts total)
// 0x40000 for each platform socket (2 sockets total)
#define SOC_BUFFERSIZE 0x100000

#define MAX_INPUT_CHAR 60

static u32 *SOC_buffer = NULL;

typedef void (*DiagnosticTaskFunction)(void *);

struct DiagnosticTask {
    DiagnosticTaskFunction function;
    void *context;
    char name[96];
    bool http_cancel_supported;
    AtomicVar<bool> done{false};
};

static const char *diagnostic_callsite(const char *name) {
    if (strcmp(name, "connect.server_init") == 0) {
        return "src/n3ds_main.cpp:init_server() / libgamestream/client.c:gs_init()";
    }
    if (strcmp(name, "connect.app_list") == 0) {
        return "libgamestream/client.c:gs_applist()";
    }
    if (strcmp(name, "connect.display_list") == 0) {
        return "libgamestream/client.c:gs_display_list()";
    }
    if (strcmp(name, "pair") == 0) {
        return "libgamestream/client.c:gs_pair()";
    }
    if (strcmp(name, "stream.start_app") == 0) {
        return "libgamestream/client.c:gs_start_app()";
    }
    if (strcmp(name, "stream.limelight_start") == 0) {
        return "src/n3ds_main.cpp:LiStartConnection()";
    }
    return "diagnostic operation callback";
}

static void diagnostic_monitor_entry(void *context) {
    DiagnosticTask *task = static_cast<DiagnosticTask *>(context);
    bool cancel_requested = false;
    bool cancel_unavailable = false;
    bool pending_reported = false;
    uint64_t cancel_time = 0;
    while (!task->done.load()) {
        char phase[128];
        char lines[DIAG_RECENT_LINE_COUNT][DIAG_LOG_LINE_MAX];
        char pair_fingerprint[DIAG_CERT_FINGERPRINT_BUFFER_SIZE];
        char pair_source[DIAG_CERT_SOURCE_MAX];
        char tls_fingerprint[DIAG_CERT_FINGERPRINT_BUFFER_SIZE];
        char tls_source[DIAG_CERT_SOURCE_MAX];
        unsigned int line_count = 0;
        bool watchdog_expired = false;
        diag_get_state(phase, sizeof(phase), lines, &line_count,
                       &watchdog_expired);
        diag_get_cert_panel_state(pair_fingerprint, sizeof(pair_fingerprint),
                                  pair_source, sizeof(pair_source),
                                  tls_fingerprint, sizeof(tls_fingerprint),
                                  tls_source, sizeof(tls_source));

        consoleSelect(&DebugTouchHandler::bottomScreen);
        consoleClear();
        printf("Phase: %s\n", phase);
        if (watchdog_expired) {
            uint32_t free_bytes = osGetMemRegionFree(MEMREGION_APPLICATION);
            printf("WATCHDOG free=%lu B\n", (unsigned long)free_bytes);
            printf("B: cancel to host list\n");
        } else if (diag_cancel_requested()) {
            printf("B cancel requested; waiting\n");
        } else if (cancel_unavailable) {
            printf("B cannot interrupt this call\n");
        } else {
            printf("Pair/connect trace\n");
        }
        for (unsigned int i = 0; i < line_count; i++) {
            printf("%.52s\n", lines[i]);
        }
        printf("----------------------------------------\n");
        printf("PAIR %.16s %s\n",
               pair_fingerprint[0] ? pair_fingerprint : "----",
               pair_source[0] ? pair_source : "----");
        printf("TLS  %.32s\n",
               tls_fingerprint[0] ? tls_fingerprint : "----");
        printf("     %.32s route=%s\n",
               tls_fingerprint[32] ? tls_fingerprint + 32 : "----",
               tls_source[0] ? tls_source : "unknown");
        printf("peer n/a (CERTINFO unavailable)\n");
        n3ds_draw_diag_version_overlay();
        consoleSelect(&DebugTouchHandler::topScreen);
        gfxFlushBuffers();
        diag_watchdog_poll();
        gspWaitForVBlank();
        hidScanInput();
        u32 keys_down = hidKeysDown();
        if (!cancel_requested && (keys_down & KEY_B) &&
            diag_phase_elapsed_ms() >= DIAG_WATCHDOG_LIMIT_MS) {
            if (task->http_cancel_supported) {
                diag_request_cancel();
                cancel_requested = true;
                cancel_time = diag_elapsed_ms();
            } else {
                char phase[128];
                diag_get_state(phase, sizeof(phase), NULL, NULL, NULL);
                diag_log("CANCEL_UNAVAILABLE", "phase=%s blocked_call=%s has no abort hook",
                         phase, diagnostic_callsite(task->name));
                cancel_unavailable = true;
                pending_reported = true;
            }
        }
        if (!pending_reported && cancel_requested &&
            diag_elapsed_ms() - cancel_time >= 3000) {
            char active_http[DIAG_LOG_LINE_MAX];
            bool http_active = false;
            char phase[128];
            diag_get_active_http(active_http, sizeof(active_http), &http_active);
            diag_get_state(phase, sizeof(phase), NULL, NULL, NULL);
            if (http_active) {
                diag_log("CANCEL_PENDING", "phase=%s blocked_call=libgamestream/http.c:http_request()->curl_easy_perform active=%s",
                         phase, active_http);
            } else {
                diag_log("CANCEL_PENDING", "operation=%s phase=%s blocked_call=%s no active HTTP transfer; no abort hook exists for this call",
                         task->name, phase, diagnostic_callsite(task->name));
            }
            pending_reported = true;
        }
    }
}

static void run_monitored_call(const char *name,
                               DiagnosticTaskFunction function,
                               void *context,
                               bool http_cancel_supported = true) {
    DiagnosticTask task;
    task.function = function;
    task.context = context;
    task.http_cancel_supported = http_cancel_supported;
    snprintf(task.name, sizeof(task.name), "%s", name);
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    diag_cancel_reset();
    diag_log("TASK_BEGIN", "name=%s", name);
    diag_set_phase(name);
    Thread monitor = threadCreate(diagnostic_monitor_entry, &task, 0x20000,
                                  priority, -1, false);
    if (monitor == NULL) {
        diag_log("MONITOR_ERROR", "name=%s reason=threadCreate failed; call remains synchronous without live watchdog UI", name);
    }

    /* Keep the original call on the caller thread. The monitor services the
     * display and forwards B only to calls with an HTTP abort callback. */
    function(context);
    if (monitor != NULL) {
        task.done.store(true);
        threadJoin(monitor, 0xFFFFFFFFFFFFFFFFULL);
        threadFree(monitor);
    }

    diag_log("TASK_END", "name=%s outcome=%s",
             name, diag_cancel_requested() ? "cancelled" : "returned");
}

static inline void wait_for_button(std::string prompt = "") {
    if (prompt.empty()) {
        printf("\nPress any button to continue\n");
    } else {
        printf("\n%s\n", prompt.c_str());
    }
    while (aptMainLoop()) {
        n3ds_draw_diag_version_overlay();
        gfxSwapBuffers();
        gfxFlushBuffers();
        gspWaitForVBlank();

        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown)
            break;
    }
}

static void n3ds_exit_handler(void) {
    diag_log("APP_EXIT", "clean exit handler entered");
    diag_close();
    NDMU_UnlockState();
    NDMU_LeaveExclusiveState();
    ndmuExit();
    irrstExit();
    SOCU_ShutdownSockets();
    SOCU_CloseSockets();
    socExit();
    free(SOC_buffer);
    romfsExit();
    aptExit();
    gfxExit();
    acExit();
}


struct GuiMenuItem {
    std::string label;
    std::string explanation;
    std::string value;
};

static void restore_consoles_after_dual_screen_ui();
static void show_gui_message(const std::string &title, const std::string &message,
                             const std::string &detail,
                             const std::string &headline = "NEXT STEP");
static bool upload_session(PSERVER_DATA server, const DIAG_SESSION &session,
                           const char *outcome, DIAG_UPLOAD_INFO *upload_info);

class GuiMenuScreen : public MoonlightUi::DualScreenModel {
  public:
    enum class Result { Back, Selected };
    GuiMenuScreen(const std::string &title, const std::string &context,
                  std::vector<GuiMenuItem> items, int initial = 0)
        : title_(title), context_(context), items_(std::move(items)),
          selected_(items_.empty() ? 0 : std::max(0, std::min(
              initial, static_cast<int>(items_.size()) - 1))) {}

    void draw(MoonlightUi::Screen &top, MoonlightUi::Screen &bottom) override {
        top.drawHeader("MOONLIGHT", title_);
        const int first = std::max(0, std::min(selected_ - 4,
                           static_cast<int>(items_.size()) - VISIBLE_ROWS));
        const int count = std::min(VISIBLE_ROWS,
                                   static_cast<int>(items_.size()) - first);
        for (int row = 0; row < count; ++row) {
            const int index = first + row;
            const int y = LIST_TOP + row * ROW_STEP;
            const MoonlightUi::Rect tile{16, y, top.width() - 16, y + ROW_HEIGHT};
            top.drawFilledTile(tile, index + 1);
            top.drawAutoRateText(tile.left + 30, tile.top + 6,
                                 items_[index].label, tile.right - tile.left - 42,
                                 {38, 43, 48});
            if (index == selected_) top.drawHighlight(tile);
        }
        top.drawFooter(DIAG_BUILD_VERSION, "UP/DOWN  A SELECT");
        bottom.drawHeader(context_, "SELECTED");
        if (!items_.empty()) {
            const GuiMenuItem &item = items_[selected_];
            bottom.drawAutoRateText(20, 65, item.label, bottom.width() - 40,
                                    {38, 43, 48});
            if (!item.value.empty())
                bottom.drawAutoRateText(20, 91, "CURRENT: " + item.value,
                                        bottom.width() - 40, {0, 137, 153});
            draw_explanation(bottom, item.explanation);
        }
        bottom.drawFilledTile(ACTION_TILE, 1);
        bottom.drawLiveText(100, 194, "SELECT", MoonlightUi::TextStyle::Tile,
                            {38, 43, 48});
        bottom.drawFooter(DIAG_BUILD_VERSION, "A SELECT  B BACK  TOUCH");
    }

    bool handleInput(const MoonlightUi::InputEvent &event, bool *redraw) override {
        *redraw = false;
        if (event.type == MoonlightUi::InputEvent::Type::Back) {
            result_ = Result::Back;
            return false;
        }
        if (event.type == MoonlightUi::InputEvent::Type::Confirm) {
            result_ = Result::Selected;
            return false;
        }
        if (event.type == MoonlightUi::InputEvent::Type::Touch) {
            if (event.touch.px >= ACTION_TILE.left && event.touch.px <= ACTION_TILE.right &&
                event.touch.py >= ACTION_TILE.top && event.touch.py <= ACTION_TILE.bottom) {
                result_ = Result::Selected;
                return false;
            }
            return true;
        }
        if (event.type == MoonlightUi::InputEvent::Type::Direction && !items_.empty()) {
            const int old = selected_;
            if (event.direction == MoonlightUi::Direction::Up)
                selected_ = selected_ == 0 ? static_cast<int>(items_.size()) - 1 : selected_ - 1;
            else if (event.direction == MoonlightUi::Direction::Down)
                selected_ = (selected_ + 1) % static_cast<int>(items_.size());
            *redraw = old != selected_;
        }
        return true;
    }
    Result result() const { return result_; }
    int selected_index() const { return selected_; }

  private:
    static constexpr int VISIBLE_ROWS = 5;
    static constexpr int LIST_TOP = 61;
    static constexpr int ROW_HEIGHT = 27;
    static constexpr int ROW_STEP = 31;
    static constexpr MoonlightUi::Rect ACTION_TILE{14, 190, 306, 218};
    std::string title_;
    std::string context_;
    std::vector<GuiMenuItem> items_;
    int selected_;
    Result result_ = Result::Back;

    static void draw_explanation(MoonlightUi::Screen &screen, const std::string &text) {
        std::istringstream words(text);
        std::string word, line;
        int y = 121;
        while (words >> word && y <= 177) {
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && screen.measureLiveText(candidate, MoonlightUi::TextStyle::Small) > screen.width() - 40) {
                screen.drawLiveText(20, y, line, MoonlightUi::TextStyle::Small, {105, 114, 122});
                y += 16;
                line = word;
            } else line = candidate;
        }
        if (!line.empty() && y <= 177)
            screen.drawLiveText(20, y, line, MoonlightUi::TextStyle::Small, {105, 114, 122});
    }
};

static bool run_gui_menu(const std::string &title, const std::string &context,
                         const std::vector<GuiMenuItem> &items, int *selected) {
    GuiMenuScreen screen(title, context, items, selected ? *selected : 0);
    if (!MoonlightUi::runDualScreen(screen)) exit(0);
    restore_consoles_after_dual_screen_ui();
    if (screen.result() != GuiMenuScreen::Result::Selected) return false;
    if (selected) *selected = screen.selected_index();
    return true;
}

static std::vector<DiagnosticInventory::Entry> log_inventory_cache;
static size_t log_inventory_cache_count = 0;
static time_t log_inventory_cache_at = 0, log_inventory_root_time = 0, log_inventory_sessions_time = 0;

class CheckLogsScreen : public MoonlightUi::DualScreenModel {
  public:
    enum class Result { Back, Send };
    CheckLogsScreen() = default;

    void draw(MoonlightUi::Screen &top, MoonlightUi::Screen &bottom) override {
        if (!loaded_) {
            top.drawHeader("CHECK LOGS", "LOADING LOCAL INVENTORY");
            top.drawLiveText(28, 92, "Scanning diagnostic files...",
                             MoonlightUi::TextStyle::Header, {38, 43, 48});
            top.drawFooter(DIAG_BUILD_VERSION, "PLEASE WAIT");
            bottom.drawHeader("LOG BROWSER", "LOCAL ONLY");
            bottom.drawLiveText(18, 78, "Reading filenames and session times.",
                                MoonlightUi::TextStyle::Small, {38, 43, 48});
            bottom.drawFooter(DIAG_BUILD_VERSION, "KEEP LOGS ON DEVICE");
            return;
        }
        const std::vector<size_t> visible = visible_indices();
        const int latest_session = latest_session_position(visible);
        top.drawHeader("CHECK LOGS", std::to_string(visible.size()) + " CLIENT LOGS · NEWEST FIRST");
        const char *tab_names[] = {"ALL", "CONNECT", "STREAM"};
        const int tab_width = (top.width() - 28) / 3;
        for (int i = 0; i < 3; ++i) {
            const int left = 14 + i * tab_width;
            const MoonlightUi::Rect tab{left, 50, left + tab_width - 4, 68};
            top.drawPanelButton(tab, tab_names[i], i == tab_ ?
                                MoonlightUi::Color{79, 86, 97} :
                                MoonlightUi::Color{222, 227, 231}, i == tab_ ?
                                MoonlightUi::Color{255, 255, 255} :
                                MoonlightUi::Color{38, 43, 48});
        }
        const int first = std::max(0, std::min(selected_ - 2,
                                              static_cast<int>(visible.size()) - 5));
        for (int row = 0; row < 5 && first + row < static_cast<int>(visible.size()); ++row) {
            const int index = static_cast<int>(visible[first + row]);
            MoonlightUi::Rect tile{14, 70 + row * 29, top.width() - 14, 95 + row * 29};
            top.drawFilledTile(tile, first + row + 1);
            if (first + row == latest_session)
                top.drawCircle(tile.left + 31, tile.top + 12, 7, {28, 143, 255});
            top.drawCircle(tile.left + 31, tile.top + 12, 4, status_color(entries_[index]));
            top.drawAutoRateText(tile.left + 42, tile.top + 5, row_label(entries_[index], tab_),
                                 tile.right - tile.left - 50, {38, 43, 48});
            if (first + row == selected_) top.drawHighlight(tile);
        }
        top.drawFooter(DIAG_BUILD_VERSION, "D-PAD L/R TAB · U/D LOG · A SEND · B BACK");
        bottom.drawHeader("LOG DETAILS", "MOONLIGHT 3DS · SUNSHINE LOGS ON PC");
        if (visible.empty()) {
            bottom.drawLiveText(20, 72, "No diagnostic logs found.",
                                MoonlightUi::TextStyle::Small, {38, 43, 48});
        } else {
            const DiagnosticInventory::Entry &e = entries_[visible[selected_]];
            const std::string no_upload_record = e.app_state ? "not applicable" :
                (e.upload_state == "unknown" ? "no send attempt in log" : "not recorded");
            const bool is_session_file = !e.app_state && e.filename != "moonlight.log";
            const bool is_latest = is_session_file && selected_ == latest_session;
            const std::string recency = !is_session_file ? "not a session record" :
                (is_latest ? "newest session in this view" : "older session record");
            const std::vector<std::string> lines = more_details_ ? std::vector<std::string>{
                "FILE: " + e.filename,
                "SESSION: " + (e.session == "unknown" ? "not recorded" : e.session),
                "RECENCY: " + recency,
                "UPLOAD RESULT: " + upload_status(e),
                "UPLOAD ID: " + (e.upload_id.empty() ? no_upload_record : e.upload_id),
                "LOCAL FILE: " + std::to_string(e.size) + " bytes",
                "SENT SOURCE: " + (e.upload_source_bytes ? std::to_string(e.upload_source_bytes) + " bytes" : no_upload_record),
                "FILTERED LOG: " + (e.upload_filtered_bytes ? std::to_string(e.upload_filtered_bytes) + " bytes" : no_upload_record),
                "HTTP BODY: " + (e.upload_request_bytes ? std::to_string(e.upload_request_bytes) + " bytes" : no_upload_record)
            } : std::vector<std::string>{
                std::string("SOURCE: ") + (e.app_state ? "Moonlight 3DS app state" :
                    (e.filename == "moonlight.log" ? "Moonlight 3DS app log" : "Moonlight client session")),
                std::string("TYPE: ") + (e.app_state ? "application state" :
                    (e.filename == "moonlight.log" ? "application log" : display_scope(e))),
                "START: " + (e.start_local == "unknown" ? e.modified_local : e.start_local),
                "AGE: " + DiagnosticInventory::format_relative_age(
                    e.start_epoch ? e.start_epoch : (e.latest_epoch ? e.latest_epoch : e.modified_epoch)),
                "DURATION: " + e.duration,
                "LOCAL FILE: " + format_size(e.size),
                "UPLOAD: " + upload_status(e),
                std::string("RECENCY: ") + (is_latest ? "newest session · blue ring" : recency)
            };
            const int detail_line_spacing = more_details_ ? 14 : 15;
            for (size_t i = 0; i < lines.size(); ++i)
                bottom.drawAutoRateText(14, 47 + static_cast<int>(i) * detail_line_spacing, lines[i],
                                        bottom.width() - 28, {38, 43, 48});
            bottom.drawPanelButton(DETAILS_TILE, more_details_ ? "BASIC" : "MORE DETAILS",
                                   {222, 227, 231}, {38, 43, 48});
            bottom.drawPanelButton(SEND_TILE,
                                   e.app_state ? "READ ONLY" : "SEND LOG",
                                   {222, 227, 231}, {38, 43, 48});
        }
        bottom.drawFooter(DIAG_BUILD_VERSION, tab_ == 0 ?
                          "Y STATE · A SEND · TAP MORE · B BACK" :
                          "A SEND · TAP MORE · B BACK");
    }

    bool update(bool *redraw) override {
        *redraw = false;
        if (loaded_ && (hidKeysDown() & KEY_X)) { loaded_ = false; log_inventory_cache_at = 0; }
        if (loaded_) return true;
        struct stat root_stat{}, sessions_stat{};
        stat("sdmc:/3ds/moonlight", &root_stat);
        stat("sdmc:/3ds/moonlight/sessions", &sessions_stat);
        const time_t now = time(NULL);
        if (log_inventory_cache_at && now - log_inventory_cache_at < 30 &&
            log_inventory_root_time == root_stat.st_mtime &&
            log_inventory_sessions_time == sessions_stat.st_mtime) {
            entries_ = log_inventory_cache; total_count_ = log_inventory_cache_count;
        } else {
            entries_ = DiagnosticInventory::list("sdmc:/3ds/moonlight", 64, &total_count_);
            log_inventory_cache = entries_; log_inventory_cache_count = total_count_;
            log_inventory_cache_at = now; log_inventory_root_time = root_stat.st_mtime;
            log_inventory_sessions_time = sessions_stat.st_mtime;
        }
        loaded_ = true;
        *redraw = true;
        return true;
    }

    bool handleInput(const MoonlightUi::InputEvent &event, bool *redraw) override {
        *redraw = false;
        if (event.type == MoonlightUi::InputEvent::Type::Back) return false;
        if (!loaded_) return true;
        const std::vector<size_t> visible = visible_indices();
        if (event.type == MoonlightUi::InputEvent::Type::AppStateToggle) {
            if (tab_ == 0) {
                show_app_state_ = !show_app_state_;
                const size_t count = visible_indices().size();
                selected_ = count ? std::min(selected_, static_cast<int>(count) - 1) : 0;
                *redraw = true;
            }
        } else if (event.type == MoonlightUi::InputEvent::Type::Confirm) {
            if (!visible.empty() && !entries_[visible[selected_]].app_state) {
                result_ = Result::Send;
                return false;
            }
        } else if (event.type == MoonlightUi::InputEvent::Type::Touch) {
            const int x = event.touch.px, y = event.touch.py;
            if (y >= DETAILS_TILE.top && y <= DETAILS_TILE.bottom &&
                x >= DETAILS_TILE.left && x <= DETAILS_TILE.right) {
                more_details_ = !more_details_;
                *redraw = true;
            } else if (y >= SEND_TILE.top && y <= SEND_TILE.bottom &&
                       x >= SEND_TILE.left && x <= SEND_TILE.right &&
                       !visible.empty() && !entries_[visible[selected_]].app_state) {
                result_ = Result::Send;
                return false;
            }
        } else if (event.type == MoonlightUi::InputEvent::Type::Direction) {
            const int before = selected_;
            if (event.direction == MoonlightUi::Direction::Left) {
                tab_ = (tab_ + 2) % 3;
                selected_ = 0;
                more_details_ = false;
                *redraw = true;
            } else if (event.direction == MoonlightUi::Direction::Right) {
                tab_ = (tab_ + 1) % 3;
                selected_ = 0;
                more_details_ = false;
                *redraw = true;
            } else if (!visible.empty() && event.direction == MoonlightUi::Direction::Up)
                selected_ = selected_ == 0 ? static_cast<int>(visible.size()) - 1 : selected_ - 1;
            else if (!visible.empty() && event.direction == MoonlightUi::Direction::Down)
                selected_ = (selected_ + 1) % static_cast<int>(visible.size());
            if (before != selected_) {
                more_details_ = false;
                *redraw = true;
            }
        }
        return true;
    }

    Result result() const { return result_; }
    bool selected_entry(DiagnosticInventory::Entry *entry) const {
        if (!loaded_ || entry == NULL) return false;
        const std::vector<size_t> visible = visible_indices();
        if (visible.empty() || selected_ < 0 || selected_ >= static_cast<int>(visible.size())) return false;
        *entry = entries_[visible[selected_]];
        return true;
    }

  private:
    static constexpr MoonlightUi::Rect DETAILS_TILE{12, 176, 153, 207};
    static constexpr MoonlightUi::Rect SEND_TILE{166, 176, 307, 207};
    std::vector<DiagnosticInventory::Entry> entries_;
    size_t total_count_ = 0;
    int selected_ = 0;
    int tab_ = 0;
    bool loaded_ = false;
    bool show_app_state_ = false;
    bool more_details_ = false;
    Result result_ = Result::Back;

    std::vector<size_t> visible_indices() const {
        std::vector<size_t> indices;
        for (size_t i = 0; i < entries_.size(); ++i) {
            const DiagnosticInventory::Entry &e = entries_[i];
            if (e.app_state) {
                if (tab_ == 0 && show_app_state_) indices.push_back(i);
                continue;
            }
            if (tab_ == 0 ||
                (tab_ == 1 && e.filename != "moonlight.log" && e.scope != "stream") ||
                (tab_ == 2 && (e.scope == "stream" || e.has_stream))) indices.push_back(i);
        }
        return indices;
    }

    int latest_session_position(const std::vector<size_t> &visible) const {
        int latest_position = -1;
        unsigned long long latest_time = 0;
        for (size_t position = 0; position < visible.size(); ++position) {
            const DiagnosticInventory::Entry &e = entries_[visible[position]];
            if (e.app_state || e.filename == "moonlight.log") continue;
            const unsigned long long timestamp = e.start_epoch ? e.start_epoch : e.modified_epoch;
            if (latest_position < 0 || timestamp > latest_time) {
                latest_position = static_cast<int>(position);
                latest_time = timestamp;
            }
        }
        return latest_position;
    }

    static std::string display_scope(const DiagnosticInventory::Entry &e) {
        if (e.has_stream || e.scope == "stream") return "connection + stream activity";
        return "host connection";
    }

    static std::string upload_status(const DiagnosticInventory::Entry &e) {
        if (e.upload_state == "accepted") return e.partial == "yes" ? "accepted � partial history" : "accepted by Sunshine";
        if (e.upload_state == "paused") return "paused � send to resume";
        if (e.upload_state == "failed") return "last attempt failed";
        if (e.upload_state == "uncertain") return "attempt outcome unknown";
        return "no send attempt in log";
    }

    static std::string format_size(uint64_t bytes) {
        if (bytes < 1024) return std::to_string(bytes) + " B";
        char text[32];
        snprintf(text, sizeof(text), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
        return text;
    }

    static MoonlightUi::Color status_color(const DiagnosticInventory::Entry &e) {
        if (e.app_state) return {105, 114, 122};
        if (e.upload_state == "accepted") return e.partial == "yes" ? MoonlightUi::Color{218, 143, 30} : MoonlightUi::Color{47, 151, 91};
        if (e.upload_state == "failed" || e.upload_state == "uncertain")
            return {218, 143, 30};
        return {194, 55, 61};
    }

    static std::string row_label(const DiagnosticInventory::Entry &e, int tab) {
        if (e.app_state) return "APP STATE  " + e.filename;
        if (e.filename == "moonlight.log") {
            const unsigned long long age_epoch = e.latest_epoch ? e.latest_epoch : e.modified_epoch;
            return "APPLICATION LOG  " + e.modified_local + " · " +
                   DiagnosticInventory::format_relative_age(age_epoch);
        }
        std::string label;
        if (tab == 2) label = "STREAM ";
        else if (tab == 1) label = "CONNECT ";
        else label = e.has_stream ? "CONNECT + STREAM " : "CONNECT ";
        if (e.start_local != "unknown") label += e.start_local;
        else if (e.modified_local != "unknown") label += e.modified_local;
        const unsigned long long age_epoch = e.start_epoch ? e.start_epoch :
            (e.latest_epoch ? e.latest_epoch : e.modified_epoch);
        const std::string age = DiagnosticInventory::format_relative_age(age_epoch);
        if (!age.empty()) label += " · " + age;
        return label;
    }
};

static bool is_diagnostic_log(const DiagnosticInventory::Entry &entry) {
    if (entry.app_state || entry.filename.empty() || entry.filename.find("..") != std::string::npos ||
        entry.filename[0] == '/') return false;
    if (entry.filename == "moonlight.log") return true;
    if (entry.filename.compare(0, 9, "sessions/") != 0) return false;
    return entry.filename.size() > 4 &&
           entry.filename.compare(entry.filename.size() - 4, 4, ".log") == 0;
}

static std::string make_sunshine_session_id(const std::string &filename,
                                            const std::string &session,
                                            unsigned long long begin_epoch) {
    const std::string material = filename + "|" + session + "|" + std::to_string(begin_epoch);
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (SHA256(reinterpret_cast<const unsigned char *>(material.data()), material.size(), digest) == NULL)
        return std::string();
    static const char hex[] = "0123456789abcdef";
    std::string id(32, '0');
    for (size_t i = 0; i < sizeof(digest) / 2; ++i) {
        id[i * 2] = hex[digest[i] >> 4];
        id[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    return id;
}

static bool upload_pin_available(PSERVER_DATA server) {
    char pin[53];
    return server != NULL && server->paired &&
           server->serverInfo.address != NULL &&
           host_pin_store_load(server->serverInfo.address, server->httpPort,
                               pin, sizeof(pin));
}

static void send_inventory_log(PSERVER_DATA server,
                               const DiagnosticInventory::Entry &entry) {
    if (!is_diagnostic_log(entry)) {
        show_gui_message("APP STATE NOT SENT",
                         "Configuration and pairing files are read-only here.",
                         "Choose a diagnostic log to send to Sunshine.");
        return;
    }
    if (server == NULL || !server->paired) {
        show_gui_message("HOST NOT PAIRED", "Pair this 3DS with the selected Sunshine host first.",
                         "The local log remains on the SD card.");
        return;
    }
    if (!upload_pin_available(server)) {
        diag_log("DIAG_UPLOAD_BLOCKED",
                 "reason=missing_host_certificate_pin recovery=repair_upload_trust");
        show_gui_message(
            "UPLOAD TRUST NEEDS REPAIR",
            "This host has no saved Sunshine certificate pin.",
            "Choose Repair Upload Trust; this log stays on the SD card.");
        return;
    }
    const std::vector<GuiMenuItem> choices = {
        {"Send This Log", "Send only the selected redacted diagnostic file over pinned HTTPS.", server->serverInfo.address},
        {"Cancel", "Keep the log local on the SD card.", "No upload"},
    };
    int selected = 1;
    if (!run_gui_menu("CONFIRM LOG TRANSFER", "PAIRED SUNSHINE HOST", choices, &selected) || selected != 0)
        return;

    DIAG_SESSION session{};
    const std::string session_id = make_sunshine_session_id(entry.filename, entry.session,
                                                            entry.start_epoch);
    if (session_id.empty()) {
        show_gui_message("LOG TRANSFER FAILED", "Could not identify the selected log.",
                         "The local copy remains available.");
        return;
    }
    snprintf(session.id, sizeof(session.id), "%s", session_id.c_str());
    snprintf(session.scope, sizeof(session.scope), "host");
    const std::string path = std::string("sdmc:/3ds/moonlight/") + entry.filename;
    if (path.size() >= sizeof(session.path)) {
        show_gui_message("LOG TRANSFER FAILED", "Selected log path is too long.",
                         "The local copy remains available.");
        return;
    }
    snprintf(session.path, sizeof(session.path), "%s", path.c_str());
    session.begin_epoch = entry.start_epoch;
    session.end_epoch = entry.end_epoch;
    if (session.begin_epoch == 0) session.begin_epoch = session.end_epoch ? session.end_epoch : (unsigned long long)time(NULL);
    if (session.end_epoch < session.begin_epoch) session.end_epoch = session.begin_epoch;
    DIAG_UPLOAD_INFO upload_info = {};
    const bool sent = upload_session(server, session, "manual_current_host",
                                     &upload_info);
    if (entry.filename == "moonlight.log") {
        diag_log("DIAG_UPLOAD", "scope=host session_id=%s upload_id=%s state=%s truncated=%s source_bytes=%llu filtered_bytes=%llu request_bytes=%llu",
                 session.id, upload_info.upload_id, sent ? "accepted" : "failed",
                 upload_info.truncated ? "true" : "false", upload_info.source_bytes, upload_info.filtered_bytes,
                 upload_info.request_bytes);
    } else {
        diag_session_record_upload(&session, sent, &upload_info);
    }
    const bool pin_available = upload_pin_available(server);
    show_gui_message(sent ? "LOG SENT" : (pin_available ? "LOG TRANSFER FAILED" : "UPLOAD TRUST NEEDS REPAIR"),
                     sent ? (upload_info.recording_complete ? "Sunshine verified the full recording." : "Retained log accepted; history is partial.") :
                         (pin_available ? "The log remains saved on the SD card." :
                                          "The log remains saved locally; no host pin is stored."),
                     sent ? "The local copy is still available in Check Logs." :
                         (pin_available ? "Check host pairing, the saved pin, and host reachability." :
                                          "Choose Repair Upload Trust, then send this log again."),
                     sent ? (upload_info.recording_complete ? "FULL RECORDING ACCEPTED" : "PARTIAL HISTORY ACCEPTED") : "PAUSED OR NOT SENT");
}

static void show_check_logs(PSERVER_DATA server) {
    while (aptMainLoop()) {
        CheckLogsScreen screen;
        MoonlightUi::runDualScreen(screen);
        restore_consoles_after_dual_screen_ui();
        if (screen.result() == CheckLogsScreen::Result::Back) return;
        DiagnosticInventory::Entry selected;
        if (screen.selected_entry(&selected)) send_inventory_log(server, selected);
    }
}

static std::string prompt_for_action(PSERVER_DATA server) {
    std::vector<GuiMenuItem> actions;
    if (server->paired) {
        const bool pin_available = upload_pin_available(server);
        bool upload_consent_known = false;
        bool upload_enabled = false;
        host_upload_consent_get(server->serverInfo.address, server->httpPort,
                                &upload_consent_known, &upload_enabled);
        if (!pin_available) {
            upload_consent_known = false;
            upload_enabled = false;
        }
        actions = {
            {"Stream", "Start streaming from the selected host. The host app list opens next.", "Ready"},
            {"Stream Profile", "Choose a saved resolution and bitrate profile for this host.", "Host-specific"},
            {"Connection Test", "Measure frame timing and staged bitrate stability without changing saved settings.", "Diagnostic"},
            {"Check Logs", "Browse diagnostic files stored on this 3DS.", "Local only"},
            {"Send Logs to this PC", "Send this host-selection session to the paired Sunshine host over pinned HTTPS.", "Manual upload"},
            {"Automatic Log Upload", "Upload each host-selection session after it ends.", !pin_available ? "Repair trust first" : (upload_consent_known ? (upload_enabled ? "Enabled" : "Disabled") : "Consent required")},
            {"Quit Stream", "Ask the host to close its currently active streaming app.", "Host request"},
            {"Stream Settings", "Edit video, audio, input, decoder, and diagnostic log options.", "Preferences"},
            {"Unpair", "Remove this device's pairing from the selected host.", "Destructive host change"},
            {"Exit to 3DS HOME", "Close Moonlight cleanly and return to the Nintendo 3DS HOME Menu.", "Exit app"},
        };
        if (!pin_available) {
            actions.insert(actions.begin() + 4,
                           {"Repair Upload Trust",
                            "Pair again to save the Sunshine certificate pin needed for uploads.",
                            "PIN required"});
        }
    } else {
        actions = {
            {"Pair", "Pair this 3DS with the selected host using the displayed PIN.", "Not paired"},
            {"Check Logs", "Browse diagnostic files stored on this 3DS.", "Local only"},
            {"Exit to 3DS HOME", "Close Moonlight cleanly and return to the Nintendo 3DS HOME Menu.", "Exit app"},
        };
    }
    int selected = 0;
    if (!run_gui_menu("HOST ACTIONS", "HOST", actions, &selected)) return "";
    const std::string &label = actions[selected].label;
    if (label == "Stream") return "stream";
    if (label == "Stream Profile") return "stream profile";
    if (label == "Connection Test") return "connection test";
    if (label == "Check Logs") return "check logs";
    if (label == "Repair Upload Trust") return "repair upload trust";
    if (label == "Send Logs to this PC") return "send logs";
    if (label == "Automatic Log Upload") return "toggle automatic logs";
    if (label == "Quit Stream") return "quit stream";
    if (label == "Stream Settings") return "stream settings";
    if (label == "Unpair") return "unpair";
    if (label == "Pair") return "pair";
    return "exit to 3ds home";
}

class HostPickerScreen : public MoonlightUi::DualScreenModel {
  public:
    enum class Result {
        Back,
        AddHost,
        EditHost,
        DeleteHost,
        SelectedHost,
        ExitToHome,
    };

    explicit HostPickerScreen(std::vector<std::string> addresses)
        : addresses_(std::move(addresses)),
          selected_index_(addresses_.empty()
                              ? static_cast<int>(addresses_.size())
                              : 0) {}

    void draw(MoonlightUi::Screen &top,
              MoonlightUi::Screen &bottom) override {
        top.drawHeader("MOONLIGHT", "SELECT A HOST");
        const int total_items = static_cast<int>(addresses_.size()) + 2;
        const int first_item = first_visible_item(total_items);
        const int visible_items =
            std::min(HOST_VISIBLE_ROWS, total_items - first_item);
        for (int row = 0; row < visible_items; ++row) {
            const int item_index = first_item + row;
            const int y = HOST_LIST_TOP + row * HOST_LIST_ROW_STEP;
            const MoonlightUi::Rect tile = {16, y, top.width() - 16,
                                             y + HOST_LIST_ROW_HEIGHT};
            top.drawFilledTile(tile, item_index + 1);
            const std::string label =
                item_index < static_cast<int>(addresses_.size())
                    ? addresses_[item_index]
                    : (item_index == static_cast<int>(addresses_.size())
                           ? "Add a new host address"
                           : "Exit to 3DS HOME");
            top.drawAutoRateText(
                tile.left + 28, tile.top + 6, label,
                tile.right - tile.left - 40, {38, 43, 48});
            if (item_index == selected_index_) {
                top.drawHighlight(tile);
            }
        }
        top.drawFooter(
            DIAG_BUILD_VERSION,
            selected_index_ == static_cast<int>(addresses_.size())
                ? "UP/DOWN  A ADD HOST"
                : selected_index_ == static_cast<int>(addresses_.size()) + 1
                    ? "UP/DOWN  A EXIT HOME"
                    : "UP/DOWN  A CONNECT");

        bottom.drawHeader("CONNECTION", "SELECTED HOST");
        bottom.drawLiveText(18, 67, "HOST", MoonlightUi::TextStyle::Small,
                            {0, 137, 153});
        const bool add_host_selected =
            selected_index_ == static_cast<int>(addresses_.size());
        const bool exit_selected =
            selected_index_ == static_cast<int>(addresses_.size()) + 1;
        const std::string selected_label =
            add_host_selected ? "Add a new host address"
            : exit_selected ? "Exit to 3DS HOME"
                            : addresses_[selected_index_];
        bottom.drawAutoRateText(18, 82, selected_label, bottom.width() - 36,
                                {38, 43, 48});

        bottom.drawFilledTile(CONNECT_TILE, 1);
        const std::string connect_label = exit_selected ? "EXIT TO HOME"
            : addresses_.empty() ? "NO SAVED HOSTS"
            : add_host_selected ? "SELECT A HOST" : "CONNECT";
        const MoonlightUi::Color connect_color =
            addresses_.empty() && !exit_selected
                ? MoonlightUi::Color{105, 114, 122}
                : MoonlightUi::Color{38, 43, 48};
        const int connect_label_width = bottom.measureLiveText(
            connect_label, MoonlightUi::TextStyle::Tile);
        bottom.drawLiveText((bottom.width() - connect_label_width) / 2, 134,
                            connect_label, MoonlightUi::TextStyle::Tile,
                            connect_color);
        if (!add_host_selected && !exit_selected) {
            bottom.drawPanelButton(EDIT_HOST_TILE, "EDIT",
                                   {222, 227, 231}, {38, 43, 48});
            bottom.drawPanelButton(DELETE_HOST_TILE, "DELETE X",
                                   {222, 227, 231}, {38, 43, 48});
        } else {
            bottom.drawFilledTile(ADD_HOST_TILE, 2);
            bottom.drawLiveText(94, 185, "ADD NEW HOST",
                                MoonlightUi::TextStyle::Tile,
                                {38, 43, 48});
        }
        bottom.drawFooter(
            DIAG_BUILD_VERSION,
            add_host_selected ? "A ADD HOST  B BACK  TOUCH"
            : exit_selected ? "A EXIT HOME  B BACK  TOUCH"
                            : "A CONNECT  TOUCH EDIT OR DELETE");
    }

    bool handleInput(const MoonlightUi::InputEvent &event,
                     bool *redraw) override {
        *redraw = false;
        if (event.type == MoonlightUi::InputEvent::Type::Back) {
            result_ = Result::Back;
            return false;
        }
        if (event.type == MoonlightUi::InputEvent::Type::Confirm) {
            activate_selected();
            return false;
        }
        if (event.type == MoonlightUi::InputEvent::Type::Touch) {
            const bool exit_selected =
                selected_index_ == static_cast<int>(addresses_.size()) + 1;
            if (touch_in_rect(event.touch, CONNECT_TILE)) {
                if (exit_selected) {
                    result_ = Result::ExitToHome;
                    return false;
                }
                if (!addresses_.empty() &&
                    selected_index_ < static_cast<int>(addresses_.size())) {
                    result_ = Result::SelectedHost;
                    return false;
                }
            } else if (!addresses_.empty() &&
                       selected_index_ < static_cast<int>(addresses_.size()) &&
                       touch_in_rect(event.touch, EDIT_HOST_TILE)) {
                result_ = Result::EditHost;
                return false;
            } else if (!addresses_.empty() &&
                       selected_index_ < static_cast<int>(addresses_.size()) &&
                       touch_in_rect(event.touch, DELETE_HOST_TILE)) {
                result_ = Result::DeleteHost;
                return false;
            } else if (touch_in_rect(event.touch, ADD_HOST_TILE)) {
                result_ = Result::AddHost;
                return false;
            }
            return true;
        }
        if (event.type != MoonlightUi::InputEvent::Type::Direction) {
            return true;
        }

        const int last_item = static_cast<int>(addresses_.size()) + 1;
        const int previous = selected_index_;
        if (event.direction == MoonlightUi::Direction::Up) {
            selected_index_ = selected_index_ == 0 ? last_item
                                                   : selected_index_ - 1;
        } else if (event.direction == MoonlightUi::Direction::Down) {
            selected_index_ =
                selected_index_ == last_item ? 0 : selected_index_ + 1;
        }
        *redraw = selected_index_ != previous;
        return true;
    }

    Result result() const { return result_; }

    const std::string &selected_address() const {
        return addresses_[selected_index_];
    }

  private:
    static constexpr int HOST_VISIBLE_ROWS = 5;
    static constexpr int HOST_LIST_TOP = 66;
    static constexpr int HOST_LIST_ROW_HEIGHT = 27;
    static constexpr int HOST_LIST_ROW_STEP = 31;
    static constexpr MoonlightUi::Rect CONNECT_TILE = {14, 119, 306, 165};
    static constexpr MoonlightUi::Rect ADD_HOST_TILE = {14, 173, 306, 218};
    static constexpr MoonlightUi::Rect EDIT_HOST_TILE = {14, 173, 155, 218};
    static constexpr MoonlightUi::Rect DELETE_HOST_TILE = {165, 173, 306, 218};

    std::vector<std::string> addresses_;
    int selected_index_;
    Result result_ = Result::Back;

    int first_visible_item(int total_items) const {
        int first = selected_index_ - HOST_VISIBLE_ROWS + 1;
        if (first < 0) {
            first = 0;
        }
        const int last_first = std::max(0, total_items - HOST_VISIBLE_ROWS);
        return std::min(first, last_first);
    }

    static bool touch_in_rect(const touchPosition &touch,
                              const MoonlightUi::Rect &rect) {
        return touch.px >= rect.left && touch.px <= rect.right &&
               touch.py >= rect.top && touch.py <= rect.bottom;
    }

    void activate_selected() {
        if (selected_index_ == static_cast<int>(addresses_.size()) + 1)
            result_ = Result::ExitToHome;
        else if (selected_index_ == static_cast<int>(addresses_.size()))
            result_ = Result::AddHost;
        else
            result_ = Result::SelectedHost;
    }
};

static void restore_consoles_after_dual_screen_ui() {
    diag_ui_state("TOP", "terminal text", false);
    diag_ui_state("BOTTOM", "terminal text", false);
    consoleSelect(&DebugTouchHandler::topScreen);
    DebugTouchHandler::topScreen.frameBuffer =
        (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    consoleClear();

    consoleSelect(&DebugTouchHandler::bottomScreen);
    DebugTouchHandler::bottomScreen.frameBuffer =
        (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    consoleClear();

    consoleSelect(&DebugTouchHandler::topScreen);
    n3ds_draw_diag_version_overlay();
}

static std::string prompt_for_host_text(const std::string &initial_text) {
    diag_ui_state("TOP", "native system UI", true);
    diag_ui_state("BOTTOM", "native system UI", true);
    SwkbdState swkbd;
    char addr_buff[MAX_INPUT_CHAR] = {};
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 3, -1);
    swkbdSetHintText(&swkbd, "Host address, optionally followed by :port");
    if (!initial_text.empty())
        swkbdSetInitialText(&swkbd, initial_text.c_str());
    const SwkbdButton button = swkbdInputText(&swkbd, addr_buff, MAX_INPUT_CHAR);
    diag_ui_state("TOP", "toolkit GUI", true);
    diag_ui_state("BOTTOM", "toolkit GUI", true);
    if (button != SWKBD_BUTTON_CONFIRM) return "";
    std::string addr_string(addr_buff);
    trim(addr_string);
    return addr_string;
}

static bool split_host_endpoint(const std::string &input,
                                uint16_t fallback_port,
                                std::string *address, uint16_t *port) {
    std::string value = input;
    trim(value);
    if (value.empty() || address == NULL || port == NULL) return false;
    const size_t separator = value.rfind(':');
    if (separator != std::string::npos) {
        if (separator + 1 >= value.size()) return false;
        const std::string port_text = value.substr(separator + 1);
        const bool numeric = std::all_of(
            port_text.begin(), port_text.end(),
            [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
        if (!numeric) return false;
        const unsigned long parsed = std::strtoul(port_text.c_str(), NULL, 10);
        if (parsed == 0 || parsed > 65535) return false;
        *address = value.substr(0, separator);
        *port = static_cast<uint16_t>(parsed);
        return !address->empty();
    }
    *address = value;
    *port = fallback_port;
    return true;
}

static std::string prompt_for_address() {
    for (;;) {
        HostPickerScreen screen(list_saved_host_entries());
        if (!MoonlightUi::runDualScreen(screen)) exit(0);
        restore_consoles_after_dual_screen_ui();

        if (screen.result() == HostPickerScreen::Result::Back) return "";
        if (screen.result() == HostPickerScreen::Result::SelectedHost)
            return screen.selected_address();
        if (screen.result() == HostPickerScreen::Result::ExitToHome)
            return "__EXIT_TO_HOME__";
        if (screen.result() == HostPickerScreen::Result::AddHost)
            return prompt_for_host_text("");
        if (screen.result() == HostPickerScreen::Result::EditHost) {
            const std::string old_entry = screen.selected_address();
            const std::string edited = prompt_for_host_text(old_entry);
            if (edited.empty()) continue;
            std::string address;
            uint16_t port = 47989;
            std::string old_address;
            uint16_t old_port = 47989;
            split_host_endpoint(old_entry, old_port, &old_address, &old_port);
            if (!split_host_endpoint(edited, old_port, &address, &port)) {
                show_gui_message("HOST ADDRESS INVALID",
                                 "The address or port is not valid.",
                                 "The original saved host was kept.");
                continue;
            }
            const std::string replacement =
                address + ":" + std::to_string(port);
            if (!replace_saved_host_entry(old_entry, replacement)) {
                show_gui_message("HOST ADDRESS NOT CHANGED",
                                 "This address is already saved or could not be written.",
                                 "The original host entry was kept.");
            } else {
                diag_log("HOST_ENTRY_EDITED", "result=saved");
            }
            continue;
        }
        if (screen.result() == HostPickerScreen::Result::DeleteHost) {
            const std::string entry = screen.selected_address();
            const std::vector<GuiMenuItem> choices = {
                {"Delete Saved Address", "Remove this entry from the host picker. Pairing on Sunshine is unchanged.", "Cannot undo"},
                {"Keep Host", "Leave the saved address in the host picker.", "No changes"},
            };
            int selected = 1;
            if (run_gui_menu("DELETE SAVED HOST", entry, choices, &selected) &&
                selected == 0) {
                if (remove_saved_host_entry(entry)) {
                    diag_log("HOST_ENTRY_DELETED", "result=saved_address_removed");
                    show_gui_message("HOST REMOVED",
                                     "The address was removed from the host picker.",
                                     "Sunshine pairing remains until you unpair separately.");
                } else {
                    show_gui_message("HOST NOT REMOVED",
                                     "Moonlight could not update the saved host list.",
                                     "The existing entry was kept.");
                }
            }
        }
    }
}
static int prompt_for_int(std::string initial_text) {
    const int previous = std::stoi(initial_text);
    char setting_buff[MAX_INPUT_CHAR] = {};
    SwkbdState swkbd;
    diag_ui_state("TOP", "native system UI", true);
    diag_ui_state("BOTTOM", "native system UI", true);
    swkbdInit(&swkbd, SWKBD_TYPE_NUMPAD, 2, 8);
    swkbdSetInitialText(&swkbd, initial_text.c_str());
    SwkbdButton button = swkbdInputText(&swkbd, setting_buff, MAX_INPUT_CHAR);
    diag_ui_state("TOP", "toolkit GUI", true);
    diag_ui_state("BOTTOM", "toolkit GUI", true);
    std::string setting_str = std::string(setting_buff);
    trim(setting_str);
    if (button != SWKBD_BUTTON_CONFIRM || setting_str.empty()) return previous;
    errno = 0;
    char *end = NULL;
    long value = strtol(setting_str.c_str(), &end, 10);
    if (errno == ERANGE || end == setting_str.c_str() || *end != '\0' ||
        value < INT_MIN || value > INT_MAX) return previous;
    return static_cast<int>(value);
}

static bool prompt_for_choice(const std::string &title,
                              const std::vector<GuiMenuItem> &options,
                              int initial, int *chosen) {
    int selected = initial;
    if (!run_gui_menu(title, "CHOOSE OPTION", options, &selected)) return false;
    *chosen = selected;
    return true;
}

static void prompt_for_stream_settings(PCONFIGURATION config) {
    struct SettingInfo { const char *name; const char *explanation; };
    static const SettingInfo settings[] = {
        {"width", "Requested host video width in pixels. Unsupported widths may be rejected or scaled."},
        {"height", "Requested host video height in pixels. 3DS-friendly heights are multiples of 240."},
        {"fps", "Target stream frame rate. Higher rates need more host and network capacity."},
        {"motion_controls", "Allow motion sensor data to be sent when the host requests it; this does not map tilt to the right stick."},
        {"bitrate", "Maximum video bitrate in kilobits per second. Higher values use more network bandwidth."},
        {"packetsize", "Maximum network packet size in bytes. Lower values can help some networks avoid fragmentation."},
        {"sops", "Ask the host to optimize game settings for streaming when supported."},
        {"localaudio", "Play audio locally on the PC instead of sending it to the 3DS."},
        {"quitappafter", "Ask the host to close the selected app after the stream ends."},
        {"viewonly", "Disable controller input sent from the 3DS while streaming."},
        {"video_decoder", "Select hardware decoding, slower software decoding, or disable video."},
        {"swapfacebuttons", "Swap A/B and X/Y to match the alternate controller layout."},
        {"swaptriggersandshoulders", "Swap L with ZL and R with ZR for the alternate shoulder mapping."},
        {"usetriggersformouse", "Use ZL and ZR as left and right mouse buttons."},
        {"diagnostic_logging", "Write Moonlight diagnostic events to sd:/3ds/moonlight/moonlight.log. Changes apply immediately and save when you leave this settings menu."},
    };
    int idx = 0;
    while (true) {
        std::vector<GuiMenuItem> items;
        for (const SettingInfo &info : settings) {
            std::string value;
            if (!strcmp(info.name, "width")) value = std::to_string(config->stream.width);
            else if (!strcmp(info.name, "height")) value = std::to_string(config->stream.height);
            else if (!strcmp(info.name, "fps")) value = std::to_string(config->stream.fps);
            else if (!strcmp(info.name, "motion_controls")) value = config->motion_controls ? "On" : "Off";
            else if (!strcmp(info.name, "bitrate")) value = std::to_string(config->stream.bitrate) + " kbps";
            else if (!strcmp(info.name, "packetsize")) value = std::to_string(config->stream.packetSize) + " bytes";
            else if (!strcmp(info.name, "sops")) value = config->sops ? "On" : "Off";
            else if (!strcmp(info.name, "localaudio")) value = config->localaudio ? "On" : "Off";
            else if (!strcmp(info.name, "quitappafter")) value = config->quitappafter ? "On" : "Off";
            else if (!strcmp(info.name, "viewonly")) value = config->viewonly ? "On" : "Off";
            else if (!strcmp(info.name, "video_decoder")) value = config->video_decoder == HARDWARE_VIDEO_DECODER ? "Hardware" : config->video_decoder == SOFTWARE_VIDEO_DECODER ? "Software" : "Disabled";
            else if (!strcmp(info.name, "swapfacebuttons")) value = config->swap_face_buttons ? "On" : "Off";
            else if (!strcmp(info.name, "swaptriggersandshoulders")) value = config->swap_triggers_and_shoulders ? "On" : "Off";
            else if (!strcmp(info.name, "usetriggersformouse")) value = config->use_triggers_for_mouse ? "On" : "Off";
            else value = config->diagnostic_logging ? "On" : "Off";
            items.push_back({info.name, info.explanation, value});
        }
        if (!run_gui_menu("STREAM SETTINGS", "SETTINGS", items, &idx)) break;
        const char *name = settings[idx].name;
        if (!strcmp(name, "width")) config->stream.width = prompt_for_int(std::to_string(config->stream.width));
        else if (!strcmp(name, "height")) config->stream.height = prompt_for_int(std::to_string(config->stream.height));
        else if (!strcmp(name, "fps")) config->stream.fps = prompt_for_int(std::to_string(config->stream.fps));
        else if (!strcmp(name, "bitrate")) config->stream.bitrate = prompt_for_int(std::to_string(config->stream.bitrate));
        else if (!strcmp(name, "packetsize")) config->stream.packetSize = prompt_for_int(std::to_string(config->stream.packetSize));
        else if (!strcmp(name, "video_decoder")) {
            int choice = static_cast<int>(config->video_decoder);
            std::vector<GuiMenuItem> opts = {
                {"Hardware", "Fast hardware decoder, available on New 3DS models.", "Fast"},
                {"Software", "CPU decoder; may be too slow for high-resolution streams.", "Slow"},
                {"Disable video", "Do not decode or display the video stream.", "Audio/input only"},
            };
            if (prompt_for_choice("VIDEO DECODER", opts, choice, &choice)) config->video_decoder = static_cast<VIDEO_DECODER_TYPE>(choice);
        } else {
            bool *value = nullptr;
            if (!strcmp(name, "motion_controls")) value = &config->motion_controls;
            else if (!strcmp(name, "sops")) value = &config->sops;
            else if (!strcmp(name, "localaudio")) value = &config->localaudio;
            else if (!strcmp(name, "quitappafter")) value = &config->quitappafter;
            else if (!strcmp(name, "viewonly")) value = &config->viewonly;
            else if (!strcmp(name, "swapfacebuttons")) value = &config->swap_face_buttons;
            else if (!strcmp(name, "swaptriggersandshoulders")) value = &config->swap_triggers_and_shoulders;
            else if (!strcmp(name, "usetriggersformouse")) value = &config->use_triggers_for_mouse;
            if (!strcmp(name, "diagnostic_logging")) {
                config->diagnostic_logging = !config->diagnostic_logging;
                diag_set_enabled(config->diagnostic_logging);
            } else if (value != nullptr) {
                int choice = *value ? 0 : 1;
                std::vector<GuiMenuItem> opts = {
                    {"On", settings[idx].explanation, "Enabled"},
                    {"Off", settings[idx].explanation, "Disabled"},
                };
                if (prompt_for_choice(name, opts, choice, &choice)) *value = choice == 0;
            }
        }
    }
    char *config_file_path = (char *)MOONLIGHT_3DS_PATH "/moonlight.conf";
    config_save(config_file_path, config);
}

struct StreamProfileRate {
    int fps;
    int bitrate;
};

struct StreamProfilePick {
    int resolution_index;
    int rate_index;
};

using StreamProfileTile = MoonlightUi::Rect;

static const int STREAM_PROFILE_WIDTHS[] = {400, 800, 1200};
static const int STREAM_PROFILE_HEIGHTS[] = {240, 480, 720};
static const StreamProfileRate STREAM_PROFILE_RATES[3][2] = {
    {{30, 3000}, {30, 5000}},
    {{60, 12000}, {30, 8000}},
    {{30, 3000}, {30, 6000}},
};

static const char *const STREAM_PROFILE_RATE_LABELS[3][2] = {
    {"Away — 30 fps · 3000 kbps", "Home light — 30 fps · 5000 kbps"},
    {"Home sharp — 60 fps · 12000 kbps", "Balanced — 30 fps · 8000 kbps"},
    {"Browse — 30 fps · 3000 kbps", "Play — 30 fps · 6000 kbps"},
};

static const StreamProfileTile STREAM_PROFILE_RESOLUTION_TILES[] = {
    {14, 75, 154, 142},
    {166, 75, 306, 142},
    {14, 151, 154, 218},
    {166, 151, 306, 218},
};

static const StreamProfileTile STREAM_PROFILE_RATE_TILES[] = {
    {14, 83, 306, 142},
    {14, 153, 306, 212},
};

static std::string stream_profile_store_path() {
    return std::string(MOONLIGHT_3DS_PATH) + "/stream_profiles.conf";
}

static bool valid_stream_profile_pick(const StreamProfilePick &pick) {
    return pick.resolution_index >= 0 && pick.resolution_index < 3 &&
           pick.rate_index >= 0 && pick.rate_index < 2;
}

static bool load_host_stream_profile(const std::string &host_key,
                                     StreamProfilePick *pick) {
    std::ifstream file(stream_profile_store_path());
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream record(line);
        std::string stored_host;
        std::string record_type;
        StreamProfilePick stored_pick{};
        if (!(record >> std::quoted(stored_host) >> record_type) ||
            stored_host != host_key || record_type == "T") {
            continue;
        }
        std::istringstream resolution_record(record_type);
        if (resolution_record >> stored_pick.resolution_index &&
            record >> stored_pick.rate_index &&
            valid_stream_profile_pick(stored_pick)) {
            *pick = stored_pick;
            return true;
        }
    }
    return false;
}

struct ConnectionTestProfile {
    int resolution_index;
    int fps30_bitrate;
    int fps60_bitrate;
};

static bool valid_connection_test_profile(
    const ConnectionTestProfile &profile) {
    return profile.resolution_index >= 0 && profile.resolution_index < 3 &&
           profile.fps30_bitrate > 0 && profile.fps60_bitrate > 0;
}

static bool load_connection_test_profile(
    const std::string &host_key, int resolution_index,
    ConnectionTestProfile *profile) {
    std::ifstream file(stream_profile_store_path());
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream record(line);
        std::string stored_host;
        std::string record_type;
        ConnectionTestProfile stored{};
        if ((record >> std::quoted(stored_host) >> record_type) &&
            stored_host == host_key && record_type == "T" &&
            record >> stored.resolution_index >> stored.fps30_bitrate >>
                stored.fps60_bitrate &&
            valid_connection_test_profile(stored) &&
            stored.resolution_index == resolution_index) {
            *profile = stored;
            return true;
        }
    }
    return false;
}

static bool save_connection_test_profile(
    const std::string &host_key, const ConnectionTestProfile &profile) {
    if (!valid_connection_test_profile(profile)) {
        return false;
    }
    const std::string path = stream_profile_store_path();
    std::vector<std::string> records;
    std::ifstream existing_file(path);
    std::string line;
    while (std::getline(existing_file, line)) {
        std::istringstream record(line);
        std::string stored_host;
        std::string record_type;
        int stored_resolution = -1;
        if ((record >> std::quoted(stored_host) >> record_type) &&
            stored_host == host_key && record_type == "T" &&
            record >> stored_resolution &&
            stored_resolution == profile.resolution_index) {
            continue;
        }
        records.push_back(line);
    }

    std::ofstream profile_file(path, std::ios::trunc);
    if (!profile_file) {
        return false;
    }
    for (const std::string &existing_record : records) {
        profile_file << existing_record << '\n';
    }
    profile_file << std::quoted(host_key) << " T "
                 << profile.resolution_index << ' ' << profile.fps30_bitrate
                 << ' ' << profile.fps60_bitrate << '\n';
    profile_file.close();
    return static_cast<bool>(profile_file);
}

static bool save_host_stream_profile(const std::string &host_key,
                                     const StreamProfilePick &pick) {
    const std::string path = stream_profile_store_path();
    std::vector<std::string> records;
    std::ifstream existing_file(path);
    std::string line;
    while (std::getline(existing_file, line)) {
        std::istringstream record(line);
        std::string stored_host;
        std::string record_type;
        if ((record >> std::quoted(stored_host) >> record_type) &&
            stored_host == host_key && record_type != "T") {
            continue;
        }
        records.push_back(line);
    }

    std::ofstream profile_file(path, std::ios::trunc);
    if (!profile_file) {
        return false;
    }
    for (const std::string &existing_record : records) {
        profile_file << existing_record << '\n';
    }
    profile_file << std::quoted(host_key) << ' ' << pick.resolution_index
                 << ' ' << pick.rate_index << '\n';
    profile_file.close();
    if (!profile_file) {
        return false;
    }
    return true;
}

static int stream_profile_resolution_for_config(PCONFIGURATION config) {
    for (int i = 0; i < 3; i++) {
        if (config->stream.width == STREAM_PROFILE_WIDTHS[i] &&
            config->stream.height == STREAM_PROFILE_HEIGHTS[i]) {
            return i;
        }
    }
    return 3;
}

static int stream_profile_rate_for_config(PCONFIGURATION config,
                                         int resolution_index) {
    long best_score = 0x7fffffff;
    int best_rate = 0;
    for (int i = 0; i < 2; i++) {
        const StreamProfileRate &rate =
            STREAM_PROFILE_RATES[resolution_index][i];
        long score = std::abs(config->stream.fps - rate.fps) * 100000L +
                     std::abs(config->stream.bitrate - rate.bitrate);
        if (score < best_score) {
            best_score = score;
            best_rate = i;
        }
    }
    return best_rate;
}

static void restore_bottom_console() {
    diag_ui_state("BOTTOM", "terminal text", false);
    PrintConsole *previous_console =
        consoleSelect(&DebugTouchHandler::bottomScreen);
    DebugTouchHandler::bottomScreen.frameBuffer =
        (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    consoleClear();
    consoleSelect(previous_console);
    n3ds_draw_diag_version_overlay();
}

static void apply_stream_profile(PCONFIGURATION config,
                                 const std::string &host_key,
                                 const StreamProfilePick &pick) {
    if (!save_host_stream_profile(host_key, pick)) {
        PrintConsole *previous_console =
            consoleSelect(&DebugTouchHandler::bottomScreen);
        consoleClear();
        printf("Unable to save the profile for this host.\n");
        printf("Check available storage, then press a button.\n");
        n3ds_draw_diag_version_overlay();
        consoleSelect(previous_console);
        wait_for_button();
        return;
    }

    const StreamProfileRate &rate =
        STREAM_PROFILE_RATES[pick.resolution_index][pick.rate_index];
    const int width = STREAM_PROFILE_WIDTHS[pick.resolution_index];
    const int height = STREAM_PROFILE_HEIGHTS[pick.resolution_index];
    config->stream.width = width;
    config->stream.height = height;
    config->stream.fps = rate.fps;
    config->stream.bitrate = rate.bitrate;

    char *config_file_path = (char *)MOONLIGHT_3DS_PATH "/moonlight.conf";
    config_save(config_file_path, config);
}

class StreamProfilePickerScreen : public MoonlightUi::ScreenModel {
  public:
    enum class Result {
        Cancelled,
        Custom,
        Picked,
    };

    StreamProfilePickerScreen(PCONFIGURATION config,
                              const std::string &host_key)
        : host_key_(host_key) {
        StreamProfilePick saved_pick{};
        const bool has_saved_pick =
            load_host_stream_profile(host_key_, &saved_pick);
        resolution_index_ =
            has_saved_pick ? saved_pick.resolution_index
                           : stream_profile_resolution_for_config(config);
        for (int resolution = 0; resolution < 3; ++resolution) {
            rate_index_by_resolution_[resolution] =
                stream_profile_rate_for_config(config, resolution);
        }
        if (has_saved_pick) {
            rate_index_by_resolution_[saved_pick.resolution_index] =
                saved_pick.rate_index;
        }
        selected_index_ = resolution_index_;
    }

    void draw(MoonlightUi::Screen &screen) override {
        if (page_ == 0) {
            screen.drawHeader("STREAM PROFILE",
                              "STEP 1 OF 2  /  RESOLUTION");
            for (int index = 0; index < 4; ++index) {
                const auto &tile = STREAM_PROFILE_RESOLUTION_TILES[index];
                screen.drawFilledTile(tile, index + 1);
                if (index < 3) {
                    const std::string resolution =
                        std::to_string(STREAM_PROFILE_WIDTHS[index]) + "x" +
                        std::to_string(STREAM_PROFILE_HEIGHTS[index]);
                    screen.drawLiveText(tile.left + 20, tile.top + 30,
                                        resolution,
                                        MoonlightUi::TextStyle::Tile,
                                        {38, 43, 48});
                } else {
                    screen.drawLiveText(tile.left + 22, tile.top + 27,
                                        "Custom",
                                        MoonlightUi::TextStyle::Tile,
                                        {38, 43, 48});
                    screen.drawLiveText(tile.left + 23, tile.top + 50,
                                        "MANUAL",
                                        MoonlightUi::TextStyle::Small,
                                        {0, 137, 153});
                }
            }
            screen.drawHighlight(STREAM_PROFILE_RESOLUTION_TILES[selected_index_]);
        } else {
            const std::string resolution =
                std::to_string(STREAM_PROFILE_WIDTHS[resolution_index_]) +
                "x" +
                std::to_string(STREAM_PROFILE_HEIGHTS[resolution_index_]);
            screen.drawHeader("STREAM PROFILE",
                              "STEP 2 OF 2  /  RATE  ·  " + resolution);
            for (int index = 0; index < 2; ++index) {
                const auto &tile = STREAM_PROFILE_RATE_TILES[index];
                screen.drawFilledTile(tile, index + 1);
                screen.drawAutoRateText(
                    tile.left + 27, tile.top + 25,
                    STREAM_PROFILE_RATE_LABELS[resolution_index_][index],
                    tile.right - tile.left - 54, {38, 43, 48});
            }
            screen.drawHighlight(STREAM_PROFILE_RATE_TILES[selected_index_]);
        }
        screen.drawFooter(DIAG_BUILD_VERSION, "MENU TAP PICK");
    }

    bool handleInput(const MoonlightUi::InputEvent &event,
                     bool *redraw) override {
        *redraw = false;
        if (event.type == MoonlightUi::InputEvent::Type::Back) {
            if (page_ == 1) {
                page_ = 0;
                selected_index_ = resolution_index_;
                *redraw = true;
                return true;
            }
            result_ = Result::Cancelled;
            return false;
        }
        if (event.type == MoonlightUi::InputEvent::Type::Confirm) {
            return activate_selection(redraw);
        }
        if (event.type == MoonlightUi::InputEvent::Type::Touch) {
            const int touched = tile_at(event.touch);
            if (touched < 0) {
                return true;
            }
            selected_index_ = touched;
            return activate_selection(redraw);
        }
        if (event.type == MoonlightUi::InputEvent::Type::Direction) {
            move_selection(event.direction, redraw);
        }
        return true;
    }

    Result result() const { return result_; }
    const StreamProfilePick &pick() const { return pick_; }

  private:
    std::string host_key_;
    int page_ = 0;
    int resolution_index_ = 0;
    int rate_index_by_resolution_[3] = {0, 0, 0};
    int selected_index_ = 0;
    Result result_ = Result::Cancelled;
    StreamProfilePick pick_{};

    int tile_at(const touchPosition &touch) const {
        const StreamProfileTile *tiles =
            page_ == 0 ? STREAM_PROFILE_RESOLUTION_TILES
                       : STREAM_PROFILE_RATE_TILES;
        const int tile_count = page_ == 0 ? 4 : 2;
        for (int index = 0; index < tile_count; ++index) {
            if (touch.px >= tiles[index].left &&
                touch.px <= tiles[index].right &&
                touch.py >= tiles[index].top &&
                touch.py <= tiles[index].bottom) {
                return index;
            }
        }
        return -1;
    }

    bool activate_selection(bool *redraw) {
        if (page_ == 0) {
            if (selected_index_ == 3) {
                result_ = Result::Custom;
                return false;
            }
            resolution_index_ = selected_index_;
            page_ = 1;
            selected_index_ = rate_index_by_resolution_[resolution_index_];
            *redraw = true;
            return true;
        }
        pick_ = {resolution_index_, selected_index_};
        result_ = Result::Picked;
        return false;
    }

    void move_selection(MoonlightUi::Direction direction, bool *redraw) {
        const int old_selection = selected_index_;
        if (page_ == 0) {
            switch (direction) {
            case MoonlightUi::Direction::Left:
                if (selected_index_ % 2 == 1) {
                    --selected_index_;
                }
                break;
            case MoonlightUi::Direction::Right:
                if (selected_index_ % 2 == 0) {
                    ++selected_index_;
                }
                break;
            case MoonlightUi::Direction::Up:
                if (selected_index_ >= 2) {
                    selected_index_ -= 2;
                }
                break;
            case MoonlightUi::Direction::Down:
                if (selected_index_ < 2) {
                    selected_index_ += 2;
                }
                break;
            }
        } else {
            if ((direction == MoonlightUi::Direction::Up ||
                 direction == MoonlightUi::Direction::Left) &&
                selected_index_ == 1) {
                selected_index_ = 0;
            } else if ((direction == MoonlightUi::Direction::Down ||
                        direction == MoonlightUi::Direction::Right) &&
                       selected_index_ == 0) {
                selected_index_ = 1;
            }
        }
        *redraw = selected_index_ != old_selection;
    }
};

static void prompt_for_stream_profile(PCONFIGURATION config,
                                      const std::string &host_key) {
    StreamProfilePickerScreen screen(config, host_key);
    if (!MoonlightUi::runScreen(screen)) {
        exit(0);
    }
    if (screen.result() == StreamProfilePickerScreen::Result::Custom) {
        restore_bottom_console();
        prompt_for_stream_settings(config);
        return;
    }
    if (screen.result() == StreamProfilePickerScreen::Result::Picked) {
        apply_stream_profile(config, host_key, screen.pick());
    }
    restore_bottom_console();
}

static void init_3ds() {
    Result status = 0;
    acInit();
    gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);

    consoleInit(GFX_TOP, &DebugTouchHandler::topScreen);
    consoleInit(GFX_BOTTOM, &DebugTouchHandler::bottomScreen);
    consoleSelect(&DebugTouchHandler::bottomScreen);
    consoleClear();
    n3ds_draw_diag_version_overlay();
    consoleSelect(&DebugTouchHandler::topScreen);
    diag_init();
    atexit(n3ds_exit_handler);

    osSetSpeedupEnable(true);
    aptSetSleepAllowed(false);
    aptInit();

    SOC_buffer = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    status = socInit(SOC_buffer, SOC_BUFFERSIZE);
    if (R_FAILED(status)) {
        printf("socInit: %08lX\n", status);
        exit(1);
    }

    status = ndmuInit();
    status |= NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
    status |= NDMU_LockState();
    if (R_FAILED(status)) {
        printf("Warning: failed to enter exclusive NDM state: %08lX\n", status);
        wait_for_button();
    }
}

struct AppListTaskContext {
    PSERVER_DATA server;
    PAPP_LIST list;
    int status;
};

struct DisplayListTaskContext {
    PSERVER_DATA server;
    PDISPLAY_INFO list;
    int status;
};

class GuiMessageScreen : public MoonlightUi::DualScreenModel {
  public:
    GuiMessageScreen(std::string title, std::string message, std::string hint,
                     std::string headline)
        : title_(std::move(title)), message_(std::move(message)),
          headline_(std::move(headline)),
          hint_(std::move(hint)) {}
    void draw(MoonlightUi::Screen &top, MoonlightUi::Screen &bottom) override {
        top.drawHeader("MOONLIGHT", title_);
        top.drawLiveText(20, 91, headline_, MoonlightUi::TextStyle::Header,
                         headline_ == "UPLOAD ACCEPTED" ?
                             MoonlightUi::Color{47, 151, 91} :
                             headline_ == "LOG NOT SENT" ?
                                 MoonlightUi::Color{194, 55, 61} :
                                 MoonlightUi::Color{38, 43, 48});
        top.drawFooter(DIAG_BUILD_VERSION, "RETURN TO HOST MENU");
        bottom.drawHeader("NEXT STEP", "YOU CAN GO BACK");
        bottom.drawAutoRateText(20, 82, message_, bottom.width() - 40,
                                {38, 43, 48});
        bottom.drawAutoRateText(20, 116, hint_, bottom.width() - 40,
                                {105, 114, 122});
        bottom.drawFooter(DIAG_BUILD_VERSION, "A OR B BACK");
    }
    bool handleInput(const MoonlightUi::InputEvent &event, bool *redraw) override {
        *redraw = false;
        if (event.type == MoonlightUi::InputEvent::Type::Back ||
            event.type == MoonlightUi::InputEvent::Type::Confirm) return false;
        return true;
    }
  private:
    std::string title_, message_, headline_, hint_;
};

static void show_gui_message(const std::string &title, const std::string &message,
                             const std::string &hint,
                             const std::string &headline) {
    GuiMessageScreen screen(title, message, hint, headline);
    MoonlightUi::runDualScreen(screen);
    restore_consoles_after_dual_screen_ui();
}

static bool ensure_upload_consent(PSERVER_DATA server) {
    bool known = false;
    bool enabled = false;
    if (server == NULL || !server->paired) {
        show_gui_message("HOST NOT PAIRED",
                         "Automatic uploads need this Sunshine host paired first.",
                         "Pair the host; local logs remain on the SD card.");
        return false;
    }
    if (!upload_pin_available(server)) {
        diag_log("UPLOAD_TRUST_UNAVAILABLE",
                 "reason=missing_host_certificate_pin recovery=repair_upload_trust");
        show_gui_message(
            "UPLOAD TRUST NEEDS REPAIR",
            "This host is paired, but its Sunshine certificate pin is missing.",
            "Choose Repair Upload Trust. Streaming works; logs stay local.");
        return false;
    }
    if (!host_upload_consent_get(server->serverInfo.address, server->httpPort,
                                 &known, &enabled)) {
        show_gui_message("UPLOAD CONSENT UNAVAILABLE",
                         "Moonlight could not read this host's upload choice.",
                         "Automatic uploads remain disabled; local logs are safe.");
        return false;
    }
    if (known) return enabled;
    std::vector<GuiMenuItem> choices = {
        {"Allow Automatic Uploads", "Diagnostic events can include host addresses, app names, and timings. The SD copy is always kept.", server->serverInfo.address},
        {"Keep Logs Local", "Do not send session logs automatically. Manual sends remain available.", "No automatic uploads"},
    };
    int selected = 1;
    run_gui_menu("DIAGNOSTIC UPLOAD CONSENT", "PAIRED SUNSHINE HOST",
                 choices, &selected);
    enabled = selected == 0;
    if (!host_upload_consent_set(server->serverInfo.address, server->httpPort,
                                 enabled)) {
        show_gui_message("CONSENT NOT SAVED",
                         "Moonlight could not save this host's upload choice.",
                         "Automatic uploads remain disabled.");
        return false;
    }
    return enabled;
}

static bool auto_upload_enabled(PSERVER_DATA server) {
    bool known = false;
    bool enabled = false;
    return server != NULL && server->paired &&
        host_upload_consent_get(server->serverInfo.address, server->httpPort,
                                &known, &enabled) && known && enabled;
}

static std::string json_quote(const std::string &value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result.push_back('\\'); result.push_back((char)c); }
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else if (c >= 0x20) result.push_back((char)c);
    }
    result.push_back('"');
    return result;
}

static bool sensitive_diagnostic_line(const std::string &line) {
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    static const char *blocked[] = {"pin", "secret", "private", "credential",
        "auth", "cert", "key", "pairing", "uniqueid", "uuid=", "?"};
    for (const char *needle : blocked)
        if (lower.find(needle) != std::string::npos) return true;
    return false;
}

static bool build_upload_json(const DIAG_SESSION &session, const char *outcome,
                              std::string &body, DIAG_UPLOAD_INFO *upload_info) {
    std::ifstream input(session.path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const std::streamoff source_size = input.tellg();
    if (source_size < 0) return false;
    input.seekg(0, std::ios::beg);
    if (!input) return false;
    std::string content, line;
    bool truncated = true; // v1 snapshot cannot attest full recording history.
    constexpr size_t content_limit = 240 * 1024;
    while (std::getline(input, line)) {
        line.push_back('\n');
        if (sensitive_diagnostic_line(line)) continue;
        const std::string encoded_line = json_quote(line);
        if (content.size() + encoded_line.size() - 2 > content_limit) {
            truncated = true;
            break;
        }
        content += line;
    }
    if (truncated) content += "[TRUNCATED AT LINE BOUNDARY]\n";
    unsigned char raw_id[16];
    if (RAND_bytes(raw_id, sizeof(raw_id)) != 1) return false;
    static const char hex[] = "0123456789abcdef";
    std::string upload_id(32, '0');
    for (size_t i = 0; i < sizeof(raw_id); ++i) {
        upload_id[i * 2] = hex[raw_id[i] >> 4];
        upload_id[i * 2 + 1] = hex[raw_id[i] & 0xf];
    }
    if (upload_info != NULL) {
        memset(upload_info, 0, sizeof(*upload_info));
        snprintf(upload_info->upload_id, sizeof(upload_info->upload_id), "%s",
                 upload_id.c_str());
        upload_info->source_bytes = static_cast<unsigned long long>(source_size);
        upload_info->filtered_bytes = static_cast<unsigned long long>(content.size());
    }
    const std::string normalized_id = make_sunshine_session_id("active-session", session.id,
                                                               session.begin_epoch);
    if (normalized_id.empty()) return false;
    const unsigned long long end_epoch = session.end_epoch ? session.end_epoch : (unsigned long long)time(NULL);
    const unsigned long long begin_epoch = session.begin_epoch > end_epoch ? end_epoch : session.begin_epoch;
    const std::string normalized_outcome =
        outcome && (strcmp(outcome, "stream_returned") == 0 ||
                    strcmp(outcome, "user_left_host") == 0 ||
                    strcmp(outcome, "app_exit") == 0 || strcmp(outcome, "unpair") == 0 ||
                    strcmp(outcome, "manual_current_host") == 0 ||
                    strcmp(outcome, "host_init_failed") == 0 ||
                    strcmp(outcome, "pair_action") == 0 || strcmp(outcome, "quit_stream") == 0)
            ? outcome : "manual_current_host";
    body = "{\"version\":1"
        ",\"scope\":\"host\""
        ",\"upload_id\":" + json_quote(upload_id) +
        ",\"session_id\":" + json_quote(normalized_id) +
        ",\"parent_id\":\"\""
        ",\"begin\":" + json_quote(std::to_string(begin_epoch)) +
        ",\"end\":" + json_quote(std::to_string(end_epoch)) +
        ",\"build\":" + json_quote(DIAG_BUILD_VERSION) +
        ",\"outcome\":" + json_quote(normalized_outcome) +
        ",\"truncated\":" + (truncated ? "true" : "false") +
        ",\"content\":" + json_quote(content) + "}";
    if (body.size() > 256 * 1024) return false;
    if (upload_info != NULL)
        {
        upload_info->request_bytes = static_cast<unsigned long long>(body.size());
        upload_info->protocol_version = 1;
        upload_info->truncated = true; // v1 cannot attest a complete segmented recording.
        upload_info->recording_complete = false;
    }
    return true;
}

static constexpr size_t DIAG_UPLOAD_RESPONSE_BODY_LIMIT = 1024;

static void log_upload_attempt(unsigned int attempt, int result,
                               PHTTP_DATA response,
                               const DIAG_UPLOAD_INFO *upload_info) {
    char response_summary[256] = "none";
    if (response != NULL && response->memory != NULL && response->size > 0) {
        size_t written = 0;
        for (size_t i = 0; i < response->size &&
                           written + 1 < sizeof(response_summary); ++i) {
            const unsigned char c =
                static_cast<unsigned char>(response->memory[i]);
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
                response_summary[written++] = static_cast<char>(c);
            } else {
                response_summary[written++] = '_';
            }
        }
        response_summary[written] = '\0';
    }
    diag_log("DIAG_UPLOAD_ATTEMPT",
             "attempt=%u result=%d http_status=%ld upload_id=%s source_bytes=%llu filtered_bytes=%llu request_bytes=%llu response_bytes=%lu response_truncated=%s response=%s",
             attempt, result, response ? response->response_code : 0,
             upload_info ? upload_info->upload_id : "",
             upload_info ? upload_info->source_bytes : 0,
             upload_info ? upload_info->filtered_bytes : 0,
             upload_info ? upload_info->request_bytes : 0,
             response ? static_cast<unsigned long>(response->size) : 0UL,
             response && response->response_truncated ? "true" : "false",
             response_summary);
}

static bool upload_session_legacy(PSERVER_DATA server, const DIAG_SESSION &session,
                           const char *outcome, DIAG_UPLOAD_INFO *upload_info) {
    if (server == NULL || !server->paired) return false;
    if (!upload_pin_available(server)) {
        diag_log("DIAG_UPLOAD_BLOCKED",
                 "reason=missing_host_certificate_pin recovery=repair_upload_trust");
        return false;
    }
    std::string body;
    DIAG_UPLOAD_INFO local_upload_info = {};
    DIAG_UPLOAD_INFO *info = upload_info != NULL ? upload_info : &local_upload_info;
    if (!build_upload_json(session, outcome, body, info)) return false;
    char url[1024];
    const int url_length = snprintf(url, sizeof(url), "https://%s:%u/client-diagnostics",
                                    server->serverInfo.address, server->httpsPort);
    if (url_length < 0 || (size_t)url_length >= sizeof(url)) return false;
    PHTTP_DATA response = http_create_data();
    if (response == NULL) return false;
    http_set_response_limit(response, DIAG_UPLOAD_RESPONSE_BODY_LIMIT);
    http_set_timeout_s(5);
    int result = gs_request_pinned(server, url, response, body.c_str(), body.size());
    log_upload_attempt(1, result, response, info);
    if (result != GS_OK &&
        (response->response_code == 0 || response->response_code >= 500)) {
        result = gs_request_pinned(server, url, response, body.c_str(), body.size());
        log_upload_attempt(2, result, response, info);
    }
    http_set_timeout_s(60);
    http_free_data(response);
    return result == GS_OK;
}

struct LogUploadTask {
    PSERVER_DATA server;
    DIAG_SESSION session;
    const char *outcome;
    DIAG_UPLOAD_INFO info{};
    AtomicVar<bool> done{false};
    LightLock lock;
    DiagnosticTransfer::Progress progress;
    DiagnosticTransfer::Result result;
    bool accepted = false;
};

static void log_upload_entry(void *context) {
    auto &task = *static_cast<LogUploadTask *>(context);
    try {
        DiagnosticTransfer::Capture capture;
        capture.path = task.session.path;
        capture.session_id = make_sunshine_session_id("active-session", task.session.id,
                                                     task.session.begin_epoch);
        capture.build = DIAG_BUILD_VERSION;
        capture.begin = task.session.begin_epoch;
        capture.end = task.session.end_epoch;
        const std::string origin = "https://" + std::string(task.server->serverInfo.address) +
                                   ":" + std::to_string(task.server->httpsPort);
        http_set_timeout_s(30);
        auto notify = [&task](const DiagnosticTransfer::Progress &progress) {
            LightLock_Lock(&task.lock);
            task.progress = progress;
            LightLock_Unlock(&task.lock);
        };
        auto transport = [&task, &origin](const std::string &path, const std::string &body,
                                         bool binary) {
            DiagnosticTransfer::Response reply;
            PHTTP_DATA response = http_create_data();
            if (!response) return reply;
            http_set_response_limit(response, DIAG_UPLOAD_RESPONSE_BODY_LIMIT);
            diag_upload_wire_progress(0, body.size());
            auto url = origin + path;
            const int status = binary ?
                gs_request_pinned_raw(task.server, url.data(), response, body.data(), body.size()) :
                gs_request_pinned(task.server, url.data(), response, body.data(), body.size());
            reply.transport_ok = status == GS_OK && !response->response_truncated;
            reply.status = response->response_code;
            if (response->memory && !response->response_truncated)
                reply.body.assign(response->memory, response->size);
            http_free_data(response);
            return reply;
        };
        task.result = DiagnosticTransfer::upload(capture, transport, notify,
                                                 [] { return diag_cancel_requested(); });
        if (task.result.unsupported && !diag_cancel_requested()) {
            DiagnosticTransfer::Progress legacy;
            legacy.phase = "Legacy host: partial log only";
            notify(legacy);
            task.accepted = upload_session_legacy(task.server, task.session, task.outcome, &task.info);
        } else {
            task.accepted = task.result.accepted;
            snprintf(task.info.upload_id, sizeof(task.info.upload_id), "%s", task.result.capture_id.c_str());
            task.info.source_bytes = task.result.source_bytes;
            task.info.filtered_bytes = task.result.content_bytes;
            task.info.request_bytes = task.result.request_bytes;
            task.info.protocol_version = 2;
            task.info.transfer_complete = task.result.accepted;
            task.info.recording_complete = task.result.complete;
            task.info.truncated = !task.result.complete;
        }
    } catch (const std::exception &error) {
        task.result.error = error.what();
    }
    http_set_timeout_s(60);
    task.done.store(true);
}

class LogUploadScreen : public MoonlightUi::DualScreenModel {
  public:
    explicit LogUploadScreen(LogUploadTask &task) : task_(task) {}
    void draw(MoonlightUi::Screen &top, MoonlightUi::Screen &bottom) override {
        LightLock_Lock(&task_.lock);
        const auto p = task_.progress;
        LightLock_Unlock(&task_.lock);
        const bool wire_phase = p.phase == "Sending to Sunshine" ||
                                p.phase == "Verifying full capture" ||
                                p.phase == "Complete capture accepted" ||
                                p.phase == "Retained log accepted; partial history";
        const bool connecting = p.phase == "Connecting to Sunshine";
        const std::string subtitle = cancelling_ ? "CANCELLING SAFELY" :
            wire_phase ? "PINNED HTTPS TO SUNSHINE" :
            connecting ? "CHECKING HOST ENDPOINT" : "LOCAL SD PREPARATION";
        top.drawHeader("SEND LOG", subtitle);
        top.drawAutoRateText(22, 78, p.phase.empty() ? "Starting upload worker..." : p.phase,
                            top.width() - 44, {38, 43, 48});
        if (wire_phase) {
            const unsigned percent = p.total ? static_cast<unsigned>(std::min<uint64_t>(100,
                                                 p.acknowledged * 100 / p.total)) : 0;
            top.drawProgressBar({22, 111, top.width() - 22, 134}, percent, {0, 137, 153});
            top.drawAutoRateText(22, 149, "Host confirmed: " + std::to_string(p.acknowledged / 1024) +
                                " / " + std::to_string(p.total / 1024) + " KiB", top.width() - 44, {38, 43, 48});
            top.drawAutoRateText(22, 178, "Part " + std::to_string(p.part) + " of " +
                                std::to_string(p.parts), top.width() - 44, {105, 114, 122});
        } else if (connecting) {
            top.drawAutoRateText(22, 116, "No log data has been sent yet.",
                                top.width() - 44, {38, 43, 48});
            top.drawAutoRateText(22, 150, "Waiting for the paired PC to respond.",
                                top.width() - 44, {105, 114, 122});
        } else {
            top.drawAutoRateText(22, 112, "Local entries checked: " +
                                std::to_string(p.local_entries_checked), top.width() - 44, {38, 43, 48});
            top.drawAutoRateText(22, 145, "Session data found: " +
                                std::to_string(p.local_retained_bytes / 1024) + " KiB",
                                top.width() - 44, {38, 43, 48});
            top.drawAutoRateText(22, 178, "Snapshot copied: " +
                                std::to_string(p.local_snapshot_bytes / 1024) + " KiB",
                                top.width() - 44, {105, 114, 122});
        }
        top.drawFooter(DIAG_BUILD_VERSION, "B CANCEL - RETRY IN CHECK LOGS");
        bottom.drawHeader("TRANSFER PROGRESS", connecting ? "CHECKING HOST" : "LOCAL SD TO PAIRED PC");
        if (wire_phase) {
            bottom.drawAutoRateText(18, 68, "Prepared SD data: " + std::to_string(p.prepared / 1024) +
                                   " / " + std::to_string(p.source / 1024) + " KiB", bottom.width() - 36, {38, 43, 48});
            unsigned long long sent = 0, total = 0;
            diag_upload_wire_snapshot(&sent, &total);
            bottom.drawAutoRateText(18, 100, "Current request: " + std::to_string(sent) +
                                   " / " + std::to_string(total) + " B", bottom.width() - 36, {38, 43, 48});
            bottom.drawProgressBar({18, 127, bottom.width() - 18, 145},
                total ? static_cast<unsigned>(std::min<unsigned long long>(100, sent * 100 / total)) : 0,
                {0, 137, 153});
            bottom.drawAutoRateText(18, 162, cancelling_ ? "Waiting for the current request to stop..." :
                "Final verification follows the last part.", bottom.width() - 36, {105, 114, 122});
        } else if (connecting) {
            bottom.drawAutoRateText(18, 72, "Connecting to the paired PC.",
                                   bottom.width() - 36, {38, 43, 48});
            bottom.drawAutoRateText(18, 105, "Local logs remain on this SD card.",
                                   bottom.width() - 36, {105, 114, 122});
        } else {
            bottom.drawAutoRateText(18, 68, "Local files checked: " +
                                   std::to_string(p.local_entries_checked), bottom.width() - 36, {38, 43, 48});
            bottom.drawAutoRateText(18, 100, "Session data found: " +
                                   std::to_string(p.local_retained_bytes / 1024) + " KiB",
                                   bottom.width() - 36, {38, 43, 48});
            bottom.drawAutoRateText(18, 132, "Snapshot copied: " +
                                   std::to_string(p.local_snapshot_bytes / 1024) + " KiB",
                                   bottom.width() - 36, {38, 43, 48});
            bottom.drawAutoRateText(18, 164, cancelling_ ? "Stopping at the next SD checkpoint..." :
                "Host transfer starts after local preparation.", bottom.width() - 36, {105, 114, 122});
        }
        bottom.drawAutoRateText(18, 188, "Local logs remain on your SD card.", bottom.width() - 36, {105, 114, 122});
        bottom.drawFooter(DIAG_BUILD_VERSION, "B CANCEL - LOCAL COPY RETAINED");
    }
    bool update(bool *redraw) override { *redraw = ++frames_ % 6 == 0; return !task_.done.load(); }
    bool handleInput(const MoonlightUi::InputEvent &event, bool *redraw) override {
        *redraw = false;
        if (event.type == MoonlightUi::InputEvent::Type::Back) {
            cancelling_ = true; diag_request_cancel(); *redraw = true;
        }
        return true;
    }
  private:
    LogUploadTask &task_;
    unsigned frames_ = 0;
    bool cancelling_ = false;
};

static bool upload_session(PSERVER_DATA server, const DIAG_SESSION &session,
                           const char *outcome, DIAG_UPLOAD_INFO *upload_info) {
    if (!server || !server->paired || !upload_pin_available(server)) return false;
    LogUploadTask task;
    task.server = server; task.session = session; task.outcome = outcome;
    LightLock_Init(&task.lock);
    diag_cancel_reset();
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    Thread worker = threadCreate(log_upload_entry, &task, 0x20000, priority, -1, false);
    if (!worker) { diag_log("DIAG_UPLOAD_ERROR", "reason=worker_unavailable"); return false; }
    LogUploadScreen screen(task);
    if (!MoonlightUi::runDualScreen(screen) && !task.done.load()) diag_request_cancel();
    threadJoin(worker, 0xFFFFFFFFFFFFFFFFULL);
    threadFree(worker);
    restore_consoles_after_dual_screen_ui();
    log_inventory_cache_at = 0;
    if (upload_info) *upload_info = task.info;
    diag_log("DIAG_TRANSFER_RESULT",
        "protocol=%u capture_id=%s transfer_complete=%s recording_complete=%s source_bytes=%llu content_bytes=%llu cancelled=%s error=%s",
        task.info.protocol_version, task.info.upload_id, task.accepted ? "true" : "false",
        task.info.recording_complete ? "true" : "false", task.info.source_bytes,
        task.info.filtered_bytes, task.result.cancelled ? "true" : "false", task.result.error.c_str());
    diag_cancel_reset();
    return task.accepted;
}


static void finish_session(PSERVER_DATA server, const char *outcome,
                           const char *reason) {
    DIAG_SESSION session{};
    const bool captured = diag_session_get_info(&session);
    diag_session_end(outcome, reason);
    if (!captured || !auto_upload_enabled(server)) return;
    DIAG_UPLOAD_INFO upload_info = {};
    const bool sent = upload_session(server, session, outcome, &upload_info);
    diag_session_record_upload(&session, sent, &upload_info);
    show_gui_message(sent ? "LOGS SENT" : "LOG UPLOAD FAILED",
                     sent ? (upload_info.recording_complete ? "Sunshine verified the full recording." : "Retained log accepted; history is partial.")
                          : "The session log remains saved on the SD card.",
                     sent ? "The local copy is still available in Check Logs."
                          : "See DIAG_UPLOAD_ATTEMPT in Check Logs.",
                     sent ? (upload_info.recording_complete ? "FULL RECORDING ACCEPTED" : "PARTIAL HISTORY ACCEPTED") : "PAUSED OR NOT SENT");
}

static void send_current_session(PSERVER_DATA server) {
    DIAG_SESSION session{};
    if (server == NULL || !server->paired || !diag_session_get_info(&session)) {
        show_gui_message("LOGS UNAVAILABLE",
                         "There is no active host session log to send.",
                         "Local logs remain available in Check Logs.");
        return;
    }
    std::vector<GuiMenuItem> choices = {
        {"Send This Session", "Send only this host-selection session over pinned HTTPS.", server->serverInfo.address},
        {"Cancel", "Keep the log local on the SD card.", "No upload"},
    };
    int selected = 1;
    if (!run_gui_menu("CONFIRM LOG UPLOAD", "THIS HOST SESSION ONLY",
                      choices, &selected) || selected != 0) return;
    DIAG_UPLOAD_INFO upload_info = {};
    const bool sent = upload_session(server, session, "manual_send", &upload_info);
    diag_session_record_upload(&session, sent, &upload_info);
    show_gui_message(sent ? "LOGS SENT" : "LOG UPLOAD FAILED",
                     sent ? (upload_info.recording_complete ? "Sunshine verified the full recording." : "Retained log accepted; history is partial.")
                          : "The session log remains saved on the SD card.",
                     sent ? "The local copy is still available in Check Logs."
                          : "See DIAG_UPLOAD_ATTEMPT in Check Logs.",
                     sent ? (upload_info.recording_complete ? "FULL RECORDING ACCEPTED" : "PARTIAL HISTORY ACCEPTED") : "PAUSED OR NOT SENT");
}

static void toggle_automatic_upload(PSERVER_DATA server) {
    bool known = false;
    bool enabled = false;
    if (server == NULL || !server->paired ||
        !host_upload_consent_get(server->serverInfo.address, server->httpPort,
                                 &known, &enabled)) {
        ensure_upload_consent(server);
        return;
    }
    if (!known) {
        ensure_upload_consent(server);
        return;
    }
    if (host_upload_consent_set(server->serverInfo.address, server->httpPort,
                                !enabled)) {
        show_gui_message("AUTOMATIC LOG UPLOAD",
                         !enabled ? "Automatic uploads are enabled for this paired host."
                                  : "Automatic uploads are disabled for this paired host.",
                         "The local session log is always retained.");
    }
}

static bool run_gui_call(const char *phase, const std::string &title,
                         DiagnosticTaskFunction function, void *context,
                         bool cancellable_http = true,
                         bool interrupt_connection = false,
                         const std::string &detail = std::string());

static void app_list_task(void *opaque) {
    AppListTaskContext *task = static_cast<AppListTaskContext *>(opaque);
    task->status = gs_applist(task->server, &task->list);
}

static void display_list_task(void *opaque) {
    DisplayListTaskContext *task = static_cast<DisplayListTaskContext *>(opaque);
    task->status = gs_display_list(task->server, &task->list);
}

static bool choose_host_default_display(std::string *selected_display_id,
                                        const std::string &reason) {
    std::vector<GuiMenuItem> choices = {
        {"Use Host Default", reason,
         "Sunshine's configured capture display"},
        {"Back", "Return to the host list without launching an app.",
         "No stream started"},
    };
    int selected = 1;
    if (!run_gui_menu("DISPLAY LIST UNAVAILABLE", "CHOOSE HOW TO CONTINUE",
                      choices, &selected) || selected != 0) {
        return false;
    }
    selected_display_id->clear();
    diag_log("DISPLAY_SELECTED", "source=host_default");
    return true;
}

static bool prompt_for_display_id(PSERVER_DATA server,
                                  std::string *selected_display_id) {
    if (selected_display_id == NULL)
        return false;
    selected_display_id->clear();

    DisplayListTaskContext task{server, NULL, GS_ERROR};
    const bool cancelled = run_gui_call(
        "connect.display_list", "Finding host displays", display_list_task,
        &task, true, false, "Enumerating capture-supported displays");
    if (cancelled || diag_cancel_requested()) {
        gs_free_display_list(task.list);
        diag_log("CONNECT_CANCELLED", "phase=connect.display_list");
        return false;
    }
    if (task.status != GS_OK) {
        const std::string reason =
            "Moonlight could not enumerate displays on this host.";
        diag_log("DISPLAY_LIST_ERROR", "status=%d detail=%s", task.status,
                 gs_error != NULL ? gs_error : "unknown");
        const bool use_default = choose_host_default_display(
            selected_display_id, reason + " " +
            "You can continue with Sunshine's configured display.");
        gs_free_display_list(task.list);
        return use_default;
    }

    std::vector<GuiMenuItem> display_items;
    std::vector<std::string> display_ids;
    for (PDISPLAY_INFO display = task.list; display != NULL;
         display = display->next) {
        if (display->id == NULL || display->id[0] == '\0')
            continue;
        const std::string id(display->id);
        const std::string name = display->name != NULL && display->name[0] != '\0'
            ? std::string(display->name) : id;
        display_items.push_back({name,
            "Use this capture-supported host display for the selected app.",
            id});
        display_ids.push_back(id);
    }
    gs_free_display_list(task.list);

    if (display_items.empty()) {
        return choose_host_default_display(
            selected_display_id,
            "The host returned no named display IDs. You can continue with "
            "Sunshine's configured display.");
    }

    display_items.push_back({"Use Host Default",
        "Use Sunshine's configured capture display instead of a specific ID.",
        "Configured Sunshine display"});
    int selected = 0;
    if (!run_gui_menu("SELECT A DISPLAY", "SUNSHINE HOST", display_items,
                      &selected)) {
        return false;
    }
    if (selected < static_cast<int>(display_ids.size())) {
        *selected_display_id = display_ids[static_cast<size_t>(selected)];
        diag_log("DISPLAY_SELECTED", "source=enumerated index=%d", selected);
    } else {
        diag_log("DISPLAY_SELECTED", "source=host_default");
    }
    return true;
}

static int prompt_for_app_id(PSERVER_DATA server) {
    PAPP_LIST list = NULL;
    AppListTaskContext task{server, NULL, GS_ERROR};
    run_gui_call("connect.app_list", "Loading host app list", app_list_task,
                 &task, true, false, "Requesting available apps");
    if (diag_cancel_requested()) {
        diag_log("CONNECT_CANCELLED", "phase=connect.applist");
        return -1;
    }
    list = task.list;
    if (task.status != GS_OK) {
        diag_log("APP_LIST_ERROR", "status=%d detail=%s", task.status, gs_error);
        show_gui_message("APP LIST UNAVAILABLE",
                         "Moonlight could not load apps from this host.",
                         "Check the host, then try again.");
        return -1;
    }

    std::vector<std::string> app_names;
    std::vector<int> app_ids;
    while (list != NULL) {
        PAPP_LIST next = list->next;
        if (list->name != NULL && list->name[0] != '\0' && list->id > 0) {
            app_names.push_back(std::string(list->name));
            app_ids.push_back(list->id);
        }
        free(list->name);
        free(list);
        list = next;
    }
    if (app_names.empty()) {
        show_gui_message("NO APPS AVAILABLE",
                         "The host returned an empty app list.",
                         "Add an app on the host, then try again.");
        return -1;
    }

    std::vector<GuiMenuItem> app_items;
    for (const std::string &name : app_names)
        app_items.push_back({name, "Launch this application on the selected Sunshine host.", "Host app"});
    int id_idx = 0;
    if (!run_gui_menu("SELECT AN APP", "SUNSHINE", app_items, &id_idx)) return -1;
    return app_ids[id_idx];
}

struct GuiCallTask {
    DiagnosticTaskFunction function;
    void *context;
    AtomicVar<bool> done{false};
    bool cancellable_http;
    bool interrupt_connection;
};

static void gui_call_entry(void *opaque) {
    GuiCallTask *task = static_cast<GuiCallTask *>(opaque);
    task->function(task->context);
    task->done.store(true);
}

class GuiCallProgressScreen : public MoonlightUi::ScreenModel {
  public:
    GuiCallProgressScreen(GuiCallTask *task, const std::string &title,
                          const std::string &detail = std::string())
        : task_(task), title_(title), detail_(detail) {}

    void draw(MoonlightUi::Screen &screen) override {
        screen.drawHeader("CONNECTING", title_);
        screen.drawLiveText(20, 98,
                            cancelling_ ? "Cancelling request..."
                                        : "Waiting for the host...",
                            MoonlightUi::TextStyle::Header, {38, 43, 48});
        screen.drawLiveText(20, 137,
                            cancelling_ ? "Waiting for cleanup"
                                        : detail_.empty() ? "Waiting for host response" : detail_,
                            MoonlightUi::TextStyle::Small, {105, 114, 122});
        screen.drawFooter(DIAG_BUILD_VERSION,
                          task_->cancellable_http || task_->interrupt_connection
                              ? "B CANCEL"
                              : "PLEASE WAIT");
    }

    bool update(bool *redraw) override {
        ++frames_;
        *redraw = frames_ % 15 == 0;
        return !task_->done.load();
    }

    bool handleInput(const MoonlightUi::InputEvent &event,
                     bool *redraw) override {
        *redraw = false;
        if (event.type == MoonlightUi::InputEvent::Type::Back && !cancelling_ &&
            (task_->cancellable_http || task_->interrupt_connection)) {
            cancelling_ = true;
            if (task_->interrupt_connection) {
                LiInterruptConnection();
            }
            if (task_->cancellable_http) {
                diag_request_cancel();
            }
            *redraw = true;
        }
        return true;
    }

    bool cancelled() const { return cancelling_; }

  private:
    GuiCallTask *task_;
    std::string title_;
    std::string detail_;
    unsigned int frames_ = 0;
    bool cancelling_ = false;
};

static bool run_gui_call(const char *phase, const std::string &title,
                         DiagnosticTaskFunction function, void *context,
                         bool cancellable_http,
                         bool interrupt_connection,
                         const std::string &detail) {
    GuiCallTask task{function, context, {false}, cancellable_http,
                     interrupt_connection};
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    diag_cancel_reset();
    diag_set_phase(phase);
    Thread worker = threadCreate(gui_call_entry, &task, 0x20000, priority, -1,
                                 false);
    if (worker == NULL) {
        diag_log("GUI_TASK_ERROR", "phase=%s reason=threadCreate failed",
                 phase);
        return false;
    }
    GuiCallProgressScreen screen(&task, title, detail);
    const bool screen_finished = MoonlightUi::runScreen(screen);
    if (!screen_finished && !task.done.load()) {
        if (interrupt_connection) {
            LiInterruptConnection();
        }
        if (cancellable_http) {
            diag_request_cancel();
        }
    }
    threadJoin(worker, 0xFFFFFFFFFFFFFFFFULL);
    threadFree(worker);
    diag_phase_outcome(phase, screen.cancelled() ? "cancelled" : "returned",
                       screen.cancelled() ? -1 : GS_OK);
    restore_bottom_console();
    return screen.cancelled() || !screen_finished;
}

class PairingProgressScreen : public MoonlightUi::DualScreenModel {
  public:
    PairingProgressScreen(GuiCallTask *task, const std::string &pin,
                          bool version_two)
        : task_(task), pin_(pin), version_two_(version_two) {}

    void draw(MoonlightUi::Screen &top,
              MoonlightUi::Screen &bottom) override {
        top.drawHeader("MOONLIGHT", version_two_ ? "PAIR WITH SUNSHINE"
                                                   : "PAIRING IN PROGRESS");
        if (version_two_) {
            const std::string instruction = "ENTER THIS PIN ON YOUR PC";
            const int instruction_x =
                (top.width() - top.measureLiveText(
                                   instruction, MoonlightUi::TextStyle::Header)) /
                2;
            top.drawLiveText(instruction_x, 73, instruction,
                             MoonlightUi::TextStyle::Header, {38, 43, 48});
            const int pin_width = top.measureLiveText(
                                     pin_, MoonlightUi::TextStyle::Tile) * 3;
            top.drawLiveTextScaled((top.width() - pin_width) / 2, 108, pin_,
                                   MoonlightUi::TextStyle::Tile,
                                   {0, 137, 153}, 3);
            top.drawLiveText(72, 178, "Enter it in Sunshine to pair.",
                             MoonlightUi::TextStyle::Small, {105, 114, 122});
        } else {
            top.drawLiveText(24, 85, "PAIR WITH YOUR SUNSHINE HOST",
                             MoonlightUi::TextStyle::Header, {38, 43, 48});
            top.drawLiveText(24, 116, "Version 2 shows the PIN larger.",
                             MoonlightUi::TextStyle::Small, {105, 114, 122});
        }
        top.drawFooter(DIAG_BUILD_VERSION,
                       task_->cancellable_http ? "B CANCEL" : "PLEASE WAIT");

        bottom.drawHeader("PAIRING", version_two_ ? "VERSION 2" : "VERSION 1");
        if (version_two_) {
            bottom.drawLiveText(20, 76, cancelling_ ? "Cancelling pairing..."
                                                     : "Waiting for Sunshine...",
                                MoonlightUi::TextStyle::Header, {38, 43, 48});
            bottom.drawAutoRateText(
                20, 111,
                "Type the large four-digit PIN from the top screen into Sunshine.",
                bottom.width() - 40, {105, 114, 122});
        } else {
            bottom.drawHeader("CONNECTING", "PAIRING WITH HOST");
            bottom.drawLiveText(20, 98, cancelling_ ? "Cancelling request..."
                                                     : "Waiting for the host...",
                                MoonlightUi::TextStyle::Header, {38, 43, 48});
            bottom.drawAutoRateText(
                20, 137, "Enter the PIN shown on this screen: " + pin_,
                bottom.width() - 40, {105, 114, 122});
        }

        const MoonlightUi::Rect version_one{16, 176, 158, 211};
        const MoonlightUi::Rect version_two{162, 176, 304, 211};
        const MoonlightUi::Color active_fill{79, 86, 97};
        const MoonlightUi::Color inactive_fill{222, 227, 231};
        const MoonlightUi::Color active_text{255, 255, 255};
        const MoonlightUi::Color inactive_text{38, 43, 48};
        bottom.drawPanelButton(version_one, "VERSION 1",
                               version_two_ ? inactive_fill : active_fill,
                               version_two_ ? inactive_text : active_text);
        bottom.drawPanelButton(version_two, "VERSION 2",
                               version_two_ ? active_fill : inactive_fill,
                               version_two_ ? active_text : inactive_text);
        bottom.drawFooter(DIAG_BUILD_VERSION, "L/R SWITCH  B CANCEL");
    }

    bool update(bool *redraw) override {
        ++frames_;
        *redraw = frames_ % 15 == 0;
        return !task_->done.load();
    }

    bool handleInput(const MoonlightUi::InputEvent &event,
                     bool *redraw) override {
        *redraw = false;
        if (event.type == MoonlightUi::InputEvent::Type::Back &&
            !cancelling_ && task_->cancellable_http) {
            cancelling_ = true;
            diag_request_cancel();
            *redraw = true;
        } else if (event.type == MoonlightUi::InputEvent::Type::Direction &&
                   (event.direction == MoonlightUi::Direction::Left ||
                    event.direction == MoonlightUi::Direction::Right)) {
            set_version(event.direction == MoonlightUi::Direction::Right,
                        redraw);
        } else if (event.type == MoonlightUi::InputEvent::Type::Touch &&
                   event.touch.py >= 176 && event.touch.py <= 211 &&
                   event.touch.px >= 16 && event.touch.px <= 304) {
            set_version(event.touch.px >= 160, redraw);
        }
        return true;
    }

    bool cancelled() const { return cancelling_; }

  private:
    GuiCallTask *task_;
    std::string pin_;
    unsigned int frames_ = 0;
    bool version_two_ = false;
    bool cancelling_ = false;

    void set_version(bool version_two, bool *redraw) {
        if (version_two_ == version_two) return;
        version_two_ = version_two;
        if (!set_pairing_layout_v2_preferred(version_two_)) {
            diag_log("PAIRING_LAYOUT_PREFERENCE",
                     "version=%d saved=false", version_two_ ? 2 : 1);
        } else {
            diag_log("PAIRING_LAYOUT_PREFERENCE",
                     "version=%d saved=true", version_two_ ? 2 : 1);
        }
        *redraw = true;
    }
};

static bool run_pairing_gui_call(const char *phase,
                                 DiagnosticTaskFunction function,
                                 void *context, const std::string &pin,
                                 bool version_two) {
    GuiCallTask task{function, context, {false}, true, false};
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    diag_cancel_reset();
    diag_set_phase(phase);
    Thread worker = threadCreate(gui_call_entry, &task, 0x20000, priority, -1,
                                 false);
    if (worker == NULL) {
        diag_log("GUI_TASK_ERROR", "phase=%s reason=threadCreate failed",
                 phase);
        return false;
    }
    PairingProgressScreen screen(&task, pin, version_two);
    const bool screen_finished = MoonlightUi::runDualScreen(screen);
    if (!screen_finished && !task.done.load()) diag_request_cancel();
    threadJoin(worker, 0xFFFFFFFFFFFFFFFFULL);
    threadFree(worker);
    diag_phase_outcome(phase, screen.cancelled() ? "cancelled" : "returned",
                       screen.cancelled() ? -1 : GS_OK);
    restore_consoles_after_dual_screen_ui();
    return screen.cancelled() || !screen_finished;
}

struct ConnectionTestApp {
    int id;
    std::string name;
};

static std::string fit_connection_test_text(
    const MoonlightUi::Screen &screen, std::string text, int max_width,
    MoonlightUi::TextStyle style) {
    if (screen.measureLiveText(text, style) <= max_width) {
        return text;
    }
    while (!text.empty() &&
           screen.measureLiveText(text + "...", style) > max_width) {
        size_t last = text.size() - 1;
        while (last > 0 &&
               (static_cast<unsigned char>(text[last]) & 0xC0) == 0x80) {
            --last;
        }
        text.erase(last);
    }
    return text + "...";
}

class ConnectionTestAppPickerScreen : public MoonlightUi::ScreenModel {
  public:
    explicit ConnectionTestAppPickerScreen(
        const std::vector<ConnectionTestApp> &apps)
        : apps_(apps) {}

    enum class Result { Cancelled, Selected };

    void draw(MoonlightUi::Screen &screen) override {
        screen.drawHeader("CONNECTION TEST", "CHOOSE A HOST APP");
        const int visible = visible_count();
        const int first = first_visible();
        for (int row = 0; row < visible; ++row) {
            const int index = first + row;
            if (index >= static_cast<int>(apps_.size())) {
                break;
            }
            const int y = 68 + row * 24;
            const std::string visible_name = fit_connection_test_text(
                screen, apps_[index].name, 250,
                MoonlightUi::TextStyle::Header);
            screen.drawListRow({14, y, 306, y + 21}, index + 1,
                               visible_name, index == selected_index_);
        }
        screen.drawFooter(DIAG_BUILD_VERSION, "A CHOOSE  B BACK");
    }

    bool handleInput(const MoonlightUi::InputEvent &event,
                     bool *redraw) override {
        *redraw = false;
        if (event.type == MoonlightUi::InputEvent::Type::Back) {
            result_ = Result::Cancelled;
            return false;
        }
        if (apps_.empty()) {
            return true;
        }
        if (event.type == MoonlightUi::InputEvent::Type::Confirm) {
            result_ = Result::Selected;
            return false;
        }
        if (event.type == MoonlightUi::InputEvent::Type::Touch) {
            const int first = first_visible();
            const int row = (event.touch.py - 68) / 24;
            const int index = first + row;
            if (event.touch.py >= 68 && row >= 0 && row < visible_count() &&
                index < static_cast<int>(apps_.size())) {
                selected_index_ = index;
                result_ = Result::Selected;
                return false;
            }
            return true;
        }
        if (event.type == MoonlightUi::InputEvent::Type::Direction) {
            const int old = selected_index_;
            if (event.direction == MoonlightUi::Direction::Up &&
                selected_index_ > 0) {
                --selected_index_;
            } else if (event.direction == MoonlightUi::Direction::Down &&
                       selected_index_ + 1 < static_cast<int>(apps_.size())) {
                ++selected_index_;
            }
            *redraw = old != selected_index_;
        }
        return true;
    }

    Result result() const { return result_; }
    const ConnectionTestApp &selected_app() const {
        return apps_[selected_index_];
    }

  private:
    const std::vector<ConnectionTestApp> &apps_;
    int selected_index_ = 0;
    Result result_ = Result::Cancelled;

    int visible_count() const {
        return std::max(1, std::min(6, static_cast<int>(apps_.size())));
    }
    int first_visible() const {
        return std::max(0, std::min(selected_index_ - visible_count() + 1,
                                    static_cast<int>(apps_.size()) -
                                        visible_count()));
    }
};

class ConnectionTestQualityScreen : public MoonlightUi::ScreenModel {
  public:
    enum class Result { Cancelled, Selected };
    static const MoonlightUi::Rect TILES[3];

    void draw(MoonlightUi::Screen &screen) override {
        screen.drawHeader("CONNECTION TEST", "CHOOSE ONE QUALITY BRACKET");
        for (int index = 0; index < 3; ++index) {
            const auto &tile = TILES[index];
            screen.drawFilledTile(tile, index + 1);
            screen.drawLiveText(tile.left + 20, tile.top + 25,
                                std::to_string(STREAM_PROFILE_WIDTHS[index]) +
                                    "x" +
                                    std::to_string(STREAM_PROFILE_HEIGHTS[index]),
                                MoonlightUi::TextStyle::Tile,
                                {38, 43, 48});
        }
        screen.drawHighlight(TILES[selected_index_]);
        screen.drawFooter(DIAG_BUILD_VERSION, "A CHOOSE  B BACK");
    }

    bool handleInput(const MoonlightUi::InputEvent &event,
                     bool *redraw) override {
        *redraw = false;
        if (event.type == MoonlightUi::InputEvent::Type::Back) {
            result_ = Result::Cancelled;
            return false;
        }
        if (event.type == MoonlightUi::InputEvent::Type::Confirm) {
            result_ = Result::Selected;
            return false;
        }
        if (event.type == MoonlightUi::InputEvent::Type::Touch) {
            for (int index = 0; index < 3; ++index) {
                const auto &tile = TILES[index];
                if (event.touch.px >= tile.left &&
                    event.touch.px <= tile.right &&
                    event.touch.py >= tile.top &&
                    event.touch.py <= tile.bottom) {
                    selected_index_ = index;
                    result_ = Result::Selected;
                    return false;
                }
            }
            return true;
        }
        if (event.type == MoonlightUi::InputEvent::Type::Direction) {
            const int old = selected_index_;
            if ((event.direction == MoonlightUi::Direction::Left ||
                 event.direction == MoonlightUi::Direction::Up) &&
                selected_index_ > 0) {
                --selected_index_;
            } else if ((event.direction == MoonlightUi::Direction::Right ||
                        event.direction == MoonlightUi::Direction::Down) &&
                       selected_index_ < 2) {
                ++selected_index_;
            }
            *redraw = old != selected_index_;
        }
        return true;
    }

    Result result() const { return result_; }
    int resolution_index() const { return selected_index_; }

  private:
    int selected_index_ = 0;
    Result result_ = Result::Cancelled;
};

const MoonlightUi::Rect ConnectionTestQualityScreen::TILES[3] = {
    {14, 75, 154, 142}, {166, 75, 306, 142}, {90, 151, 230, 218}};

static const int CONNECTION_TEST_BITRATES[3][3] = {
    {1000, 2000, 3000}, {3000, 6000, 9000}, {5000, 10000, 15000}};
static const unsigned int CONNECTION_TEST_RUNG_MS = 8000;
static const size_t CONNECTION_TEST_MAX_SAMPLES = 600;

struct ConnectionTestMetricsSnapshot {
    unsigned int frames = 0;
    std::vector<uint16_t> frame_intervals_ms;
    std::vector<uint16_t> frame_receive_spans_ms;
    std::vector<uint16_t> host_processing_ms;
    std::vector<uint16_t> local_submit_ms;
    std::vector<uint16_t> control_rtt_ms;
    std::vector<uint16_t> control_rtt_variance_ms;
};

class ConnectionTestMetrics {
  public:
    void reset() {
        lock_.lock();
        frames_ = 0;
        previous_receive_us_ = 0;
        clear_series(frame_intervals_ms_);
        clear_series(frame_receive_spans_ms_);
        clear_series(host_processing_ms_);
        clear_series(local_submit_ms_);
        clear_series(control_rtt_ms_);
        clear_series(control_rtt_variance_ms_);
        lock_.unlock();
    }

    void record_frame(PDECODE_UNIT unit, uint32_t local_submit_us) {
        if (unit == nullptr) {
            return;
        }
        lock_.lock();
        ++frames_;
        // receiveTimeUs is the first-packet arrival time; this records frame
        // delivery intervals, not one-way latency or decoded/displayed FPS.
        if (unit->receiveTimeUs != 0 && previous_receive_us_ != 0 &&
            unit->receiveTimeUs > previous_receive_us_) {
            append(frame_intervals_ms_, static_cast<uint16_t>(std::min<uint64_t>(
                65535, (unit->receiveTimeUs - previous_receive_us_ + 500) /
                          1000)));
        }
        if (unit->receiveTimeUs != 0) {
            previous_receive_us_ = unit->receiveTimeUs;
        }
        // The Moonlight depacketizer documents this difference as frame
        // receive/assembly span. It is kept separate from the ENet RTT sample.
        if (unit->enqueueTimeUs >= unit->receiveTimeUs &&
            unit->receiveTimeUs != 0) {
            append(frame_receive_spans_ms_, static_cast<uint16_t>(
                std::min<uint64_t>(65535,
                                   (unit->enqueueTimeUs - unit->receiveTimeUs +
                                    500) /
                                       1000)));
        }
        if (unit->frameHostProcessingLatency != 0) {
            append(host_processing_ms_, static_cast<uint16_t>(
                (unit->frameHostProcessingLatency + 5) / 10));
        }
        append(local_submit_ms_, static_cast<uint16_t>(std::min<uint32_t>(
            65535, (local_submit_us + 999) / 1000)));
        lock_.unlock();
    }

    void record_control_rtt(uint32_t rtt_ms, uint32_t variance_ms) {
        lock_.lock();
        append(control_rtt_ms_, static_cast<uint16_t>(
                                    std::min<uint32_t>(65535, rtt_ms)));
        append(control_rtt_variance_ms_, static_cast<uint16_t>(
                                             std::min<uint32_t>(65535,
                                                                variance_ms)));
        lock_.unlock();
    }

    ConnectionTestMetricsSnapshot snapshot() {
        ConnectionTestMetricsSnapshot result;
        lock_.lock();
        result.frames = frames_;
        copy_series(frame_intervals_ms_, &result.frame_intervals_ms);
        copy_series(frame_receive_spans_ms_, &result.frame_receive_spans_ms);
        copy_series(host_processing_ms_, &result.host_processing_ms);
        copy_series(local_submit_ms_, &result.local_submit_ms);
        copy_series(control_rtt_ms_, &result.control_rtt_ms);
        copy_series(control_rtt_variance_ms_,
                    &result.control_rtt_variance_ms);
        lock_.unlock();
        return result;
    }

  private:
    struct Series {
        uint16_t values[CONNECTION_TEST_MAX_SAMPLES] = {};
        size_t count = 0;
        size_t next = 0;
    };

    ThreadLock lock_;
    unsigned int frames_ = 0;
    uint64_t previous_receive_us_ = 0;
    Series frame_intervals_ms_;
    Series frame_receive_spans_ms_;
    Series host_processing_ms_;
    Series local_submit_ms_;
    Series control_rtt_ms_;
    Series control_rtt_variance_ms_;

    static void clear_series(Series &series) {
        series.count = 0;
        series.next = 0;
    }
    static void append(Series &series, uint16_t value) {
        series.values[series.next] = value;
        series.next = (series.next + 1) % CONNECTION_TEST_MAX_SAMPLES;
        if (series.count < CONNECTION_TEST_MAX_SAMPLES) {
            ++series.count;
        }
    }
    static void copy_series(const Series &series,
                            std::vector<uint16_t> *destination) {
        destination->clear();
        destination->reserve(series.count);
        const size_t first =
            (series.next + CONNECTION_TEST_MAX_SAMPLES - series.count) %
            CONNECTION_TEST_MAX_SAMPLES;
        for (size_t index = 0; index < series.count; ++index) {
            destination->push_back(
                series.values[(first + index) % CONNECTION_TEST_MAX_SAMPLES]);
        }
    }
};

struct ConnectionTestSession {
    AtomicVar<bool> terminated{false};
    AtomicVar<int> termination_error{0};
    AtomicVar<bool> poor_status_seen{false};
};

static ConnectionTestSession *active_connection_test_session = nullptr;

static void connection_test_terminated(int error_code) {
    if (active_connection_test_session != nullptr) {
        active_connection_test_session->termination_error.store(error_code);
        active_connection_test_session->terminated.store(true);
    }
    N3dsConnectionListener *listener = N3dsConnectionListener::get_instance();
    if (listener != nullptr) {
        listener->connection_terminated(error_code, false);
    }
}

static void connection_test_status_update(int status) {
    if (status == CONN_STATUS_POOR &&
        active_connection_test_session != nullptr) {
        active_connection_test_session->poor_status_seen.store(true);
    }
    if (n3ds_connection_callbacks.connectionStatusUpdate != nullptr) {
        n3ds_connection_callbacks.connectionStatusUpdate(status);
    }
}

static int connection_test_submit_decode_unit(
    PDECODER_RENDERER_CALLBACKS delegate, ConnectionTestMetrics *metrics,
    PDECODE_UNIT unit) {
    if (delegate == nullptr || delegate->submitDecodeUnit == nullptr) {
        return DR_OK;
    }
    const u64 started = svcGetSystemTick();
    const int status = delegate->submitDecodeUnit(unit);
    const u64 elapsed_ticks = svcGetSystemTick() - started;
    const uint32_t elapsed_us = static_cast<uint32_t>(std::min<uint64_t>(
        UINT32_MAX, elapsed_ticks * 1000000ULL / SYSCLOCK_ARM11));
    metrics->record_frame(unit, elapsed_us);
    return status;
}

static PDECODER_RENDERER_CALLBACKS connection_test_delegate = nullptr;
static ConnectionTestMetrics *connection_test_active_metrics = nullptr;

static int connection_test_video_setup(int video_format, int width, int height,
                                       int redraw_rate, void *context,
                                       int dr_flags) {
    if (connection_test_delegate == nullptr ||
        connection_test_delegate->setup == nullptr) {
        return -1;
    }
    return connection_test_delegate->setup(video_format, width, height,
                                           redraw_rate, context, dr_flags);
}

static void connection_test_video_start() {
    if (connection_test_delegate != nullptr &&
        connection_test_delegate->start != nullptr) {
        connection_test_delegate->start();
    }
}

static void connection_test_video_stop() {
    if (connection_test_delegate != nullptr &&
        connection_test_delegate->stop != nullptr) {
        connection_test_delegate->stop();
    }
}

static void connection_test_video_cleanup() {
    if (connection_test_delegate != nullptr &&
        connection_test_delegate->cleanup != nullptr) {
        connection_test_delegate->cleanup();
    }
}

static int connection_test_video_submit(PDECODE_UNIT unit) {
    return connection_test_submit_decode_unit(
        connection_test_delegate, connection_test_active_metrics, unit);
}

static DECODER_RENDERER_CALLBACKS connection_test_video_callbacks = {};

static PDECODER_RENDERER_CALLBACKS wrap_connection_test_video_callbacks(
    PDECODER_RENDERER_CALLBACKS delegate, ConnectionTestMetrics *metrics) {
    connection_test_delegate = delegate;
    connection_test_active_metrics = metrics;
    connection_test_video_callbacks.setup = connection_test_video_setup;
    connection_test_video_callbacks.start = connection_test_video_start;
    connection_test_video_callbacks.stop = connection_test_video_stop;
    connection_test_video_callbacks.cleanup = connection_test_video_cleanup;
    connection_test_video_callbacks.submitDecodeUnit =
        connection_test_video_submit;
    connection_test_video_callbacks.capabilities = delegate->capabilities;
    return &connection_test_video_callbacks;
}

static uint16_t percentile_ms(std::vector<uint16_t> samples,
                              unsigned int percentile) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const size_t index = std::min(
        samples.size() - 1,
        (samples.size() * percentile + 99) / 100 - 1);
    return samples[index];
}

struct ConnectionTestRung {
    int fps = 0;
    int bitrate = 0;
    bool attempted = false;
    bool skipped = false;
    bool connected = false;
    bool stable = false;
    bool cancelled = false;
    bool stream_ended_early = false;
    int connection_error = 0;
    unsigned int duration_ms = 0;
    unsigned int submitted_units_per_second = 0;
    bool cleanup_failed = false;
    bool poor_status = false;
    bool local_work_over_budget = false;
    bool delivery_timing_over_budget = false;
    unsigned int frames = 0;
    uint16_t p95_interval_ms = 0;
    uint16_t p95_receive_span_ms = 0;
    uint16_t p95_host_processing_ms = 0;
    uint16_t p95_local_submit_ms = 0;
    uint16_t median_control_rtt_ms = 0;
    uint16_t p95_control_rtt_variance_ms = 0;
    std::vector<uint16_t> frame_intervals_ms;
};

static void finish_connection_test_rung(ConnectionTestRung *rung,
                                        ConnectionTestMetrics *metrics,
                                        unsigned int duration_ms) {
    rung->duration_ms = duration_ms;
    const ConnectionTestMetricsSnapshot snapshot = metrics->snapshot();
    rung->frames = snapshot.frames;
    if (duration_ms > 0) {
        rung->submitted_units_per_second =
            static_cast<unsigned int>(snapshot.frames * 1000ULL / duration_ms);
    }
    rung->p95_interval_ms =
        percentile_ms(snapshot.frame_intervals_ms, 95);
    rung->p95_receive_span_ms =
        percentile_ms(snapshot.frame_receive_spans_ms, 95);
    rung->p95_host_processing_ms =
        percentile_ms(snapshot.host_processing_ms, 95);
    rung->p95_local_submit_ms = percentile_ms(snapshot.local_submit_ms, 95);
    rung->median_control_rtt_ms =
        percentile_ms(snapshot.control_rtt_ms, 50);
    rung->p95_control_rtt_variance_ms =
        percentile_ms(snapshot.control_rtt_variance_ms, 95);
    rung->frame_intervals_ms = snapshot.frame_intervals_ms;

    const unsigned int expected_frames =
        static_cast<unsigned int>(rung->fps) * duration_ms / 1000;
    const uint16_t frame_period_ms =
        static_cast<uint16_t>((1000 + rung->fps - 1) / rung->fps);
    const bool adequate_frame_count =
        rung->frames >= (expected_frames * 85) / 100;
    const bool adequate_timing =
        rung->p95_interval_ms <= (frame_period_ms * 3) / 2;
    const bool local_work_in_budget =
        rung->p95_local_submit_ms <= frame_period_ms;
    rung->local_work_over_budget = !local_work_in_budget;
    rung->delivery_timing_over_budget =
        !adequate_frame_count || !adequate_timing || rung->poor_status;
    rung->stable = rung->connected && !rung->stream_ended_early &&
                   !rung->poor_status &&
                   duration_ms >= 7000 &&
                   adequate_frame_count &&
                   adequate_timing && local_work_in_budget;
}

class ConnectionTestStreamScreen : public MoonlightUi::ScreenModel {
  public:
    ConnectionTestStreamScreen(ConnectionTestMetrics *metrics,
                               ConnectionTestSession *session,
                               const std::string &app_name, int width,
                               int height, int fps, int bitrate)
        : metrics_(metrics), session_(session), app_name_(app_name),
          width_(width), height_(height), fps_(fps), bitrate_(bitrate),
          started_tick_(svcGetSystemTick()) {}

    void draw(MoonlightUi::Screen &screen) override {
        const ConnectionTestMetricsSnapshot snapshot = metrics_->snapshot();
        screen.drawHeader("TEST STREAM", std::to_string(width_) + "x" +
                                              std::to_string(height_) + "  " +
                                              std::to_string(fps_) + " FPS");
        screen.drawLiveText(18, 66, app_name_, MoonlightUi::TextStyle::Small,
                            {105, 114, 122});
        const unsigned int elapsed = elapsed_ms();
        const unsigned int percent = std::min(
            100U, elapsed * 100U / CONNECTION_TEST_RUNG_MS);
        screen.drawProgressBar({18, 82, 302, 106}, percent,
                               {63, 190, 205});
        screen.drawLiveText(18, 114,
                            std::to_string(bitrate_) + " kbps  " +
                                std::to_string(snapshot.frames) + " frames",
                            MoonlightUi::TextStyle::Small, {38, 43, 48});
        screen.drawFrameTimingGraph({20, 140, 300, 194},
                                    snapshot.frame_intervals_ms, 60,
                                    {63, 190, 205});
        screen.drawLiveText(18, 199, "FRAME INTERVAL 0-60 ms",
                            MoonlightUi::TextStyle::Small, {105, 114, 122});
        screen.drawFooter(DIAG_BUILD_VERSION, "B STOP TEST");
    }

    bool update(bool *redraw) override {
        ++frames_;
        const u64 now = svcGetSystemTick();
        if (now - last_rtt_tick_ >= SYSCLOCK_ARM11 / 2) {
            uint32_t rtt_ms = 0;
            uint32_t variance_ms = 0;
            if (LiGetEstimatedRttInfo(&rtt_ms, &variance_ms)) {
                metrics_->record_control_rtt(rtt_ms, variance_ms);
            }
            last_rtt_tick_ = now;
        }
        *redraw = frames_ % 15 == 0;
        return !cancelled_ && !session_->terminated.load() &&
               elapsed_ms() < CONNECTION_TEST_RUNG_MS;
    }

    bool handleInput(const MoonlightUi::InputEvent &event,
                     bool *redraw) override {
        *redraw = false;
        if (event.type == MoonlightUi::InputEvent::Type::Back) {
            cancelled_ = true;
            return false;
        }
        return true;
    }

    bool cancelled() const { return cancelled_; }
    unsigned int elapsed_ms() const {
        return static_cast<unsigned int>(
            (svcGetSystemTick() - started_tick_) * 1000ULL / SYSCLOCK_ARM11);
    }

  private:
    ConnectionTestMetrics *metrics_;
    ConnectionTestSession *session_;
    std::string app_name_;
    int width_;
    int height_;
    int fps_;
    int bitrate_;
    u64 started_tick_;
    u64 last_rtt_tick_ = 0;
    unsigned int frames_ = 0;
    bool cancelled_ = false;
};

class ConnectionTestResultsScreen : public MoonlightUi::ScreenModel {
  public:
    enum class Result { Done, Save };

    ConnectionTestResultsScreen(
        const std::string &host_key, const std::string &app_name,
        int resolution_index, const std::vector<ConnectionTestRung> &rungs)
        : host_key_(host_key), app_name_(app_name),
          resolution_index_(resolution_index), rungs_(rungs),
          has_existing_(load_connection_test_profile(
              host_key, resolution_index, &existing_)) {}

    void draw(MoonlightUi::Screen &screen) override {
        const int fps = page_ == 0 ? 30 : 60;
        const ConnectionTestRung *graph_rung = best_rung(fps);
        screen.drawHeader("CONNECTION TEST RESULTS",
                          std::to_string(STREAM_PROFILE_WIDTHS[
                              resolution_index_]) +
                              "x" +
                              std::to_string(STREAM_PROFILE_HEIGHTS[
                                  resolution_index_]) +
                              "  /  " + std::to_string(fps) + " FPS");
        screen.drawLiveText(
            18, 62,
            fit_connection_test_text(screen, app_name_, 280,
                                     MoonlightUi::TextStyle::Small),
            MoonlightUi::TextStyle::Small, {105, 114, 122});
        if (graph_rung != nullptr &&
            !graph_rung->frame_intervals_ms.empty()) {
            const uint16_t scale = static_cast<uint16_t>(std::min(
                65535, 60));
            screen.drawFrameTimingGraph({20, 78, 300, 123},
                                        graph_rung->frame_intervals_ms, scale,
                                        {63, 190, 205});
            screen.drawLiveText(18, 126, "FRAME INTERVAL 0-60 ms",
                                MoonlightUi::TextStyle::Small,
                                {105, 114, 122});
            const std::string control_rtt =
                graph_rung->median_control_rtt_ms == 0
                    ? "CONTROL RTT unavailable"
                    : "CONTROL RTT " +
                          std::to_string(
                              graph_rung->median_control_rtt_ms) +
                          " +/- " + std::to_string(
                                        graph_rung->p95_control_rtt_variance_ms) +
                          " ms";
            screen.drawLiveText(155, 126, control_rtt,
                                MoonlightUi::TextStyle::Small,
                                {105, 114, 122});
        } else {
            screen.drawLiveText(18, 95, "No frame timing samples",
                                MoonlightUi::TextStyle::Small,
                                {105, 114, 122});
        }

        int row = 0;
        for (const ConnectionTestRung &rung : rungs_) {
            if (rung.fps != fps) {
                continue;
            }
            const std::string status =
                rung.skipped ? "SKIP"
                             : rung.cleanup_failed
                                   ? "CLEANUP"
                                   : rung.cancelled
                                         ? "STOP"
                                         : rung.poor_status
                                               ? "POOR"
                                               : rung.stable
                                                     ? "STABLE"
                                                     : !rung.connected
                                                           ? "FAIL"
                                                           : rung.local_work_over_budget &&
                                                                     rung.delivery_timing_over_budget
                                                                 ? "MIXED"
                                                                 : rung.local_work_over_budget
                                                                       ? "LOCAL"
                                                                       : rung.delivery_timing_over_budget
                                                                             ? "DELIVERY"
                                                                             : "LIMIT";
            std::string detail = std::to_string(rung.bitrate) + " " + status;
            if (rung.attempted) {
                detail += " " +
                          std::to_string(rung.submitted_units_per_second) +
                          " sub/s int " +
                          std::to_string(rung.p95_interval_ms) + " local " +
                          std::to_string(rung.p95_local_submit_ms) +
                          " rxspan " +
                          std::to_string(rung.p95_receive_span_ms) + " ms";
                if (rung.p95_host_processing_ms > 0) {
                    detail += " host " +
                              std::to_string(rung.p95_host_processing_ms);
                }
            }
            screen.drawLiveText(18, 150 + row * 15, detail,
                                MoonlightUi::TextStyle::Small,
                                rung.stable ? MoonlightUi::Color{0, 137, 153}
                                            : MoonlightUi::Color{105, 114, 122});
            ++row;
            if (row == 3) {
                break;
            }
        }

        const int fps30 = recommendation(30);
        const int fps60 = recommendation(60);
        std::string recommendation_line =
            "30: " + (fps30 > 0 ? std::to_string(fps30) : "none") +
            "  /  60: " + (fps60 > 0 ? std::to_string(fps60) : "none") +
            " kbps";
        screen.drawLiveText(18, 193, recommendation_line,
                            MoonlightUi::TextStyle::Small,
                            {38, 43, 48});
        if (save_failed_) {
            screen.drawLiveText(18, 207, "Save failed; check SD storage",
                                MoonlightUi::TextStyle::Small,
                                {176, 45, 45});
        } else if (saved_) {
            screen.drawLiveText(18, 207, "Saved for this host and bracket",
                                MoonlightUi::TextStyle::Small,
                                {0, 137, 153});
        } else if (has_existing_) {
            screen.drawLiveText(
                18, 207,
                "Saved: " + std::to_string(existing_.fps30_bitrate) + "/" +
                    std::to_string(existing_.fps60_bitrate) +
                    " kbps. A replaces these.",
                MoonlightUi::TextStyle::Small, {135, 91, 0});
        }
        screen.drawFooter(
            DIAG_BUILD_VERSION,
            can_save() ? "LEFT/RIGHT FPS  A SAVE  B DONE"
                       : "LEFT/RIGHT FPS  B DONE");
    }

    bool handleInput(const MoonlightUi::InputEvent &event,
                     bool *redraw) override {
        *redraw = false;
        if (event.type == MoonlightUi::InputEvent::Type::Back) {
            result_ = Result::Done;
            return false;
        }
        if (event.type == MoonlightUi::InputEvent::Type::Confirm) {
            if (can_save()) {
                result_ = Result::Save;
                return false;
            }
            return true;
        }
        if (event.type == MoonlightUi::InputEvent::Type::Direction &&
            (event.direction == MoonlightUi::Direction::Left ||
             event.direction == MoonlightUi::Direction::Right)) {
            const int next_page = page_ == 0 ? 1 : 0;
            *redraw = next_page != page_;
            page_ = next_page;
        }
        return true;
    }

    Result result() const { return result_; }
    int fps30_recommendation() const { return recommendation(30); }
    int fps60_recommendation() const { return recommendation(60); }
    void save_failed() {
        save_failed_ = true;
        result_ = Result::Done;
    }
    void saved() {
        saved_ = true;
        result_ = Result::Done;
    }

  private:
    std::string host_key_;
    std::string app_name_;
    int resolution_index_;
    const std::vector<ConnectionTestRung> &rungs_;
    int page_ = 0;
    Result result_ = Result::Done;
    ConnectionTestProfile existing_{};
    bool has_existing_ = false;
    bool save_failed_ = false;
    bool saved_ = false;

    int recommendation(int fps) const {
        int bitrate = 0;
        for (const ConnectionTestRung &rung : rungs_) {
            if (rung.fps == fps && rung.stable) {
                bitrate = std::max(bitrate, rung.bitrate);
            }
        }
        return bitrate;
    }
    bool can_save() const {
        return recommendation(30) > 0 && recommendation(60) > 0 &&
               !has_cleanup_failure();
    }
    bool has_cleanup_failure() const {
        for (const ConnectionTestRung &rung : rungs_) {
            if (rung.cleanup_failed) {
                return true;
            }
        }
        return false;
    }
    const ConnectionTestRung *best_rung(int fps) const {
        const ConnectionTestRung *best_stable = nullptr;
        const ConnectionTestRung *best_with_samples = nullptr;
        for (const ConnectionTestRung &rung : rungs_) {
            if (rung.fps != fps || !rung.attempted) {
                continue;
            }
            if (!rung.frame_intervals_ms.empty() &&
                (best_with_samples == nullptr ||
                 rung.bitrate > best_with_samples->bitrate)) {
                best_with_samples = &rung;
            }
            if (rung.stable &&
                (best_stable == nullptr || rung.bitrate > best_stable->bitrate)) {
                best_stable = &rung;
            }
        }
        return best_stable != nullptr ? best_stable : best_with_samples;
    }
};

static inline void dispatch_loop(void *_unused_) {
    auto pDispatcher = MessageDispatcher::get_instance();
    auto connection_listener = N3dsConnectionListener::get_instance();
    while (!connection_listener->is_connection_closed()) {
        gspWaitForAnyEvent();
        pDispatcher->dispatch_all();
    }
}

static inline void input_loop(void *input_handler_in) {
    N3dsInput *input_handler = static_cast<N3dsInput *>(input_handler_in);
    auto connection_listener = N3dsConnectionListener::get_instance();
    diag_ui_state("BOTTOM", "stream companion panel", true);
    while (!connection_listener->is_connection_closed()) {
        gspWaitForAnyEvent();
        input_handler->n3dsinput_handle_event();
    }
}

static void draw_stream_gamepad_panel(uint32_t rtt_ms, bool rtt_available,
                                     const std::vector<uint16_t> &history,
                                     unsigned int session_seconds,
                                     int battery_percent,
                                     int touch_buttons,
                                     int left_touch_x, int left_touch_y,
                                     int right_touch_x, int right_touch_y,
                                     bool click_mode,
                                     int toolbar_page, int touch_zone,
                                     bool quit_armed,
                                     int display_count, int default_display,
                                     const N3ds3DSettings &stereo_settings) {
    static MoonlightUi::Screen bottom(GFX_BOTTOM);
    using MoonlightUi::Color;
    using MoonlightUi::TextStyle;
    const Color ink{231, 239, 244};
    const Color muted{151, 166, 178};
    const Color teal{64, 205, 214};
    const Color dark{17, 25, 34};
    bottom.drawSolidBackground(dark);

    char status[64];
    if (rtt_available)
        snprintf(status, sizeof(status), "RTT  %lu ms", (unsigned long)rtt_ms);
    else
        snprintf(status, sizeof(status), "RTT  unavailable");
    bottom.drawLiveText(8, 9, status, TextStyle::Small, teal);
    bottom.drawFrameTimingGraph({67, 8, 108, 26}, history, 200, teal);
    char battery[32];
    if (battery_percent >= 0)
        snprintf(battery, sizeof(battery), "BATTERY  %d%%", battery_percent);
    else
        snprintf(battery, sizeof(battery), "BATTERY  unavailable");
    bottom.drawLiveText(116, 9, battery, TextStyle::Small, muted);
    char session[32];
    snprintf(session, sizeof(session), "SESSION  %02u:%02u",
             session_seconds / 3600, (session_seconds / 60) % 60);
    bottom.drawLiveText(211, 9, session, TextStyle::Small, muted);
    bottom.drawLiveText(257, 25, DIAG_BUILD_VERSION, TextStyle::Small, muted);
    bottom.drawDisplaySelector(default_display, display_count, false);

    const Color mint{78, 232, 174};
    const Color ring{61, 202, 178};
    const Color ring_shadow{23, 75, 75};
    if (toolbar_page == 3) {
        char shift[32];
        snprintf(shift, sizeof(shift), "SHIFT  %+d", stereo_settings.shift);
        bottom.drawLiveText(24, 78, "3D PRESENTATION", TextStyle::Header, teal);
        bottom.drawPanelButton({24, 99, 296, 126},
                               n3ds_3d_mode_name(stereo_settings.mode),
                               {35, 48, 60}, ink);
        bottom.drawPanelButton({24, 132, 120, 159},
                               n3ds_3d_aspect_name(stereo_settings.aspect),
                               {35, 48, 60}, ink);
        bottom.drawPanelButton({124, 132, 210, 159}, shift,
                               {35, 48, 60}, ink);
        bottom.drawPanelButton({214, 132, 296, 159},
                               stereo_settings.prefilter ? "PF ON" : "PF OFF",
                               stereo_settings.prefilter ? Color{29, 112, 82}
                                                         : Color{35, 48, 60},
                               ink);
        bottom.drawLiveText(26, 164, "3D OUTPUT REQUIRES SLIDER UP",
                            TextStyle::Small, muted);
    } else {
        const int left_center_x = 78;
        const int right_center_x = 242;
        const int stick_center_y = 137;
        const int left_knob_x = left_center_x + (left_touch_x * 22 / 32767);
        const int left_knob_y = stick_center_y - (left_touch_y * 22 / 32767);
        const int right_knob_x = right_center_x + (right_touch_x * 22 / 32767);
        const int right_knob_y = stick_center_y - (right_touch_y * 22 / 32767);
        for (const int center_x : {left_center_x, right_center_x}) {
            bottom.drawCircle(center_x, stick_center_y, 43, ring_shadow);
            bottom.drawCircle(center_x, stick_center_y, 40, dark);
            bottom.drawCircleOutline(center_x, stick_center_y, 42, 2, ring);
        }
        if (touch_zone == 1)
            bottom.drawCircleOutline(left_center_x, stick_center_y, 43, 3, mint);
        if (touch_zone == 2)
            bottom.drawCircleOutline(right_center_x, stick_center_y, 43, 3, mint);
        if (touch_buttons & LS_CLK_FLAG)
            bottom.drawCircleOutline(left_center_x, stick_center_y, 43, 4, mint);
        if (touch_buttons & RS_CLK_FLAG)
            bottom.drawCircleOutline(right_center_x, stick_center_y, 43, 4, mint);
        const auto draw_stick = [&](int knob_x, int knob_y, const char *label) {
            bottom.drawCircle(knob_x, knob_y, 21, {45, 58, 67});
            bottom.drawCircle(knob_x, knob_y, 18, {25, 34, 42});
            bottom.drawCircleOutline(knob_x, knob_y, 20, 1, ring);
            const int label_width = bottom.measureLiveText(label, TextStyle::Header);
            bottom.drawLiveText(knob_x - label_width / 2, knob_y - 7, label,
                                TextStyle::Header, mint);
        };
        draw_stick(left_knob_x, left_knob_y, "L3");
        draw_stick(right_knob_x, right_knob_y, "R3");

        const Color mode_on{29, 176, 98};
        const Color mode_off{55, 67, 75};
        bottom.drawPanelButton({132, 83, 188, 103}, "MOVE",
                               click_mode ? mode_off : mode_on, ink);
        bottom.drawSwapIcon(160, 112, ring);
        bottom.drawPanelButton({132, 121, 188, 141}, "CLICK",
                               click_mode ? mode_on : mode_off, ink);
    }
    const Color button_ink{231, 241, 246};
    const Color button_fill{35, 48, 60};
    const Color home_fill{21, 85, 92};
    char page_status[16];
    snprintf(page_status, sizeof(page_status), "%d/4", toolbar_page + 1);
    bottom.drawLiveText(151, 165, page_status, TextStyle::Small, muted);
    bottom.drawPanelButton({8, 184, 42, 228}, "", button_fill, button_ink);
    bottom.drawPanelButton({46, 184, 88, 228}, "", button_fill, button_ink);
    bottom.drawPanelButton({90, 184, 132, 228}, "", button_fill, button_ink);
    bottom.drawPanelButton({136, 181, 184, 230}, "", home_fill, button_ink);
    bottom.drawPanelButton({188, 184, 230, 228}, "", button_fill, button_ink);
    bottom.drawPanelButton({232, 184, 274, 228}, "", button_fill, button_ink);
    bottom.drawPanelButton({278, 184, 312, 228}, "", button_fill, button_ink);
    bottom.drawArrowIcon(25, 205, true, button_ink);
    bottom.drawHomeIcon(160, 198, {64, 228, 231});
    bottom.drawLiveText(145, 215, "HOME", TextStyle::Small, button_ink);
    bottom.drawArrowIcon(295, 205, false, button_ink);

    const auto draw_toolbar_item = [&](int slot, const char *label, int icon,
                                       bool warning) {
        const MoonlightUi::Rect rects[] = {
            {46, 184, 88, 228}, {90, 184, 132, 228},
            {188, 184, 230, 228}, {232, 184, 274, 228}};
        const MoonlightUi::Rect &rect = rects[slot];
        const int center_x = (rect.left + rect.right) / 2;
        const int center_y = 199;
        const Color label_color = warning ? Color{255, 184, 164} : button_ink;
        const Color fill = warning ? Color{91, 42, 47} : button_fill;
        bottom.drawPanelButton(rect, "", fill, label_color);
        switch (icon) {
        case 0: bottom.drawMagnifierIcon(center_x, center_y, label_color); break;
        case 1: bottom.drawGamepadIcon(center_x, center_y, label_color); break;
        case 2: bottom.drawMouseIcon(center_x, center_y, label_color); break;
        case 3: bottom.drawKeyboardIcon(center_x, center_y - 1, label_color); break;
        case 4: bottom.drawTouchIcon(center_x, center_y, label_color); break;
        case 5: bottom.drawSettingsIcon(center_x, center_y, label_color); break;
        case 6: bottom.drawDisplayIcon(center_x, center_y - 1, label_color); break;
        case 7: bottom.drawPowerIcon(center_x, center_y, label_color); break;
        case 8: {
            const int label_width =
                bottom.measureLiveText("3D", TextStyle::Header);
            bottom.drawLiveText(center_x - label_width / 2, center_y - 7,
                                "3D", TextStyle::Header, label_color);
            break;
        }
        case 9: bottom.drawArrowIcon(center_x, center_y, true, label_color); break;
        case 10: bottom.drawArrowIcon(center_x, center_y, false, label_color); break;
        default: break;
        }
        const int label_width = bottom.measureLiveText(label, TextStyle::Small);
        bottom.drawLiveText(center_x - label_width / 2, 215, label,
                            TextStyle::Small, label_color);
    };

    if (toolbar_page == 0) {
        draw_toolbar_item(0, "MAG", 0, false);
        draw_toolbar_item(1, "3D", 8, false);
        draw_toolbar_item(2, "GAME", 1, false);
        draw_toolbar_item(3, "KEY", 3, false);
    } else if (toolbar_page == 1) {
        draw_toolbar_item(0, "MOUSE", 2, false);
        draw_toolbar_item(1, "ABS", 4, false);
        draw_toolbar_item(2, "DS", 4, false);
        draw_toolbar_item(3, "MAG", 0, false);
    } else if (toolbar_page == 2) {
        draw_toolbar_item(0, "DEBUG", 5, false);
        draw_toolbar_item(1, "PREV", 6, false);
        draw_toolbar_item(2, "NEXT", 6, false);
        draw_toolbar_item(3, quit_armed ? "AGAIN" : "QUIT", 7,
                          quit_armed);
    } else {
        draw_toolbar_item(0, "MODE", 8, false);
        draw_toolbar_item(1, "ASPECT", 5, false);
        draw_toolbar_item(2, "SHIFT-", 9, false);
        draw_toolbar_item(3, "SHIFT+", 10, false);
    }
    bottom.present(false);
}

struct StreamWorkerThreads {
    Thread input_thread = NULL;
    Thread dispatch_thread = NULL;
};

static inline StreamWorkerThreads stream_loop(PCONFIGURATION config,
                               N3dsConnectionListener *connection_listener,
                               std::shared_ptr<N3dsInput> input_handler) {
    // Spin off worker threads
    StreamWorkerThreads workers;
    size_t stack_size = 0x20000;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    bool panel_active = true;
    bool rtt_available = false;
    bool battery_available = false;
    uint32_t latest_rtt_ms = 0;
    uint8_t battery_level = 0;
    std::vector<uint16_t> rtt_history;
    const u64 session_started = svcGetSystemTick();
    u64 last_panel_update = 0;
    char stream_config_path[] = MOONLIGHT_3DS_PATH "/moonlight.conf";
    bool top_magnified = false;
    n3ds_set_top_magnified(false);
    const bool ptmu_ready = R_SUCCEEDED(ptmuInit());
    MessageDispatcher::get_instance()->post_immediate(
        std::make_shared<TouchStateChangedMsg>(N3dsTouchType::GAMEPAD));
    if (input_handler) input_handler->set_stream_panel_enabled(true);
    diag_log("MONITOR_CONFIG", "configured_count=%d selected=%d",
             config->sunshine_display_count, config->sunshine_default_display);
    if (!config->viewonly) {
        workers.input_thread = threadCreate(input_loop, input_handler.get(),
                                            stack_size, priority, -1, false);
    }
    workers.dispatch_thread =
        threadCreate(dispatch_loop, nullptr, stack_size, priority, -1, false);

    // Run the main connection loop
    while (!connection_listener->is_connection_closed() && aptMainLoop()) {
        gspWaitForAnyEvent();
        if (aptShouldClose()) {
            connection_listener->connection_terminated(0);
        }
        if (input_handler && input_handler->quit_to_menu_requested())
            connection_listener->connection_terminated(ML_ERROR_GRACEFUL_TERMINATION);
        if (input_handler && input_handler->consume_stream_panel_keyboard_request()) {
            input_handler->open_stream_keyboard();
            panel_active = false;
        }
        if (input_handler && input_handler->consume_stream_panel_settings_request())
        {
            input_handler->open_stream_mode_menu();
            panel_active = false;
        }
        if (input_handler) {
            const int requested_mode =
                input_handler->consume_stream_panel_mode_request();
            if (requested_mode >= 0) {
                const N3dsTouchType mode =
                    static_cast<N3dsTouchType>(requested_mode);
                input_handler->open_stream_touch_mode(mode);
                panel_active = mode == N3dsTouchType::GAMEPAD;
            }
        }
        if (input_handler && input_handler->consume_stream_panel_restore_request()) {
            input_handler->set_stream_panel_enabled(true);
            panel_active = true;
        }
        if (input_handler) {
            const int display_action = input_handler->consume_stream_display_action();
            if (display_action == 3) {
                top_magnified = !top_magnified;
                n3ds_set_top_magnified(top_magnified);
                diag_log("STREAM_MAGNIFIER", "top_screen=%s",
                         top_magnified ? "on" : "off");
            } else if (display_action != 0 &&
                       config->sunshine_display_count > 1) {
                int selected = config->sunshine_default_display + display_action;
                if (selected < 1) selected = 1;
                if (selected > config->sunshine_display_count)
                    selected = config->sunshine_display_count;
                if (selected != config->sunshine_default_display) {
                    config->sunshine_default_display = selected;
                    config_save(stream_config_path, config);
                    diag_log("MONITOR_REQUEST",
                             "requested_display=%d configured_count=%d",
                             selected, config->sunshine_display_count);
                    const int key = 0x70 + selected - 1; // VK_F1 ... VK_F12
                    const char modifiers =
                        MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT;
                    const int down_status = LiSendKeyboardEvent(
                        key, KEY_ACTION_DOWN, modifiers);
                    diag_log("MONITOR_SHORTCUT",
                             "key=F%d action=down modifiers=ctrl+alt+shift send_result=%d",
                             selected, down_status);
                    const int up_status = LiSendKeyboardEvent(
                        key, KEY_ACTION_UP, modifiers);
                    diag_log("MONITOR_SHORTCUT",
                             "key=F%d action=up modifiers=ctrl+alt+shift send_result=%d",
                             selected, up_status);
                    diag_log("MONITOR_SELECTED",
                             "requested_display_index=%d configured_display_count=%d",
                             selected, config->sunshine_display_count);
                }
            }
            if (display_action != 0)
                diag_log("MONITOR_SELECTION_STATE",
                         "tap_action=%d selected_display_index=%d configured_display_count=%d",
                         display_action, config->sunshine_default_display,
                         config->sunshine_display_count);
        }
        const u64 now = svcGetSystemTick();
        const bool metric_update =
            now - last_panel_update >= SYSCLOCK_ARM11 / 2;
        const bool input_redraw =
            input_handler && input_handler->consume_stream_panel_redraw_request();
        if (panel_active && (metric_update || input_redraw)) {
            if (metric_update) {
                uint32_t rtt_variance = 0;
                rtt_available =
                    LiGetEstimatedRttInfo(&latest_rtt_ms, &rtt_variance);
                if (rtt_available) {
                    rtt_history.push_back(static_cast<uint16_t>(
                        std::min<uint32_t>(latest_rtt_ms, 65535)));
                    if (rtt_history.size() > 110)
                        rtt_history.erase(rtt_history.begin());
                }
                battery_available =
                    ptmu_ready &&
                    R_SUCCEEDED(PTMU_GetBatteryLevel(&battery_level));
                last_panel_update = now;
            }
            const unsigned int session_seconds = static_cast<unsigned int>(
                (now - session_started) / SYSCLOCK_ARM11);
            draw_stream_gamepad_panel(
                latest_rtt_ms, rtt_available, rtt_history, session_seconds,
                battery_available ? static_cast<int>(battery_level) * 20 : -1,
                input_handler ? input_handler->stream_panel_touch_buttons() : 0,
                input_handler ? input_handler->stream_panel_touch_x() : 0,
                input_handler ? input_handler->stream_panel_touch_y() : 0,
                input_handler ? input_handler->stream_panel_right_touch_x() : 0,
                input_handler ? input_handler->stream_panel_right_touch_y() : 0,
                input_handler ? input_handler->stream_panel_click_mode() : false,
                input_handler ? input_handler->stream_panel_toolbar_page() : 0,
                input_handler ? input_handler->stream_panel_touch_zone() : 0,
                input_handler ? input_handler->stream_panel_quit_armed() : false,
                config->sunshine_display_count,
                config->sunshine_default_display,
                n3ds_3d_settings_snapshot());
        }
    }
    if (input_handler) input_handler->set_stream_panel_enabled(false);
    n3ds_set_top_magnified(false);
    if (ptmu_ready) ptmuExit();
    return workers;
}

static void join_stream_worker(Thread &thread) {
    if (thread == NULL) return;
    threadJoin(thread, 0xFFFFFFFFFFFFFFFFULL);
    threadFree(thread);
    thread = NULL;
}

struct LimelightStartCall {
    PSERVER_DATA server;
    PCONFIGURATION config;
    PCONNECTION_LISTENER_CALLBACKS listener_callbacks;
    PDECODER_RENDERER_CALLBACKS video_callbacks;
    AUDIO_RENDERER_CALLBACKS *audio_callbacks;
    int status;
};

static void limelight_start_call(void *opaque) {
    LimelightStartCall *call = static_cast<LimelightStartCall *>(opaque);
    diag_video_counters_reset();
    call->status = LiStartConnection(
        &call->server->serverInfo, &call->config->stream,
        call->listener_callbacks, call->video_callbacks,
        call->audio_callbacks, NULL, DISPLAY_FULLSCREEN,
        call->config->audio_device, 0);
}

static void stream(PSERVER_DATA server, PCONFIGURATION config, int appId,
                   const std::string &display_id,
                   std::shared_ptr<N3dsInput> input_handler) {
    int gamepad_mask = 1;
    diag_log("STREAM_START_REQUESTED", "app_id=%d", appId);
    struct StartAppCall {
        PSERVER_DATA server;
        PCONFIGURATION config;
        int app_id;
        int gamepad_mask;
        const char *display_id;
        int status;
    } start_call{server, config, appId, gamepad_mask,
                 display_id.empty() ? NULL : display_id.c_str(), GS_ERROR};
    auto start_app_call = [](void *opaque) {
        StartAppCall *call = static_cast<StartAppCall *>(opaque);
        call->status = gs_start_app(call->server, &call->config->stream,
                                    call->app_id, call->config->sops,
                                    call->config->localaudio, call->gamepad_mask,
                                    call->display_id);
    };
    run_gui_call("stream.start_app", "Starting selected app", start_app_call,
                 &start_call, true, false, "Requesting stream startup");
    if (diag_cancel_requested()) {
        diag_log("STREAM_STOP", "app_id=%d reason=watchdog_cancelled", appId);
        return;
    }
    int ret = start_call.status;
    if (ret < 0) {
        diag_log("STREAM_STOP", "app_id=%d reason=start_app_failed result=%d",
                 appId, ret);
        diag_log("START_APP_ERROR", "result=%d detail=%s", ret, gs_error);
        show_gui_message("APP COULD NOT START",
                         "The host did not start the selected app.",
                         "Check host support and stream settings, then retry.");
        return;
    }

    AUDIO_RENDERER_CALLBACKS *audio_callbacks =
        config->localaudio ? &audio_callbacks_mock : &audio_callbacks_n3ds;

    PDECODER_RENDERER_CALLBACKS video_callbacks = &decoder_callbacks_mock;
    switch (config->video_decoder) {
    case (VIDEO_DECODER_TYPE::HARDWARE_VIDEO_DECODER):
        video_callbacks = &decoder_callbacks_n3ds_mvd;
        break;
    case (VIDEO_DECODER_TYPE::SOFTWARE_VIDEO_DECODER):
        video_callbacks = &decoder_callbacks_n3ds;
        break;
    default:
        break;
    }

    diag_log("STREAM_CONFIG",
             "identity=Moonlight_GamePad build=%s config_source=client_request resolution=%dx%d fps=%d bitrate_kbps=%d",
             DIAG_BUILD_VERSION, config->stream.width, config->stream.height,
             config->stream.fps, config->stream.bitrate);

    auto connection_listener =
        N3dsConnectionListener::create_instance(config->motion_controls);
    LimelightStartCall connection_call{server, config,
                                       &n3ds_connection_callbacks,
                                       video_callbacks, audio_callbacks,
                                       GS_ERROR};
    run_gui_call("stream.limelight_start", "Starting stream connection",
                 limelight_start_call, &connection_call, false, true,
                 "Waiting for video and input channels");
    int status = connection_call.status;
    diag_phase_outcome("stream.limelight_start",
                       status == 0 ? "success" : "failed", status);

    if (status != 0) {
        diag_log("STREAM_STOP", "app_id=%d reason=connection_start_failed status=%d",
                 appId, status);
        n3ds_connection_callbacks.connectionTerminated(status);
        show_gui_message("STREAM CONNECTION FAILED",
                         "Moonlight could not establish the stream.",
                         "Return to the host menu and try again.");
        return;
    }

    diag_log("STREAM_STARTED", "app_id=%d", appId);
    diag_log("STREAM_BEGIN", "app_id=%d", appId);
    diag_set_phase("stream.active");
    diag_ui_state("TOP", "stream video", true);
    diag_ui_state("BOTTOM", "legacy graphical UI", true);
    StreamWorkerThreads workers =
        stream_loop(config, connection_listener, input_handler);
    join_stream_worker(workers.input_thread);
    join_stream_worker(workers.dispatch_thread);

    diag_set_phase("stream.stopping");
    diag_log("STREAM_END", "app_id=%d reason=connection_loop_ended", appId);
    LiStopConnection();
    diag_phase_outcome("stream.stopping", "complete", GS_OK);
    N3dsConnectionListener::destroy_instance();
    restore_bottom_console();

    if (config->quitappafter) {
        printf("Sending app quit request ...\n");
        gs_quit_app(server);
    }
}

static int init_server(CONFIGURATION *config, SERVER_DATA *server) {
    diag_set_phase("connect.server_init");
    gs_cleanup();
    int status = gs_init(server, config->address, config->port, config->key_dir,
                         0, config->unsupported);
    if (status != GS_OK) {
        diag_phase_outcome("connect.server_init", "failed", status);
        return status;
    }

    diag_phase_outcome("connect.server_init", "success", status);
    return GS_OK;
}

struct InitServerCall {
    CONFIGURATION *config;
    SERVER_DATA *server;
    int status;
};

static void init_server_call(void *opaque) {
    InitServerCall *call = static_cast<InitServerCall *>(opaque);
    call->status = init_server(call->config, call->server);
}

static void print_init_server_error(CONFIGURATION *config, int status) {
    if (status == GS_OUT_OF_MEMORY) {
        diag_log("HOST_INIT_ERROR", "status=%d reason=out_of_memory", status);
    } else if (status == GS_ERROR) {
        diag_log("HOST_INIT_ERROR", "status=%d detail=%s", status, gs_error);
    } else if (status == GS_INVALID) {
        diag_log("HOST_INIT_ERROR", "status=%d invalid_host_response=%s", status, gs_error);
    } else if (status == GS_UNSUPPORTED_VERSION) {
        diag_log("HOST_INIT_ERROR", "status=%d unsupported_version=%s", status, gs_error);
    } else {
        diag_log("HOST_INIT_ERROR", "status=%d connection_failed", status);
    }
}

static const char *action_stream(CONFIGURATION *config, SERVER_DATA *server) {
    diag_log("SESSION_PHASE", "name=host_display_selection");
    std::string display_id;
    if (!prompt_for_display_id(server, &display_id)) {
        return diag_cancel_requested() ? "user_cancelled"
                                       : "display_selection_cancelled";
    }
    int appId = prompt_for_app_id(server);
    if (appId == -1) {
        return diag_cancel_requested() ? "user_cancelled" : "app_list_unavailable";
    }

    config->stream.supportedVideoFormats = VIDEO_FORMAT_H264;

    consoleClear();

    std::shared_ptr<N3dsInput> input_handler = nullptr;
    if (config->viewonly) {
        printf("View-only mode enabled, no input will be sent "
               "to the host computer\n");
    } else {
        input_handler = std::make_shared<N3dsInput>(
            config->stream.width, config->stream.height,
            config->swap_face_buttons, config->swap_triggers_and_shoulders,
            config->use_triggers_for_mouse);
    }
    stream(server, config, appId, display_id, input_handler);
    return diag_cancel_requested() ? "stream_cancelled_or_stopped" : "stream_teardown";
}

class ConnectionTestMessageScreen : public MoonlightUi::ScreenModel {
  public:
    ConnectionTestMessageScreen(const std::string &title,
                                const std::string &message)
        : title_(title), message_(message) {}

    void draw(MoonlightUi::Screen &screen) override {
        screen.drawHeader("CONNECTION TEST", title_);
        screen.drawLiveText(18, 101, message_, MoonlightUi::TextStyle::Small,
                            {38, 43, 48});
        screen.drawFooter(DIAG_BUILD_VERSION, "A/B BACK");
    }
    bool handleInput(const MoonlightUi::InputEvent &event,
                     bool *redraw) override {
        *redraw = false;
        return event.type != MoonlightUi::InputEvent::Type::Confirm &&
               event.type != MoonlightUi::InputEvent::Type::Back;
    }

  private:
    std::string title_;
    std::string message_;
};

struct ConnectionTestStartAppCall {
    PSERVER_DATA server;
    PCONFIGURATION config;
    int app_id;
    int status = GS_ERROR;
};

static void connection_test_start_app_task(void *opaque) {
    ConnectionTestStartAppCall *call =
        static_cast<ConnectionTestStartAppCall *>(opaque);
    call->status = gs_start_app(call->server, &call->config->stream,
                                call->app_id, call->config->sops,
                                call->config->localaudio, 1, NULL);
}

struct ConnectionTestQuitAppCall {
    PSERVER_DATA server;
    int status = GS_ERROR;
};

static void connection_test_quit_app_task(void *opaque) {
    ConnectionTestQuitAppCall *call =
        static_cast<ConnectionTestQuitAppCall *>(opaque);
    call->status = gs_quit_app(call->server);
}

static int close_connection_test_app(PSERVER_DATA server) {
    ConnectionTestQuitAppCall call{server, GS_ERROR};
    const bool cancelled = run_gui_call(
        "connection_test.cleanup", "Closing selected app",
        connection_test_quit_app_task, &call, false, false);
    return cancelled ? GS_ERROR : call.status;
}

static PDECODER_RENDERER_CALLBACKS selected_video_callbacks(
    PCONFIGURATION config) {
    switch (config->video_decoder) {
    case VIDEO_DECODER_TYPE::HARDWARE_VIDEO_DECODER:
        return &decoder_callbacks_n3ds_mvd;
    case VIDEO_DECODER_TYPE::SOFTWARE_VIDEO_DECODER:
        return &decoder_callbacks_n3ds;
    default:
        return &decoder_callbacks_mock;
    }
}

static bool run_connection_test_rung(
    CONFIGURATION *base_config, PSERVER_DATA server, int app_id,
    const std::string &app_name, int resolution_index, int fps, int bitrate,
    ConnectionTestRung *rung, ConnectionTestMetrics *metrics) {
    CONFIGURATION test_config = *base_config;
    test_config.stream.width = STREAM_PROFILE_WIDTHS[resolution_index];
    test_config.stream.height = STREAM_PROFILE_HEIGHTS[resolution_index];
    test_config.stream.fps = fps;
    test_config.stream.bitrate = bitrate;
    test_config.stream.supportedVideoFormats = VIDEO_FORMAT_H264;
    rung->fps = fps;
    rung->bitrate = bitrate;
    rung->attempted = true;
    metrics->reset();

    ConnectionTestStartAppCall start_app{server, &test_config, app_id,
                                         GS_ERROR};
    const bool start_cancelled = run_gui_call(
        "connection_test.start_app",
        "Starting " + app_name + " / " + std::to_string(fps) + " FPS",
        connection_test_start_app_task, &start_app);
    if (start_cancelled) {
        rung->cancelled = true;
        rung->cleanup_failed = close_connection_test_app(server) != GS_OK;
        return true;
    }
    if (start_app.status < 0) {
        rung->connection_error = start_app.status;
        return false;
    }

    ConnectionTestSession session;
    active_connection_test_session = &session;
    N3dsConnectionListener::create_instance(false);
    CONNECTION_LISTENER_CALLBACKS test_listener_callbacks =
        n3ds_connection_callbacks;
    test_listener_callbacks.connectionTerminated = connection_test_terminated;
    test_listener_callbacks.connectionStatusUpdate =
        connection_test_status_update;
    AUDIO_RENDERER_CALLBACKS *audio_callbacks =
        test_config.localaudio ? &audio_callbacks_mock : &audio_callbacks_n3ds;
    PDECODER_RENDERER_CALLBACKS video_callbacks =
        wrap_connection_test_video_callbacks(
            selected_video_callbacks(&test_config), metrics);
    LimelightStartCall connection_call{server,
                                       &test_config,
                                       &test_listener_callbacks,
                                       video_callbacks,
                                       audio_callbacks,
                                       GS_ERROR};
    const bool connect_cancelled = run_gui_call(
        "connection_test.connect",
        "Connecting " + std::to_string(fps) + " FPS / " +
            std::to_string(bitrate) + " kbps",
        limelight_start_call, &connection_call, false, true);

    bool cancelled = connect_cancelled;
    N3dsConnectionListener *listener =
        N3dsConnectionListener::get_instance();
    Thread dispatcher = nullptr;
    if (connection_call.status == 0 && !connect_cancelled) {
        rung->connected = true;
        s32 priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        dispatcher = threadCreate(dispatch_loop, nullptr, 0x20000, priority,
                                  -1, false);
        if (dispatcher == NULL) {
            rung->connection_error = -2;
            if (listener != nullptr) {
                listener->connection_terminated(0, false);
            }
        } else {
            ConnectionTestStreamScreen progress(
                metrics, &session, app_name, test_config.stream.width,
                test_config.stream.height, fps, bitrate);
            const bool screen_finished = MoonlightUi::runScreen(progress);
            cancelled = progress.cancelled() || !screen_finished;
            rung->duration_ms = progress.elapsed_ms();
        }
        if (session.terminated.load()) {
            rung->stream_ended_early = true;
            rung->connection_error = session.termination_error.load();
        }
        rung->poor_status = session.poor_status_seen.load();
        if (listener != nullptr && !listener->is_connection_closed()) {
            listener->connection_terminated(0, false);
        }
        LiStopConnection();
    } else {
        rung->connection_error = connection_call.status;
        if (connection_call.status == 0) {
            if (listener != nullptr && !listener->is_connection_closed()) {
                listener->connection_terminated(0, false);
            }
            LiStopConnection();
        }
    }

    if (dispatcher != nullptr) {
        threadJoin(dispatcher, 0xFFFFFFFFFFFFFFFFULL);
        threadFree(dispatcher);
    }
    N3dsConnectionListener::destroy_instance();
    active_connection_test_session = nullptr;
    connection_test_delegate = nullptr;
    connection_test_active_metrics = nullptr;
    if (cancelled) {
        rung->cancelled = true;
    }
    finish_connection_test_rung(rung, metrics, rung->duration_ms);
    if (!rung->connected || rung->duration_ms < CONNECTION_TEST_RUNG_MS) {
        rung->stable = false;
    }
    rung->cleanup_failed = close_connection_test_app(server) != GS_OK;
    return cancelled || rung->cleanup_failed;
}

static void free_connection_test_app_list(PAPP_LIST list) {
    while (list != nullptr) {
        PAPP_LIST next = list->next;
        free(list->name);
        free(list);
        list = next;
    }
}

static void action_connection_test(CONFIGURATION *config, SERVER_DATA *server,
                                   const std::string &host_key) {
    AppListTaskContext app_list_task_context{server, nullptr, GS_ERROR};
    const bool app_list_cancelled = run_gui_call(
        "connection_test.app_list", "Loading host apps", app_list_task,
        &app_list_task_context);
    if (app_list_cancelled) {
        free_connection_test_app_list(app_list_task_context.list);
        return;
    }
    if (app_list_task_context.status != GS_OK) {
        free_connection_test_app_list(app_list_task_context.list);
        ConnectionTestMessageScreen message("APP LIST FAILED",
                                            "Could not load Sunshine apps");
        MoonlightUi::runScreen(message);
        restore_bottom_console();
        return;
    }

    std::vector<ConnectionTestApp> apps;
    PAPP_LIST app = app_list_task_context.list;
    while (app != nullptr) {
        PAPP_LIST next = app->next;
        if (app->name != nullptr && app->name[0] != '\0' && app->id > 0) {
            apps.push_back({app->id, app->name});
            free(app->name);
        }
        free(app);
        app = next;
    }
    if (apps.empty()) {
        ConnectionTestMessageScreen message("NO HOST APPS",
                                            "Sunshine returned no apps");
        MoonlightUi::runScreen(message);
        restore_bottom_console();
        return;
    }

    ConnectionTestAppPickerScreen app_picker(apps);
    if (!MoonlightUi::runScreen(app_picker) ||
        app_picker.result() != ConnectionTestAppPickerScreen::Result::Selected) {
        restore_bottom_console();
        return;
    }
    const ConnectionTestApp selected_app = app_picker.selected_app();
    restore_bottom_console();

    ConnectionTestQualityScreen quality_picker;
    if (!MoonlightUi::runScreen(quality_picker) ||
        quality_picker.result() != ConnectionTestQualityScreen::Result::Selected) {
        restore_bottom_console();
        return;
    }
    const int resolution_index = quality_picker.resolution_index();
    restore_bottom_console();

    std::vector<ConnectionTestRung> rungs;
    ConnectionTestMetrics metrics;
    bool cancelled = false;
    for (int fps_index = 0; fps_index < 2 && !cancelled; ++fps_index) {
        const int fps = fps_index == 0 ? 30 : 60;
        bool skip_higher = false;
        for (int bitrate_index = 0; bitrate_index < 3 && !cancelled;
             ++bitrate_index) {
            ConnectionTestRung rung;
            rung.fps = fps;
            rung.bitrate =
                CONNECTION_TEST_BITRATES[resolution_index][bitrate_index];
            if (skip_higher) {
                rung.skipped = true;
                rungs.push_back(rung);
                continue;
            }
            cancelled = run_connection_test_rung(
                config, server, selected_app.id, selected_app.name,
                resolution_index, fps, rung.bitrate, &rung, &metrics);
            if (!rung.cancelled && !rung.stable) {
                skip_higher = true;
            }
            rungs.push_back(rung);
        }
    }

    ConnectionTestResultsScreen results(host_key, selected_app.name,
                                        resolution_index, rungs);
    while (aptMainLoop()) {
        const bool screen_finished = MoonlightUi::runScreen(results);
        if (!screen_finished ||
            results.result() != ConnectionTestResultsScreen::Result::Save) {
            break;
        }
        ConnectionTestProfile profile{
            resolution_index, results.fps30_recommendation(),
            results.fps60_recommendation()};
        if (save_connection_test_profile(host_key, profile)) {
            results.saved();
        } else {
            results.save_failed();
        }
        restore_bottom_console();
    }
    restore_bottom_console();
}

struct PairTaskContext {
    SERVER_DATA *server;
    char *pin;
    int status;
};

static void pair_task(void *opaque) {
    PairTaskContext *task = static_cast<PairTaskContext *>(opaque);
    task->status = gs_pair(task->server, task->pin);
}

static bool action_pair(CONFIGURATION *config, SERVER_DATA *server) {
    // Extend the timeout to 5 minutes for pairing
    http_set_timeout_s(5 * 60);

    char pin[5];
    sprintf(pin, "%d%d%d%d", (unsigned)random() % 10, (unsigned)random() % 10,
            (unsigned)random() % 10, (unsigned)random() % 10);
    PairTaskContext task{server, &pin[0], GS_ERROR};
    run_pairing_gui_call("pair", pair_task, &task, pin,
                         pairing_layout_v2_preferred());
    bool cancelled = diag_cancel_requested();
    if (cancelled) {
        diag_log("PAIR_CANCELLED", "returning to host list after watchdog cancellation");
    } else if (task.status != GS_OK) {
        diag_log("PAIR_ERROR", "status=%d detail=%s", task.status, gs_error);
        show_gui_message("PAIRING FAILED", "Moonlight could not pair with this host.",
                         "Check the PIN and host, then try again.");
    } else {
        diag_log("PAIR_SUCCESS", "host pairing completed");
        add_saved_host_entry(config->address, config->port);
        show_gui_message("PAIRING COMPLETE", "This 3DS is paired with the host.",
                         "Return to the host menu to continue.");
    }

    // Revert to default HTTP timeout
    http_set_timeout_s(60);
    return cancelled;
}

static void action_unpair(SERVER_DATA *server) {
    if (gs_unpair(server) != GS_OK) {
        diag_log("UNPAIR_ERROR", "detail=%s", gs_error);
        show_gui_message("UNPAIR FAILED", "Moonlight could not remove pairing.",
                         "Check the host connection, then try again.");
    } else {
        diag_log("UNPAIR_SUCCESS", "host unpaired");
        show_gui_message("UNPAIRED", "This 3DS is no longer paired with the host.",
                         "The saved address stays in the host picker; pair again any time.");
    }
}

static bool action_repair_upload_trust(CONFIGURATION *config,
                                      SERVER_DATA *server) {
    if (config == NULL || server == NULL || !server->paired) {
        show_gui_message("HOST NOT PAIRED",
                         "Pair this 3DS with Sunshine before repairing upload trust.",
                         "Local logs remain available in Check Logs.");
        return false;
    }
    if (upload_pin_available(server)) {
        show_gui_message("UPLOAD TRUST AVAILABLE",
                         "This host already has a saved Sunshine certificate pin.",
                         "No pairing change is needed for uploads.");
        return false;
    }
    const std::vector<GuiMenuItem> choices = {
        {"Unpair and Pair Again", "Refresh the trusted Sunshine certificate pin using a new PIN.", "Local logs stay on SD"},
        {"Cancel", "Leave the current host pairing unchanged.", "No changes"},
    };
    int selected = 1;
    if (!run_gui_menu("REPAIR UPLOAD TRUST", "SUNSHINE PAIRING",
                      choices, &selected) || selected != 0)
        return false;

    diag_log("UPLOAD_TRUST_REPAIR", "phase=unpair_begin");
    if (gs_unpair(server) != GS_OK) {
        diag_log("UPLOAD_TRUST_REPAIR", "phase=unpair_failed detail=%s",
                 gs_error ? gs_error : "unknown");
        show_gui_message("TRUST REPAIR FAILED",
                         "Sunshine did not complete the unpair request.",
                         "The existing pairing and local logs remain available.");
        return false;
    }
    diag_log("UPLOAD_TRUST_REPAIR", "phase=pair_begin");
    return action_pair(config, server);
}

static void action_quit_stream(SERVER_DATA *server) {
    const int status = gs_quit_app(server);
    if (status != GS_OK) {
        diag_log("QUIT_APP_ERROR", "status=%d detail=%s", status, gs_error);
        show_gui_message("QUIT REQUEST FAILED", "The host did not close its app.",
                         "Check the host connection, then try again.");
    } else {
        diag_log("QUIT_APP_SUCCESS", "host app quit requested");
        show_gui_message("QUIT REQUEST SENT", "The host accepted the quit request.",
                         "Return to the host menu to continue.");
    }
}

int main_loop(int argc, char *argv[]) {
    init_3ds();

    CONFIGURATION config;
    config_parse(argc, argv, &config);
    diag_set_enabled(config.diagnostic_logging);
    diag_log("APP_START", "version=%s", DIAG_BUILD_VERSION);

    while (aptMainLoop()) {
        auto address_string = prompt_for_address();
        if (address_string == "__EXIT_TO_HOME__") {
            diag_log("APP_EXIT_REQUEST", "route=host_picker");
            return 0;
        }
        if (address_string.empty()) {
            continue;
        }
        std::string parsed_address;
        uint16_t parsed_port = static_cast<uint16_t>(config.port);
        if (!split_host_endpoint(address_string, parsed_port, &parsed_address,
                                 &parsed_port)) {
            show_gui_message("HOST ADDRESS INVALID",
                             "The host address or port is not valid.",
                             "Use a hostname or IP address with an optional :port.");
            continue;
        }
        address_string = parsed_address;
        config.port = parsed_port;
        config.address = (char *)address_string.c_str();
        const bool address_saved =
            add_saved_host_entry(config.address, config.port);
        diag_log("HOST_ENTRY_SELECTED", "saved=%s port=%d",
                 address_saved ? "true" : "false", config.port);
        if (!address_saved) {
            show_gui_message("HOST LIST SAVE FAILED",
                             "Moonlight could not save this address to the host picker.",
                             "You can try connecting now; the entry may not persist.");
        }
        const std::string profile_host_key =
            std::string(config.address) + ":" + std::to_string(config.port);
        diag_session_begin();
        diag_log("HOST_SELECTION", "host=%s port=%d", config.address,
                 config.port);
        SERVER_DATA server;
        InitServerCall connect_call{&config, &server, GS_ERROR};
        diag_http_summary_begin();
        run_gui_call("connect.server_init", "Checking host status",
                     init_server_call, &connect_call, true, false,
                     "Checking host status and pairing state");
        diag_http_summary_flush(connect_call.status, diag_cancel_requested());
        if (diag_cancel_requested()) {
            diag_log("CONNECT_CANCELLED", "returning to host list");
            diag_session_end("cancelled", "host_contact_cancelled");
            continue;
        }
        if (connect_call.status != GS_OK) {
            print_init_server_error(&config, connect_call.status);
            show_gui_message("HOST CONNECTION FAILED",
                             "Moonlight could not connect to this host.",
                             "Check that the host is online, then try again.");
            diag_session_end("failed", "host_info_failed");
            continue;
        }
        diag_log("HOST_INFO", "host connection initialized codec_flags=0x%x",
                 server.serverInfo.serverCodecModeSupport);
        diag_log("SESSION_PHASE", "name=host_info_ready");
        if (server.paired) ensure_upload_consent(&server);

        while (aptMainLoop()) {
            std::string action = prompt_for_action(&server);
            if (action.empty()) {
                finish_session(&server, "cancelled", "user_left_host_menu_before_stream");
                break;
            }
            config.action = (char *)action.c_str();

            if (strcmp("exit to 3ds home", config.action) == 0) {
                diag_log("APP_EXIT_REQUEST", "route=host_action_menu");
                finish_session(&server, "cancelled", "user_exit_before_stream");
                return 0;
            } else if (strcmp("stream", config.action) == 0) {
                const char *reason = action_stream(&config, &server);
                finish_session(&server,
                               diag_cancel_requested() ? "cancelled" : "complete",
                               reason);
                break;
            } else if (strcmp("pair", config.action) == 0) {
                bool cancelled = action_pair(&config, &server);
                finish_session(&server, cancelled ? "cancelled" : "complete",
                               "pair_action_selected");
                break;
            } else if (strcmp("repair upload trust", config.action) == 0) {
                bool cancelled = action_repair_upload_trust(&config, &server);
                finish_session(&server, cancelled ? "cancelled" : "complete",
                               "upload_trust_repair_selected");
                break;
            } else if (strcmp("stream settings", config.action) == 0) {
                prompt_for_stream_settings(&config);
            } else if (strcmp("stream profile", config.action) == 0) {
                prompt_for_stream_profile(&config, profile_host_key);
            } else if (strcmp("connection test", config.action) == 0) {
                action_connection_test(&config, &server, profile_host_key);
            } else if (strcmp("check logs", config.action) == 0) {
                show_check_logs(&server);
            } else if (strcmp("send logs", config.action) == 0) {
                send_current_session(&server);
            } else if (strcmp("toggle automatic logs", config.action) == 0) {
                toggle_automatic_upload(&server);
            } else if (strcmp("unpair", config.action) == 0) {
                finish_session(&server, "complete", "unpair_action_selected");
                action_unpair(&server);
                break;
            } else if (strcmp("quit stream", config.action) == 0) {
                action_quit_stream(&server);
                finish_session(&server, "complete", "quit_action_selected");
                break;
            } else {
                printf("%s is not a valid action\n", config.action);
                wait_for_button();
            }
        }
        finish_session(&server, "cancelled", "host_menu_loop_ended");
    }
    return 0;
}

int main(int argc, char *argv[]) {
    try {
        main_loop(argc, argv);
    } catch (const std::exception &ex) {
        printf("Moonlight crashed with the following error: %s\n", ex.what());
        return 1;
    } catch (const std::string &ex) {
        printf("Moonlight crashed with the following error message: %s\n",
               ex.c_str());
        return 1;
    } catch (...) {
        printf("Moonlight crashed with an unknown error\n");
        return 1;
    }
    return 0;
}

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
#include "input/n3ds_input.hpp"
#include "input/touch/TouchHandler.hpp"
#include "system/AtomicVar.hpp"
#include "system/dispatcher.hpp"
#include "system/n3ds_connection.hpp"
#include "system/pair_record.hpp"
#include "video/video.hpp"

#include <3ds.h>
#include <Limelight.h>

#include <client.h>
#include <discover.h>
#include <http.h>

#include <arpa/inet.h>
#include <exception>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/rand.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

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
        printf("     %.32s %s\n",
               tls_fingerprint[32] ? tls_fingerprint + 32 : "----",
               tls_source[0] ? tls_source : "----");
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

static int console_selection_prompt(std::string prompt,
                                    std::vector<std::string> options,
                                    int default_idx) {
    int option_idx = default_idx;
    int last_option_idx = -1;
    while (aptMainLoop()) {
        if (option_idx != last_option_idx) {
            consoleClear();
            if (!prompt.empty()) {
                printf("%s\n", prompt.c_str());
            }
            printf("Press up/down to select\n");
            printf("Press A to confirm\n");
            printf("Press B to go back\n\n");

            for (int i = 0; i < options.size(); i++) {
                if (i == option_idx) {
                    printf(">%s\n", options[i].c_str());
                } else {
                    printf("%s\n", options[i].c_str());
                }
            }
            last_option_idx = option_idx;
        }

        n3ds_draw_diag_version_overlay();
        gfxSwapBuffers();
        gfxFlushBuffers();
        gspWaitForVBlank();

        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_A) {
            consoleClear();
            return option_idx;
        }
        if (kDown & KEY_B) {
            consoleClear();
            return -1;
        }
        if (kDown & KEY_DOWN) {
            if (option_idx < options.size() - 1) {
                option_idx++;
            }
        } else if (kDown & KEY_UP) {
            if (option_idx > 0) {
                option_idx--;
            }
        }
    }

    exit(0);
}

static std::string prompt_for_action(PSERVER_DATA server) {
    if (server->paired) {
        std::vector<std::string> actions = {
            "stream",
            "quit stream",
            "stream settings",
            "unpair",
        };
        int idx = console_selection_prompt("Select an action", actions, 0);
        if (idx < 0) {
            return "";
        }
        return actions[idx];
    }
    std::vector<std::string> actions = {"pair"};
    int idx = console_selection_prompt("Select an action", actions, 0);
    if (idx < 0) {
        return "";
    }
    return actions[idx];
}

static std::string prompt_for_address() {
    auto address_list = list_paired_addresses();
    address_list.push_back("new");
    int idx =
        console_selection_prompt("Select a server address", address_list, 0);
    if (idx < 0) {
        return "";
    } else if (address_list[idx] != "new") {
        return address_list[idx];
    }

    // Prompt users for a custom address
    SwkbdState swkbd;
    char *addr_buff = (char *)malloc(MAX_INPUT_CHAR);
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 3, -1);
    swkbdSetHintText(&swkbd, "Address of host to connect to");
    swkbdInputText(&swkbd, addr_buff, MAX_INPUT_CHAR);
    std::string addr_string = std::string(addr_buff);
    free(addr_buff);
    trim(addr_string);
    return addr_string;
}

static VIDEO_DECODER_TYPE
prompt_for_video_decoder(VIDEO_DECODER_TYPE default_val) {
    std::vector<std::string> decoders = {
        "hardware (fast, *new* 3DS only)",
        "software (slow)",
        "disable video",
    };
    int idx = console_selection_prompt("Select a video option", decoders,
                                       (int)default_val);
    if (idx < 0) {
        return default_val;
    }
    return (VIDEO_DECODER_TYPE)idx;
}

static bool prompt_for_boolean(std::string prompt, bool default_val) {
    std::vector<std::string> options = {
        "true",
        "false",
    };
    int idx = console_selection_prompt(prompt, options, default_val ? 0 : 1);
    if (idx < 0) {
        idx = default_val ? 0 : 1;
    }
    return idx == 0;
}

static int prompt_for_int(std::string initial_text) {
    char *setting_buff = (char *)malloc(MAX_INPUT_CHAR);
    memset(setting_buff, 0, MAX_INPUT_CHAR);

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NUMPAD, 1, 8);
    swkbdSetInitialText(&swkbd, initial_text.c_str());
    swkbdInputText(&swkbd, setting_buff, MAX_INPUT_CHAR);
    std::string setting_str = std::string(setting_buff);

    free(setting_buff);
    trim(setting_str);
    return std::stoi(setting_str);
}

static void prompt_for_stream_settings(PCONFIGURATION config) {
    std::vector<std::string> setting_names = {
        "width",
        "height",
        "fps",
        "motion_controls",
        "bitrate",
        "packetsize",
        "sops",
        "localaudio",
        "quitappafter",
        "viewonly",
        "video_decoder",
        "swapfacebuttons",
        "swaptriggersandshoulders",
        "usetriggersformouse",
    };
    int idx = 0;
    while (1) {
        std::string prompt = "Select a setting";
        if (config->stream.width % GSP_SCREEN_HEIGHT_TOP &&
            config->stream.width % GSP_SCREEN_HEIGHT_BOTTOM) {
            prompt += "\n\nWARNING: Using an unsupported width may "
                      "cause issues (3DS supports multiples of 400 or 320)\n";
        }
        if (config->stream.height % GSP_SCREEN_WIDTH) {
            prompt += "\n\nWARNING: Using an unsupported height may "
                      "cause issues (3DS supports multiples of 240)\n";
        }
        idx = console_selection_prompt(prompt, setting_names, idx);
        if (idx < 0) {
            break;
        }

        if ("width" == setting_names[idx]) {
            config->stream.width =
                prompt_for_int(std::to_string(config->stream.width));
        } else if ("height" == setting_names[idx]) {
            config->stream.height =
                prompt_for_int(std::to_string(config->stream.height));
        } else if ("motion_controls" == setting_names[idx]) {
            config->motion_controls = prompt_for_boolean(
                "Enable Motion Controls", config->motion_controls);
        } else if ("fps" == setting_names[idx]) {
            config->stream.fps =
                prompt_for_int(std::to_string(config->stream.fps));
        } else if ("bitrate" == setting_names[idx]) {
            config->stream.bitrate =
                prompt_for_int(std::to_string(config->stream.bitrate));
        } else if ("packetsize" == setting_names[idx]) {
            config->stream.packetSize =
                prompt_for_int(std::to_string(config->stream.packetSize));
        } else if ("sops" == setting_names[idx]) {
            config->sops = prompt_for_boolean(
                "Optimize Game settings for streaming", config->sops);
        } else if ("localaudio" == setting_names[idx]) {
            config->localaudio =
                prompt_for_boolean("Enable local audio", config->localaudio);
        } else if ("quitappafter" == setting_names[idx]) {
            config->quitappafter = prompt_for_boolean(
                "Quit app after streaming", config->quitappafter);
        } else if ("viewonly" == setting_names[idx]) {
            config->viewonly = prompt_for_boolean("Disable controller input",
                                                  config->viewonly);
        } else if ("video_decoder" == setting_names[idx]) {
            config->video_decoder =
                prompt_for_video_decoder(config->video_decoder);
        } else if ("swapfacebuttons" == setting_names[idx]) {
            config->swap_face_buttons = prompt_for_boolean(
                "Swaps A/B and X/Y to match Xbox controller layout",
                config->swap_face_buttons);
        } else if ("swaptriggersandshoulders" == setting_names[idx]) {
            config->swap_triggers_and_shoulders = prompt_for_boolean(
                "Swaps L/ZL and R/ZR for a more natural feel",
                config->swap_triggers_and_shoulders);
        } else if ("usetriggersformouse" == setting_names[idx]) {
            config->use_triggers_for_mouse =
                prompt_for_boolean("Use ZL/ZR as left/right mouse buttons",
                                   config->use_triggers_for_mouse);
        }
    }

    // Update the config file
    char *config_file_path = (char *)MOONLIGHT_3DS_PATH "/moonlight.conf";
    config_save(config_file_path, config);
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

static void app_list_task(void *opaque) {
    AppListTaskContext *task = static_cast<AppListTaskContext *>(opaque);
    task->status = gs_applist(task->server, &task->list);
}

static int prompt_for_app_id(PSERVER_DATA server) {
    PAPP_LIST list = NULL;
    AppListTaskContext task{server, NULL, GS_ERROR};
    run_monitored_call("connect.app_list", app_list_task, &task);
    if (diag_cancel_requested()) {
        diag_log("CONNECT_CANCELLED", "phase=connect.applist");
        return -1;
    }
    list = task.list;
    if (task.status != GS_OK) {
        printf("Can't get app list\n");
        return -1;
    }

    std::vector<std::string> app_names;
    std::vector<int> app_ids;
    while (list != NULL) {
        printf("%d. %s\n", list->id, list->name);
        app_names.push_back(std::string(list->name));
        app_ids.push_back(list->id);
        list = list->next;
    }

    int id_idx = console_selection_prompt("Select an app", app_names, 0);
    if (id_idx == -1) {
        return -1;
    }
    return app_ids[id_idx];
}

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
    input_handler->force_touchscreen_menu();
    while (!connection_listener->is_connection_closed()) {
        gspWaitForAnyEvent();
        input_handler->n3dsinput_handle_event();
    }
}

static inline void stream_loop(PCONFIGURATION config,
                               N3dsConnectionListener *connection_listener,
                               std::shared_ptr<N3dsInput> input_handler) {
    // Spin off worker threads
    size_t stack_size = 0x20000;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    if (!config->viewonly) {
        threadCreate(input_loop, input_handler.get(), stack_size, priority, -1,
                     true);
    }
    threadCreate(dispatch_loop, nullptr, stack_size, priority, -1, true);

    // Run the main connection loop
    while (!connection_listener->is_connection_closed() && aptMainLoop()) {
        gspWaitForAnyEvent();
        if (aptShouldClose()) {
            connection_listener->connection_terminated(0);
        }
    }
}

struct LimelightStartCall {
    PSERVER_DATA server;
    PCONFIGURATION config;
    PDECODER_RENDERER_CALLBACKS video_callbacks;
    AUDIO_RENDERER_CALLBACKS *audio_callbacks;
    int status;
};

static void limelight_start_call(void *opaque) {
    LimelightStartCall *call = static_cast<LimelightStartCall *>(opaque);
    call->status = LiStartConnection(
        &call->server->serverInfo, &call->config->stream,
        &n3ds_connection_callbacks, call->video_callbacks,
        call->audio_callbacks, NULL, DISPLAY_FULLSCREEN,
        call->config->audio_device, 0);
}

static void stream(PSERVER_DATA server, PCONFIGURATION config, int appId,
                   std::shared_ptr<N3dsInput> input_handler) {
    int gamepad_mask = 1;
    diag_log("STREAM_START_REQUESTED", "app_id=%d", appId);
    struct StartAppCall {
        PSERVER_DATA server;
        PCONFIGURATION config;
        int app_id;
        int gamepad_mask;
        int status;
    } start_call{server, config, appId, gamepad_mask, GS_ERROR};
    auto start_app_call = [](void *opaque) {
        StartAppCall *call = static_cast<StartAppCall *>(opaque);
        call->status = gs_start_app(call->server, &call->config->stream,
                                    call->app_id, call->config->sops,
                                    call->config->localaudio, call->gamepad_mask);
    };
    run_monitored_call("stream.start_app", start_app_call, &start_call);
    if (diag_cancel_requested()) {
        diag_log("STREAM_STOP", "app_id=%d reason=watchdog_cancelled", appId);
        return;
    }
    int ret = start_call.status;
    if (ret < 0) {
        diag_log("STREAM_STOP", "app_id=%d reason=start_app_failed result=%d",
                 appId, ret);
        if (ret == GS_NOT_SUPPORTED_4K)
            printf("Server doesn't support 4K\n");
        else if (ret == GS_NOT_SUPPORTED_MODE)
            printf("Server doesn't support %dx%d (%d fps) or remove "
                   "--nounsupported option\n",
                   config->stream.width, config->stream.height,
                   config->stream.fps);
        else if (ret == GS_NOT_SUPPORTED_SOPS_RESOLUTION)
            printf(
                "Optimal Playable Settings isn't supported for the resolution "
                "%dx%d, use supported resolution or disable 'sops' option\n",
                config->stream.width, config->stream.height);
        else if (ret == GS_ERROR)
            printf("Gamestream error: %s\n", gs_error);
        else
            printf("Errorcode starting app: %d\n", ret);
        wait_for_button();
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

    printf(
        "Loading...\nStream %dx%d, %dfps, %dkbps, sops=%d, localaudio=%d, quitappafter=%d,\
 viewonly=%d, encryption=%x, video_decoder=%d, swapfacebuttons=%d, swaptriggersandshoulders=%d,\
 usetriggersformouse=%d, motion_controls=%d\n",
        config->stream.width, config->stream.height, config->stream.fps,
        config->stream.bitrate, config->sops, config->localaudio,
        config->quitappafter, config->viewonly, config->stream.encryptionFlags,
        config->video_decoder, config->swap_face_buttons,
        config->swap_triggers_and_shoulders, config->use_triggers_for_mouse,
        config->motion_controls);

    auto connection_listener =
        N3dsConnectionListener::create_instance(config->motion_controls);
    LimelightStartCall connection_call{server, config, video_callbacks,
                                       audio_callbacks, GS_ERROR};
    run_monitored_call("stream.limelight_start", limelight_start_call,
                       &connection_call, false);
    int status = connection_call.status;
    diag_phase_outcome("stream.limelight_start",
                       status == 0 ? "success" : "failed", status);

    if (status != 0) {
        diag_log("STREAM_STOP", "app_id=%d reason=connection_start_failed status=%d",
                 appId, status);
        n3ds_connection_callbacks.connectionTerminated(status);
        printf("Connection failed with error: %d\n", status);
        wait_for_button();
        return;
    }

    printf("Connected!\n");
    diag_log("STREAM_STARTED", "app_id=%d", appId);
    diag_set_phase("stream.active");
    stream_loop(config, connection_listener, input_handler);

    diag_set_phase("stream.stopping");
    diag_log("STREAM_STOP", "app_id=%d reason=connection_loop_ended", appId);
    LiStopConnection();
    diag_phase_outcome("stream.stopping", "complete", GS_OK);
    N3dsConnectionListener::destroy_instance();

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
    diag_set_phase("connect.host_state");
    if (server->paired) {
        add_pair_address(config->address, config->port);
    } else {
        remove_pair_address(config->address, config->port);
    }
    diag_phase_outcome("connect.host_state", "success", GS_OK);
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
        printf("Not enough memory\n");
    } else if (status == GS_ERROR) {
        printf("Gamestream error: %s\n", gs_error);
    } else if (status == GS_INVALID) {
        printf("Invalid data received from server: %s\n", gs_error);
    } else if (status == GS_UNSUPPORTED_VERSION) {
        printf("Unsupported version: %s\n", gs_error);
    } else {
        printf("Can't connect to server %s:%d\n", config->address,
               config->port);
    }
}

static void action_stream(CONFIGURATION *config, SERVER_DATA *server) {
    int appId = prompt_for_app_id(server);
    if (appId == -1) {
        return;
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
    stream(server, config, appId, input_handler);
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
    printf("Please enter the following PIN on the target PC:\n%s\n", pin);

    // Actually display the PIN on screen by swapping buffers
    n3ds_draw_diag_version_overlay();
    gfxSwapBuffers();
    gfxFlushBuffers();
    gspWaitForVBlank();

    PairTaskContext task{server, &pin[0], GS_ERROR};
    run_monitored_call("pair", pair_task, &task);
    bool cancelled = diag_cancel_requested();
    if (cancelled) {
        diag_log("PAIR_CANCELLED", "returning to host list after watchdog cancellation");
    } else if (task.status != GS_OK) {
        printf("Failed to pair to server: %s\n", gs_error);
    } else {
        printf("Succesfully paired\n");
        // Display success message before breaking
        n3ds_draw_diag_version_overlay();
        gfxSwapBuffers();
        gfxFlushBuffers();
        gspWaitForVBlank();
        add_pair_address(config->address, config->port);
    }

    // Revert to default HTTP timeout
    http_set_timeout_s(60);
    return cancelled;
}

static void action_unpair(CONFIGURATION *config, SERVER_DATA *server) {
    if (gs_unpair(server) != GS_OK) {
        printf("Failed to unpair from server: %s\n", gs_error);
    } else {
        printf("Succesfully unpaired\n");
        remove_pair_address(config->address, config->port);
    }
}

static void action_quit_stream(SERVER_DATA *server) {
    printf("Sending app quit request ...\n");
    gs_quit_app(server);
    printf("Request completed\n");
}

int main_loop(int argc, char *argv[]) {
    init_3ds();

    CONFIGURATION config;
    config_parse(argc, argv, &config);
    diag_log("APP_START", "version=%s", DIAG_BUILD_VERSION);

    while (aptMainLoop()) {
        auto address_string = prompt_for_address();
        if (address_string.empty()) {
            continue;
        }
        // Split address and port (if specified)
        uint32_t port_delim_pos = address_string.find(':');
        if (port_delim_pos != std::string::npos) {
            std::string port_string = address_string.substr(port_delim_pos + 1);
            address_string = address_string.substr(0, port_delim_pos);
            config.port = std::stoi(port_string);
        }
        config.address = (char *)address_string.c_str();
        diag_log("HOST_SELECTION", "host=%s port=%d", config.address,
                 config.port);
        printf("Connecting to %s:%d...\n", config.address, config.port);

        SERVER_DATA server;
        InitServerCall connect_call{&config, &server, GS_ERROR};
        run_monitored_call("connect.server_init", init_server_call,
                           &connect_call);
        if (diag_cancel_requested()) {
            diag_log("CONNECT_CANCELLED", "returning to host list");
            continue;
        }
        if (connect_call.status != GS_OK) {
            print_init_server_error(&config, connect_call.status);
            wait_for_button();
            continue;
        }
        printf("GPU: %s, GFE: %s (%s, %s)\n", server.gpuType,
               server.serverInfo.serverInfoGfeVersion, server.gsVersion,
               server.serverInfo.serverInfoAppVersion);
        printf("Server codec flags: 0x%x\n",
               server.serverInfo.serverCodecModeSupport);

        while (aptMainLoop()) {
            std::string action = prompt_for_action(&server);
            if (action.empty()) {
                break;
            }
            config.action = (char *)action.c_str();

            if (strcmp("stream", config.action) == 0) {
                action_stream(&config, &server);
                break;
            } else if (strcmp("pair", config.action) == 0) {
                bool cancelled = action_pair(&config, &server);
                if (!cancelled) {
                    wait_for_button();
                }
                break;
            } else if (strcmp("stream settings", config.action) == 0) {
                prompt_for_stream_settings(&config);
            } else if (strcmp("unpair", config.action) == 0) {
                action_unpair(&config, &server);
                wait_for_button();
                break;
            } else if (strcmp("quit stream", config.action) == 0) {
                action_quit_stream(&server);
                wait_for_button();
                break;
            } else {
                printf("%s is not a valid action\n", config.action);
                wait_for_button();
            }
        }
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

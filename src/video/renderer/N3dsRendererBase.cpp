/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
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

#include "N3dsRenderer.hpp"
#include "../../input/n3ds_input.hpp"
#include "N3ds3DPresentation.hpp"
#include "../../diagnostics.h"
#include "vshader_shbin.h"
#include "../../input/touch/TouchHandler.hpp"

#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdbool.h>
#include <stdexcept>
#include <unistd.h>

static constexpr u64 GPU_CLEAR_WAIT_TIMEOUT_NS = 250000000ULL;
static AtomicVar<bool> psc0_wait_worker_active = false;

static void psc0_wait_worker(void *) {
    gspWaitForEvent(GSPGPU_EVENT_PSC0, false);
    psc0_wait_worker_active.store(false);
}

static bool wait_for_psc0_bounded(u64 timeout_ns) {
    if (psc0_wait_worker_active.load()) {
        return false;
    }

    psc0_wait_worker_active.store(true);
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    Thread waiter =
        threadCreate(psc0_wait_worker, NULL, 0x2000, priority, -1, false);
    if (waiter == NULL) {
        psc0_wait_worker_active.store(false);
        return false;
    }

    const Result join_result = threadJoin(waiter, timeout_ns);
    if (R_SUCCEEDED(join_result)) {
        threadFree(waiter);
        return true;
    }

    threadDetach(waiter);
    return false;
}

N3dsRendererBase::N3dsRendererBase(gfxScreen_t screen_in, int surface_width_in,
                                   int surface_height_in, int image_width_in,
                                   int image_height_in, int pixel_size,
                                   bool debug_in)
    : screen(screen_in), surface_width(surface_width_in),
      surface_height(surface_height_in), image_width(image_width_in),
      image_height(image_height_in), debug(debug_in), px_size(pixel_size) {
    cmdlist = (u32 *)linearAlloc(CMDLIST_SZ * 4);
    vramFb = vramAlloc(surface_width * surface_height * px_size);
    // Needs to be able to hold an 800x480
    vramTex = vramAlloc(MOON_CTR_VIDEO_TEX_W * MOON_CTR_VIDEO_TEX_H * px_size);
}

N3dsRendererBase::~N3dsRendererBase() {
    diag_log("TEARDOWN", "renderer_destroy screen=%s phase=before",
             screen == GFX_TOP ? "TOP" : "BOTTOM");
    diag_log("RENDER",
             "renderer_destroy screen=%s frames_entered=%llu frames_completed=%llu",
             screen == GFX_TOP ? "TOP" : "BOTTOM",
             (unsigned long long)renderer_frames_entered,
             (unsigned long long)renderer_frames_completed);

    linearFree(cmdlist);
    vramFree(vramFb);
    vramFree(vramTex);

    // Screen clears happen once, on the teardown coordinator, after all
    // renderers and Limelight callbacks have stopped.
    diag_log("TEARDOWN", "renderer_destroy screen=%s phase=after",
             screen == GFX_TOP ? "TOP" : "BOTTOM");
}

void N3dsRendererBase::ensure_3d_enabled() {
    if (!gfxIs3D()) {
        gfxSetWide(false);
        gfxSet3D(true);
    }
}

void N3dsRendererBase::ensure_3d_disabled() {
    if (gfxIs3D()) {
        gfxSet3D(false);
    }
    if (surface_width == GSP_SCREEN_HEIGHT_TOP_2X) {
        gfxSetWide(true);
    }
}

inline void N3dsRendererBase::write24(u8 *p, u32 val) {
    p[0] = val;
    p[1] = val >> 8;
    p[2] = val >> 16;
}

inline void N3dsRendererBase::draw_perf_counters() {
    u8 *dest = gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);

    // Use a line going across the first scanline (left) for the perf counters.
    // Clear to black
    memset(dest, 0, GSP_SCREEN_WIDTH * 3);

    // Display frame target in the middle of the screen.
    double perf_tick_divisor =
        ((double)GSP_SCREEN_WIDTH) / ((double)(perf_frame_target_ticks * 2));
    u32 perf_px = 0;
    u32 perf_tmp_height = 0;

#define PERF_DRAW(ticks, r, g, b)                                              \
    perf_tmp_height = perf_tick_divisor * ((double)(ticks));                   \
    do {                                                                       \
        if (perf_px > GSP_SCREEN_WIDTH)                                        \
            break;                                                             \
        const u32 color = (r << 16) | (g << 8) | b;                            \
        memcpy(dest + (perf_px * 3), &color, 3);                               \
        perf_px++;                                                             \
    } while (perf_tmp_height-- > 0);

    PERF_DRAW(perf_decode_ticks, 255, 0, 0);
    PERF_DRAW(perf_fbcopy_ticks, 0, 0, 255);

    // Draw two green pixels at the center
    perf_px = (GSP_SCREEN_WIDTH / 2) - 1;
    PERF_DRAW(0, 0, 255, 0);
    PERF_DRAW(0, 0, 255, 0);
}

void N3dsRendererBase::write_px_to_framebuffer_gpu(uint8_t *__restrict source) {
    const u64 frame_start_ticks = svcGetSystemTick();
    const u64 frame_number = ++renderer_frames_entered;
    const bool log_frame = frame_number <= 10;
    if (log_frame) {
        const N3ds3DSettings entry_settings = n3ds_3d_settings_snapshot();
        diag_log("RENDER",
                 "frame=%llu phase=entry screen=%s surface=%dx%d image=%dx%d mode=%s aspect=%s shift=%d gfx3d=%.2f",
                 (unsigned long long)frame_number,
                 screen == GFX_TOP ? "TOP" : "BOTTOM", surface_width,
                 surface_height, image_width, image_height,
                 n3ds_3d_mode_name(entry_settings.mode),
                 n3ds_3d_aspect_name(entry_settings.aspect),
                 entry_settings.shift, (double)gfxIs3D());
    }

    // Do nothing when GPU right is lost, otherwise we hang when going to
    // the home menu.
    if (!gspHasGpuRight()) {
        return;
    }

    u64 start_ticks = svcGetSystemTick();

    // NOTE: At 800x480, we can display the _width_ natively, but the height
    // needs to be downsampled. MVD is incapable of downsampling, so we have to
    // do it on the GPU.

    // TODO: If we can use rotation from the decoder, we can do a 2x downscale
    // using display transfer and skip P3D. Not necessary because PICA is
    // significantly faster than the decoder.

    // Tile the source image into the scratch buffer.
    if (log_frame) {
        diag_log("RENDER",
                 "frame=%llu eye=SHARED stage=texture_display_transfer phase=begin",
                 (unsigned long long)frame_number);
    }
    GX_DisplayTransfer(
        (u32 *)source,
        GX_BUFFER_DIM(MOON_CTR_VIDEO_TEX_W, MOON_CTR_VIDEO_TEX_H),
        (u32 *)vramTex,
        GX_BUFFER_DIM(MOON_CTR_VIDEO_TEX_W, MOON_CTR_VIDEO_TEX_H),
        GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) |
            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565));
    if (log_frame) {
        diag_log("RENDER",
                 "frame=%llu eye=SHARED stage=texture_display_transfer phase=end result=returned",
                 (unsigned long long)frame_number);
    }

    // Compose one independent eye image at a time in the existing GPU frame
    // presentation path. Updating these rectangles is safe mid-stream because
    // every decoded frame reads a fresh settings snapshot.
    const N3ds3DSettings settings = n3ds_3d_settings_snapshot();
    const int eye_pass_count = screen == GFX_TOP ? 2 : 1;
    for (int eye = 0; eye < eye_pass_count; eye++) {
        const char *eye_name = screen == GFX_TOP
                                   ? (eye == 0 ? "LEFT" : "RIGHT")
                                   : "MONO";
        const N3ds3DEyePresentation eye_presentation =
            screen == GFX_TOP
                ? n3ds_3d_eye_presentation(settings, image_width,
                                           image_height, eye)
                : N3ds3DEyePresentation{
                      0.0f, image_width / 1024.0f, 0.0f,
                      image_height / 512.0f, 1.0f, 1.0f, 0.0f};
        // While the transfer is running, create a temporary command list to
        // rotate the framebuffer into source.
        GPUCMD_SetBuffer(cmdlist, CMDLIST_SZ, 0);

        // TODO: Verify this mitigates rounding errors due to f24 precision issues.

    #define C GPUCMD_AddWrite

        C(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
        C(GPUREG_COLORBUFFER_LOC, osConvertVirtToPhys(vramFb) >> 3);
        C(GPUREG_DEPTHBUFFER_LOC, 0);
        C(GPUREG_RENDERBUF_DIM,
          (1 << 24) | ((surface_width - 1) << 12) | surface_height);
        C(GPUREG_FRAMEBUFFER_DIM,
          (1 << 24) | ((surface_width - 1) << 12) | surface_height);
        C(GPUREG_FRAMEBUFFER_BLOCK32, 0);

        C(GPUREG_DEPTH_COLOR_MASK, 0xF << 8); // Write RGBA, no depth
        C(GPUREG_EARLYDEPTH_TEST1, 0);
        C(GPUREG_EARLYDEPTH_TEST2, 0);
        C(GPUREG_COLORBUFFER_FORMAT, GPU_RGB565 << 16);
        C(GPUREG_COLORBUFFER_READ,
          0x0); // Buffer is uninitialized and should not be read.
        C(GPUREG_COLORBUFFER_WRITE, 0xF);
        C(GPUREG_DEPTHBUFFER_READ, 0); // No depth buffer
        C(GPUREG_DEPTHBUFFER_WRITE, 0);

        C(GPUREG_VIEWPORT_XY, 0);

        C(GPUREG_VIEWPORT_WIDTH, f32tof24(surface_height / 2));
        C(GPUREG_VIEWPORT_INVW, f32tof31(2.0 / ((double)surface_height)) << 1);
        C(GPUREG_VIEWPORT_HEIGHT, f32tof24(surface_width / 2));
        C(GPUREG_VIEWPORT_INVH, f32tof31(2.0 / ((double)surface_width)) << 1);

        C(GPUREG_SCISSORTEST_MODE, 0);
        C(GPUREG_SCISSORTEST_POS, 0);
        C(GPUREG_SCISSORTEST_DIM, 0);

        C(GPUREG_DEPTHMAP_ENABLE, 1);
        C(GPUREG_DEPTHMAP_SCALE, f32tof24(-1.0));
        C(GPUREG_DEPTHMAP_OFFSET, 0);
        C(GPUREG_STENCIL_TEST, 0);
        C(GPUREG_FRAGOP_ALPHA_TEST, 0);
        C(GPUREG_LOGIC_OP, 3);
        C(GPUREG_COLOR_OPERATION, 0x00E40000);

        // Texturing
        C(GPUREG_TEXUNIT0_TYPE, GPU_RGB565);
        C(GPUREG_TEXUNIT0_DIM, MOON_CTR_VIDEO_TEX_H | (MOON_CTR_VIDEO_TEX_W << 16));
        C(GPUREG_TEXUNIT0_ADDR1, osConvertVirtToPhys(vramTex) >> 3);
        C(GPUREG_TEXUNIT0_PARAM,
          GPU_NEAREST | (GPU_LINEAR << 1)); // Linear min and mag filter

        // Shading
        // GPUCMD_AddMaskedWrite(GPUREG_SH_OUTATTR_CLOCK, 0x2, 1 << 8); // No Z, Yes
        // texcoord0
        C(GPUREG_TEXUNIT_CONFIG,
          1 | (1 << 12) | (1 << 16)); // Activate texture 0, clear texture cache

        C(GPUREG_TEXENV0_SOURCE, 0x003003); // Texture 0
        C(GPUREG_TEXENV0_OPERAND, 0);       // Source Color
        C(GPUREG_TEXENV0_COMBINER, 0);      // Replace
        C(GPUREG_TEXENV0_SCALE, 0);         // No Scale

        C(GPUREG_TEXENV1_SOURCE, 0x003003); // Texture 0
        C(GPUREG_TEXENV1_OPERAND, 0);       // Source Color
        C(GPUREG_TEXENV1_COMBINER, 0);      // Replace
        C(GPUREG_TEXENV1_SCALE, 0);         // No Scale

        C(GPUREG_TEXENV2_SOURCE, 0x003003); // Texture 0
        C(GPUREG_TEXENV2_OPERAND, 0);       // Source Color
        C(GPUREG_TEXENV2_COMBINER, 0);      // Replace
        C(GPUREG_TEXENV2_SCALE, 0);         // No Scale

        C(GPUREG_TEXENV3_SOURCE, 0x003003); // Texture 0
        C(GPUREG_TEXENV3_OPERAND, 0);       // Source Color
        C(GPUREG_TEXENV3_COMBINER, 0);      // Replace
        C(GPUREG_TEXENV3_SCALE, 0);         // No Scale

        C(GPUREG_TEXENV4_SOURCE, 0x003003); // Texture 0
        C(GPUREG_TEXENV4_OPERAND, 0);       // Source Color
        C(GPUREG_TEXENV4_COMBINER, 0);      // Replace
        C(GPUREG_TEXENV4_SCALE, 0);         // No Scale

        C(GPUREG_TEXENV5_SOURCE, 0x003003); // Texture 0
        C(GPUREG_TEXENV5_OPERAND, 0);       // Source Color
        C(GPUREG_TEXENV5_COMBINER, 0);      // Replace
        C(GPUREG_TEXENV5_SCALE, 0);         // No Scale

        // Attribute buffers
        C(GPUREG_ATTRIBBUFFERS_LOC, 0);
        C(GPUREG_ATTRIBBUFFERS_FORMAT_LOW, 0);
        C(GPUREG_ATTRIBBUFFERS_FORMAT_HIGH,
          (0xFFF << 16) | (1 << 28)); // Two fixed vertex attributes

        // Vertex Shader
        static DVLB_s *vshader_dvlb = NULL;
        static shaderProgram_s program;

        if (!vshader_dvlb) {
            vshader_dvlb = DVLB_ParseFile((u32 *)vshader_shbin, vshader_shbin_size);
            shaderProgramInit(&program);
            shaderProgramSetVsh(&program, &vshader_dvlb->DVLE[0]);
        }

        shaderProgramUse(&program);

        C(GPUREG_VSH_NUM_ATTR, 1); // 2 attributes
        GPUCMD_AddMaskedWrite(GPUREG_VSH_INPUTBUFFER_CONFIG, 0xB,
                              1 | (0xA0 << 24)); // 2 attributes, no geometry shader
        C(GPUREG_VSH_ATTRIBUTES_PERMUTATION_LOW, 0x00000010);
        C(GPUREG_VSH_ATTRIBUTES_PERMUTATION_HIGH, 0);

        // Geometry Pipeline
        C(GPUREG_FACECULLING_CONFIG, 0);
        C(GPUREG_GEOSTAGE_CONFIG, 0);
        GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2,
                              (1 << 8) |
                                  1); // 2 outmap registers, drawing triangle strip
        C(GPUREG_INDEXBUFFER_CONFIG, 0x80000000);
        C(GPUREG_RESTART_PRIMITIVE, 1);

        // Vertex Data
        GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 1, 1);
        GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
        C(GPUREG_FIXEDATTRIB_INDEX, 0xF);

        union {
            u32 packed[3];
            struct {
                u8 x[3], y[3], z[3], w[3];
            };
        } param;

    #define ATTR(X, Y, Z, W)                                                       \
        {                                                                          \
            write24(param.x, f32tof24(X));                                         \
            write24(param.y, f32tof24(Y));                                         \
            write24(param.z, f32tof24(Z));                                         \
            write24(param.w, f32tof24(W));                                         \
                                                                                   \
            u32 p = param.packed[0];                                               \
            param.packed[0] = param.packed[2];                                     \
            param.packed[2] = p;                                                   \
            GPUCMD_AddIncrementalWrites(GPUREG_FIXEDATTRIB_DATA0, param.packed,    \
                                        3);                                        \
        }

        const float top = eye_presentation.vertical_scale;
        const float bottom = -eye_presentation.vertical_scale;
        const float left = -eye_presentation.horizontal_scale +
                           eye_presentation.horizontal_offset;
        const float right = eye_presentation.horizontal_scale +
                            eye_presentation.horizontal_offset;

        ATTR(top, left, 0.0, 0.0); // TR
        ATTR(eye_presentation.u1, eye_presentation.v0, 0.0, 0.0);

        ATTR(bottom, left, 0.0, 0.0); // TL
        ATTR(eye_presentation.u1, eye_presentation.v1, 0.0, 0.0);

        ATTR(top, right, 0.0, 0.0); // BR
        ATTR(eye_presentation.u0, eye_presentation.v0, 0.0, 0.0);

        ATTR(bottom, right, 0.0, 0.0); // BL
        ATTR(eye_presentation.u0, eye_presentation.v1, 0.0, 0.0);

        // End Geometry Pipeline
        GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
        GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 1, 0);
        C(GPUREG_VTX_FUNC, 1);

        // Stop Command List
        GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0x00000000);
        C(GPUREG_FRAMEBUFFER_FLUSH, 1);
        C(GPUREG_FRAMEBUFFER_INVALIDATE, 1);

    #undef C

        if (eye == 0) {
            if (log_frame) {
                diag_log("RENDER",
                         "frame=%llu eye=%s stage=pre_process_ppf_wait phase=begin",
                         (unsigned long long)frame_number, eye_name);
            }
            gspWaitForEvent(GSPGPU_EVENT_PPF, 0);
            if (log_frame) {
                diag_log("RENDER",
                         "frame=%llu eye=%s stage=pre_process_ppf_wait phase=end result=returned",
                         (unsigned long long)frame_number, eye_name);
            }
        } else if (log_frame) {
            diag_log("RENDER",
                     "frame=%llu eye=%s stage=pre_process_ppf_wait phase=skipped",
                     (unsigned long long)frame_number, eye_name);
        }

        u32 *unused;
        u32 cmdlist_len;
        GPUCMD_Split(&unused, &cmdlist_len);
        GSPGPU_FlushDataCache(cmdlist, cmdlist_len);

        extern u32 __ctru_linear_heap;
        extern u32 __ctru_linear_heap_size;
        GX_FlushCacheRegions(cmdlist, cmdlist_len * 4, (u32 *)__ctru_linear_heap,
                             __ctru_linear_heap_size, NULL, 0);

        if (log_frame) {
            diag_log("RENDER",
                     "frame=%llu eye=%s stage=command_list_process phase=begin",
                     (unsigned long long)frame_number, eye_name);
        }
        GX_ProcessCommandList(cmdlist, cmdlist_len * 4, 2);
        if (log_frame) {
            diag_log("RENDER",
                     "frame=%llu eye=%s stage=command_list_process phase=end result=returned",
                     (unsigned long long)frame_number, eye_name);
        }

        if (log_frame) {
            diag_log("RENDER",
                     "frame=%llu eye=%s stage=p3d_wait phase=begin",
                     (unsigned long long)frame_number, eye_name);
        }
        gspWaitForEvent(GSPGPU_EVENT_P3D, 0);
        if (log_frame) {
            diag_log("RENDER",
                     "frame=%llu eye=%s stage=p3d_wait phase=end result=returned",
                     (unsigned long long)frame_number, eye_name);
        }

        // Copy the complete rendered eye into its matching hardware framebuffer.
        const bool use_right_eye = screen == GFX_TOP && eye == 1;
        const char *framebuffer_side = use_right_eye ? "RIGHT" : "LEFT";
        if (log_frame) {
            diag_log("RENDER",
                     "frame=%llu eye=%s framebuffer_side=%s stage=eye_display_transfer phase=begin",
                     (unsigned long long)frame_number, eye_name,
                     framebuffer_side);
        }
        u32 *dest = (u32 *)gfxGetFramebuffer(
            screen, use_right_eye ? GFX_RIGHT : GFX_LEFT, NULL, NULL);
        GX_DisplayTransfer((u32 *)vramFb,
                           GX_BUFFER_DIM(surface_height, surface_width), dest,
                           GX_BUFFER_DIM(surface_height, surface_width),
                           GX_TRANSFER_OUT_TILED(0) |
                               GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
                               GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
                               GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
        if (log_frame) {
            diag_log("RENDER",
                     "frame=%llu eye=%s framebuffer_side=%s stage=eye_display_transfer phase=end result=returned",
                     (unsigned long long)frame_number, eye_name,
                     framebuffer_side);
        }
        if (log_frame) {
            diag_log("RENDER",
                     "frame=%llu eye=%s stage=post_transfer_ppf_wait phase=begin",
                     (unsigned long long)frame_number, eye_name);
        }
        gspWaitForEvent(GSPGPU_EVENT_PPF, 0);
        if (log_frame) {
            diag_log("RENDER",
                     "frame=%llu eye=%s stage=post_transfer_ppf_wait phase=end result=returned",
                     (unsigned long long)frame_number, eye_name);
            diag_log("RENDER",
                     "frame=%llu eye=%s stage=pass_complete phase=begin",
                     (unsigned long long)frame_number, eye_name);
            diag_log("RENDER",
                     "frame=%llu eye=%s stage=pass_complete phase=end",
                     (unsigned long long)frame_number, eye_name);
        }
    }

    perf_fbcopy_ticks = svcGetSystemTick() - start_ticks;
    if (debug) {
        draw_perf_counters();
    }

    if (screen == GFX_BOTTOM) {
        n3ds_draw_diag_version_overlay();
        n3ds_draw_stream_quit_status(n3ds_quit_hold_elapsed_ms());
        gfxFlushBuffers();
    }

    gfxScreenSwapBuffers(screen, true);

    renderer_frames_completed++;
    if (log_frame) {
        diag_log("RENDER",
                 "frame=%llu phase=exit frames_completed=%llu elapsed_ticks=%llu",
                 (unsigned long long)frame_number,
                 (unsigned long long)renderer_frames_completed,
                 (unsigned long long)(svcGetSystemTick() - frame_start_ticks));
    } else if (renderer_frames_completed % 300 == 0) {
        diag_log("RENDER", "heartbeat frames_completed=%llu",
                 (unsigned long long)renderer_frames_completed);
    }
}

void n3ds_draw_diag_version_overlay() {
    PrintConsole *previous_console =
        consoleSelect(&DebugTouchHandler::bottomScreen);
    DebugTouchHandler::bottomScreen.frameBuffer =
        (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    printf("\x1b[30;1HMoonlight " DIAG_BUILD_VERSION);
    consoleSelect(previous_console);
}

void n3ds_draw_stream_quit_status(unsigned int elapsed_ms) {
    PrintConsole *previous_console =
        consoleSelect(&DebugTouchHandler::bottomScreen);
    DebugTouchHandler::bottomScreen.frameBuffer =
        (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);

    printf("\x1b[28;1H%-40.40s",
           "HOLD L+R+START+SELECT 5s: QUIT TO MENU");
    if (elapsed_ms == 0) {
        printf("\x1b[29;1H%-40.40s", " ");
    } else {
        if (elapsed_ms > 5000) {
            elapsed_ms = 5000;
        }
        char status[41];
        std::snprintf(status, sizeof(status), "Held %1.1fs - menu in %1.1fs",
                      elapsed_ms / 1000.0,
                      (5000 - elapsed_ms) / 1000.0);
        printf("\x1b[29;1H%-40.40s", status);
    }
    consoleSelect(previous_console);
}

static bool fill_and_wait(u32 *first, u32 first_words, u32 *second,
                          u32 second_words, const char *screen_name) {
    if (first == NULL) {
        diag_log("TEARDOWN", "step=Clearing the screens screen=%s result=null",
                 screen_name);
        return false;
    }
    const Result result = GX_MemoryFill(
        first, 0, first + first_words,
        GX_FILL_TRIGGER | GX_FILL_16BIT_DEPTH, second, 0,
        second ? second + second_words : NULL,
        second ? GX_FILL_16BIT_DEPTH : 0);
    if (R_FAILED(result)) {
        diag_log("TEARDOWN",
                 "step=Clearing the screens screen=%s result=%ld",
                 screen_name, (long)result);
        return false;
    }

    diag_log("TEARDOWN",
             "step=Clearing the screens screen=%s event=PSC0 timeout_ms=250 phase=begin",
             screen_name);
    if (!wait_for_psc0_bounded(GPU_CLEAR_WAIT_TIMEOUT_NS)) {
        diag_log("TEARDOWN",
                 "step=Clearing the screens screen=%s event=PSC0 TIMED_OUT",
                 screen_name);
        return false;
    }
    diag_log("TEARDOWN",
             "step=Clearing the screens screen=%s event=PSC0 phase=end",
             screen_name);
    return true;
}

bool n3ds_clear_stream_screens() {
    bool clear_ok = true;
    gfxSetWide(false);
    if (!gfxIs3D()) {
        gfxSet3D(true);
    }

    u32 *bottom = (u32 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    const u32 bottom_words =
        (GSP_SCREEN_HEIGHT_BOTTOM * GSP_SCREEN_WIDTH * 2) / sizeof(u32);
    const bool bottom_ok =
        fill_and_wait(bottom, bottom_words, NULL, 0, "BOTTOM");
    clear_ok = bottom_ok && clear_ok;
    n3ds_draw_diag_version_overlay();
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_BOTTOM, true);

    u32 *left = (u32 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    u32 *right = (u32 *)gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL);
    const u32 top_words =
        (GSP_SCREEN_HEIGHT_TOP * GSP_SCREEN_WIDTH * 2) / sizeof(u32);
    const bool top_ok = fill_and_wait(left, top_words, right, top_words, "TOP");
    clear_ok = top_ok && clear_ok;
    gfxScreenSwapBuffers(GFX_TOP, true);
    return clear_ok;
}

void n3ds_draw_touch_panel_3d_readout(const N3ds3DSettings &settings) {
    char status[41];
    std::snprintf(status, sizeof(status), "3D: %s | %s | SHIFT %+d",
                  n3ds_3d_mode_name(settings.mode),
                  n3ds_3d_aspect_name(settings.aspect), settings.shift);

    PrintConsole *previous_console =
        consoleSelect(&DebugTouchHandler::bottomScreen);
    DebugTouchHandler::bottomScreen.frameBuffer =
        (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    char line[21];
    std::snprintf(line, sizeof(line), "MODE: %.14s",
                  n3ds_3d_mode_name(settings.mode));
    printf("\x1b[20;21H%-20.20s", line);
    std::snprintf(line, sizeof(line), "ASPECT: %.12s",
                  n3ds_3d_aspect_name(settings.aspect));
    printf("\x1b[21;21H%-20.20s", line);
    std::snprintf(line, sizeof(line), "SHIFT: %+d", settings.shift);
    printf("\x1b[22;21H%-20.20s", line);
    printf("\x1b[26;1H%-40.40s", status);
    consoleSelect(previous_console);
}

void n3ds_restore_mono_display() {
    gfxSetWide(false);
    if (!gfxIs3D()) {
        // The screen-clear phase already cleared both eye buffers.
        gfxSet3D(true);
    }
    gfxSet3D(false);
    gfxSetWide(false);
    diag_log("TEARDOWN", "mono_display_swap phase=begin");
    gfxScreenSwapBuffers(GFX_TOP, true);
    diag_log("TEARDOWN", "mono_display_swap phase=end");
    diag_log("RENDER", "stereo_presentation=mono outside_stream=1");
}

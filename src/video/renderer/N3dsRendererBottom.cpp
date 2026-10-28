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

#include <cstdlib>
#include <cstring>
#include <stdbool.h>
#include <stdexcept>
#include <unistd.h>

N3dsRendererBottom::N3dsRendererBottom(int src_width, int src_height,
                                       int px_size, bool debug_in)
    : N3dsRendererBase(GFX_BOTTOM, GSP_SCREEN_HEIGHT_BOTTOM, GSP_SCREEN_WIDTH,
                       src_width, src_height, px_size, debug_in) {}

void N3dsRendererBottom::write_px_to_framebuffer(uint8_t *source) {
    panel_source = nullptr;
    write_px_to_framebuffer_gpu(source);
}

void N3dsRendererBottom::set_quit_hold_progress(unsigned int elapsed_ms) {
    quit_hold_elapsed_ms = elapsed_ms;
    if (panel_source != nullptr) {
        write_px_to_framebuffer_raw(
            panel_source, panel_offset, panel_size,
            panel_is_3d_page ? &panel_settings : nullptr);
        return;
    }

    n3ds_draw_diag_version_overlay();
    n3ds_draw_stream_quit_status(quit_hold_elapsed_ms);
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

void N3dsRendererBottom::write_px_to_framebuffer_raw(const uint8_t *source,
                                                     int offset, int size,
                                                     const N3ds3DSettings *page_settings) {
    panel_source = source;
    panel_offset = offset;
    panel_size = size;
    panel_is_3d_page = page_settings != nullptr;
    if (page_settings != nullptr) {
        panel_settings = *page_settings;
    }
    u8 *gfxbtmadr = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);

    if (size == 0) {
        size = surface_width * surface_height * px_size;
    }
    memcpy(gfxbtmadr + offset, source, size);

    n3ds_draw_diag_version_overlay();
    n3ds_draw_stream_quit_status(quit_hold_elapsed_ms);
    if (page_settings != nullptr) {
        n3ds_draw_touch_panel_3d_readout(*page_settings);
    }
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

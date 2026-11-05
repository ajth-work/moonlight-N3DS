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
#include "N3ds3DPresentation.hpp"

#include <3ds.h>
#include <cstdlib>
#include <cstring>
#include <stdbool.h>
#include <stdexcept>
#include <unistd.h>

N3dsRendererTop::N3dsRendererTop(int dest_width, int dest_height, int src_width,
                                 int src_height, int px_size, bool debug_in)
    : N3dsRendererBase(GFX_TOP, dest_width, dest_height, src_width, src_height,
                       px_size, debug_in) {}

void N3dsRendererTop::write_px_to_framebuffer(uint8_t *source) {
    const N3ds3DSettings settings = n3ds_3d_settings_snapshot();
    if (osGet3DSliderState() > 0.0 &&
        settings.mode != N3ds3DMode::OFF_2D) {
        ensure_3d_enabled();
    } else {
        ensure_3d_disabled();
    }
    write_px_to_framebuffer_gpu(source);
}

void N3dsRendererTop::set_perf_decode_ticks(u64 ticks) {
    perf_decode_ticks = ticks;
}
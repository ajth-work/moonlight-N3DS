/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
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

#include "TouchHandler.hpp"
#include "../../system/dispatcher.hpp"
#include <Limelight.h>
#include <cstring>

static bool is_home_tile(touchPosition touch) {
    return touch.py >= 132 && touch.px >= 112 && touch.px <= 208;
}

GamepadTouchHandler::GamepadTouchHandler(GAMEPAD_STATE *gamepad_in)
    : gamepad_state(gamepad_in) {}

void GamepadTouchHandler::_handle_touch_down(touchPosition touch) {
    home_tile_pressed = is_home_tile(touch);
    if (home_tile_pressed) {
        return;
    }
    if (touch.py >= 120) {
        gamepad_state->buttons |= SPECIAL_FLAG;
        return;
    }

    if (touch.px < 235)
        gamepad_state->buttons |= LS_CLK_FLAG;
    if (touch.px > 104)
        gamepad_state->buttons |= RS_CLK_FLAG;
}

void GamepadTouchHandler::_handle_touch_up(touchPosition touch) {
    if (home_tile_pressed && is_home_tile(touch)) {
        MessageDispatcher::get_instance()->post(
            std::make_shared<GenericEventMsg>(MessageType::EXIT_STREAM));
    }
    home_tile_pressed = false;
    gamepad_state->buttons &= ~(SPECIAL_FLAG | LS_CLK_FLAG | RS_CLK_FLAG);
}

void GamepadTouchHandler::_handle_touch_hold(touchPosition touch) {
    if (home_tile_pressed || is_home_tile(touch)) {
        gamepad_state->buttons &= ~(SPECIAL_FLAG | LS_CLK_FLAG | RS_CLK_FLAG);
        return;
    }
    gamepad_state->buttons &= ~(SPECIAL_FLAG | LS_CLK_FLAG | RS_CLK_FLAG);

    if (touch.py >= 120) {
        gamepad_state->buttons |= SPECIAL_FLAG;
        return;
    }

    if (touch.px < 235)
        gamepad_state->buttons |= LS_CLK_FLAG;
    if (touch.px > 104)
        gamepad_state->buttons |= RS_CLK_FLAG;
}

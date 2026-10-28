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

#include "N3dsTouchscreenInput.hpp"
#include "../../diagnostics.h"
#include "../n3ds_input.hpp"
#include "../../system/dispatcher.hpp"

N3dsTouchscreenInput::N3dsTouchscreenInput(GAMEPAD_STATE *gamepad_in,
                                           int image_width_in,
                                           int image_height_in)
    : gamepad_state(gamepad_in), image_width(image_width_in),
      image_height(image_height_in) {
    MessageDispatcher::get_instance()->subscribe(
        MessageType::TOUCH_STATE_CHANGED, this);
};

N3dsTouchscreenInput::~N3dsTouchscreenInput() {
    MessageDispatcher::get_instance()->unsubscribe(
        MessageType::TOUCH_STATE_CHANGED, this);
}

void N3dsTouchscreenInput::accept(IMessage *msg) {
    if (msg->getMessageType() != MessageType::TOUCH_STATE_CHANGED) {
        return;
    }

    auto touch_msg = static_cast<TouchStateChangedMsg *>(msg);
    next_touch_type.store(touch_msg->ttype);
}

bool N3dsTouchscreenInput::handle_home_press() {
    if (handler != nullptr && handler->handle_home_press()) {
        home_restore_pending = true;
        n3ds_set_home_allowed(false, "home_page_back_3d_panel");
        return true;
    }

    if (touch_type == N3dsTouchType::MENU_TOUCH) {
        return false;
    }

    home_back_pending = true;
    home_restore_pending = true;
    next_touch_type.store(N3dsTouchType::MENU_TOUCH);
    n3ds_set_home_allowed(false, "home_page_back_touch_submenu");
    return true;
}

void N3dsTouchscreenInput::_n3dsinput_set_touch(N3dsTouchType touch_type_in) {
    switch (touch_type_in) {
    case N3dsTouchType::GAMEPAD:
        handler = std::make_unique<GamepadTouchHandler>(gamepad_state);
        break;
    case N3dsTouchType::MOUSEPAD:
        handler = std::make_unique<MouseTouchHandler>();
        break;
    case N3dsTouchType::KEYBOARD:
        handler = std::make_unique<KeyboardTouchHandler>();
        break;
    case N3dsTouchType::ABSOLUTE_TOUCH:
        handler = std::make_unique<MirrorTouchHandler>();
        break;
    case N3dsTouchType::DS_TOUCH:
        handler = std::make_unique<StretchTouchHandler>();
        break;
    case N3dsTouchType::MAGNIFY_TOUCH:
        handler = std::make_unique<MagnifyTouchHandler>(
            gamepad_state, image_width, image_height);
        break;
    case N3dsTouchType::MENU_TOUCH:
        handler = std::make_unique<MenuTouchHandler>();
        break;
    case N3dsTouchType::DEBUG_TOUCH:
        handler = std::make_unique<DebugTouchHandler>();
        break;
    default:
        handler = nullptr;
        break;
    }
    touch_type = touch_type_in;
    const bool allow_home = !home_restore_pending &&
                            (touch_type_in == N3dsTouchType::MENU_TOUCH ||
                             touch_type_in == N3dsTouchType::DISABLED);
    n3ds_set_home_allowed(allow_home, "touch_mode_changed");
    if (home_back_pending && touch_type_in == N3dsTouchType::MENU_TOUCH) {
        home_back_pending = false;
        diag_log("HOME", "page-back taken route=touch-submenu");
    }
}

void N3dsTouchscreenInput::n3dsinput_handle_touch(u32 kDown, u32 kUp) {
    if (next_touch_type.load() != touch_type) {
        _n3dsinput_set_touch(next_touch_type.load());
    }

    // Keep OS HOME disabled through the rejected press. A new touch after the
    // main panel is active proves the page-back press has ended.
    if (home_restore_pending && touch_type == N3dsTouchType::MENU_TOUCH &&
        (kDown & KEY_TOUCH)) {
        home_restore_pending = false;
        n3ds_set_home_allowed(true, "main_panel_touched_after_home_page_back");
    }

    if (handler == nullptr) {
        return;
    }

    touchPosition touch;
    hidTouchRead(&touch);
    if (kDown & KEY_TOUCH) {
        handler->handle_touch_down(touch);
    } else if (kUp & KEY_TOUCH) {
        handler->handle_touch_up(touch);
    } else {
        handler->handle_touch_hold(touch);
    }
}

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
#include "../../system/dispatcher.hpp"
#include "TouchHandler.hpp"
#include "../../video/renderer/N3ds3DPresentation.hpp"
#include "../../diagnostics.h"
#include "../n3ds_input.hpp"
#include <Limelight.h>
#include <memory>
#include <vector>

static const int button_size_y = 60;
static const int button_size_x = 160;

// Map the Row and Column numbers of each button to their corresponding
// Touch Type Setting
static std::map<int, std::map<int, N3dsTouchType>> button_map{
    {0, {{0, N3dsTouchType::GAMEPAD}, {1, N3dsTouchType::MOUSEPAD}}},
    {1, {{1, N3dsTouchType::ABSOLUTE_TOUCH}}},
    {2, {{0, N3dsTouchType::DS_TOUCH}, {1, N3dsTouchType::MAGNIFY_TOUCH}}},
    {3, {{0, N3dsTouchType::DEBUG_TOUCH}, {1, N3dsTouchType::DISABLED}}},
};

MenuTouchHandler::MenuTouchHandler() = default;

MenuTouchHandler::~MenuTouchHandler() = default;

bool MenuTouchHandler::handle_home_press() {
    if (!three_d_page) {
        return false;
    }

    three_d_page = false;
    pending_enter_3d_page = false;
    pending_3d_action = ThreeDPanelAction::NONE;
    MessageDispatcher::get_instance()->post(
        std::make_shared<TouchPanelPageChangedMsg>(false));
    diag_log("HOME", "page-back taken route=3d-panel");
    return true;
}

void MenuTouchHandler::_handle_touch_down(touchPosition touch) {
    _handle_touch_hold(touch);
}

void MenuTouchHandler::_handle_touch_up(touchPosition touch) {
    if (three_d_page) {
        switch (pending_3d_action) {
        case ThreeDPanelAction::MODE:
            n3ds_3d_cycle_mode();
            break;
        case ThreeDPanelAction::ASPECT:
            n3ds_3d_cycle_aspect();
            break;
        case ThreeDPanelAction::SHIFT_MINUS:
            n3ds_3d_adjust_shift(-1);
            break;
        case ThreeDPanelAction::SHIFT_PLUS:
            n3ds_3d_adjust_shift(1);
            break;
        case ThreeDPanelAction::HOME:
            three_d_page = false;
            n3ds_set_home_allowed(true, "3d_panel_home_tile");
            break;
        case ThreeDPanelAction::NONE:
            break;
        }
        if (pending_3d_action != ThreeDPanelAction::NONE) {
            MessageDispatcher::get_instance()->post(
                std::make_shared<TouchPanelPageChangedMsg>(three_d_page));
        }
        pending_3d_action = ThreeDPanelAction::NONE;
        message = nullptr;
        return;
    }

    if (pending_enter_3d_page) {
        three_d_page = true;
        n3ds_set_home_allowed(false, "3d_panel_opened");
        pending_enter_3d_page = false;
        MessageDispatcher::get_instance()->post(
            std::make_shared<TouchPanelPageChangedMsg>(true));
        message = nullptr;
        return;
    }

    if (message != nullptr)
        MessageDispatcher::get_instance()->post(message);
    message = nullptr;
}

void MenuTouchHandler::_handle_touch_hold(touchPosition touch) {
    if (touch.py >= 240) {
        touch.py = 239;
    }
    if (touch.px >= 320) {
        touch.px = 319;
    }
    int round_y = touch.py / button_size_y;
    int round_x = touch.px / button_size_x;

    if (three_d_page) {
        if (round_y == 0 && round_x == 0) {
            pending_3d_action = ThreeDPanelAction::MODE;
        } else if (round_y == 0 && round_x == 1) {
            pending_3d_action = ThreeDPanelAction::ASPECT;
        } else if (round_y == 1 && round_x == 0) {
            pending_3d_action = ThreeDPanelAction::SHIFT_MINUS;
        } else if (round_y == 1 && round_x == 1) {
            pending_3d_action = ThreeDPanelAction::SHIFT_PLUS;
        } else if (round_y == 2 && round_x == 0) {
            pending_3d_action = ThreeDPanelAction::HOME;
        } else {
            pending_3d_action = ThreeDPanelAction::NONE;
        }
        message = nullptr;
        return;
    }

    if (round_y == 1 && round_x == 0) {
        pending_enter_3d_page = true;
        message = nullptr;
        return;
    }

    pending_enter_3d_page = false;
    N3dsTouchType touch_type = button_map[round_y][round_x];
    switch (touch_type) {
    case (N3dsTouchType::GAMEPAD):
        message = std::make_shared<TouchStateChangedMsg>(touch_type);
        break;
    case (N3dsTouchType::MOUSEPAD):
        message = std::make_shared<TouchStateChangedMsg>(touch_type);
        break;
    case (N3dsTouchType::DISABLED):
        // Signal to exit the stream
        message = std::make_shared<GenericEventMsg>(MessageType::EXIT_STREAM);
        break;
    default:
        message = std::make_shared<TouchStateChangedMsg>(touch_type);
        break;
    }
}

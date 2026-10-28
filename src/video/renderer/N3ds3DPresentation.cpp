#include "N3ds3DPresentation.hpp"

#include "../../system/AtomicVar.hpp"

#include <algorithm>

namespace {
constexpr float SCREEN_ASPECT = 400.0f / 240.0f;
constexpr int MAX_HORIZONTAL_SHIFT = 8;

AtomicVar<int> current_mode(static_cast<int>(N3ds3DMode::MONO_DUP));
AtomicVar<int> current_aspect(static_cast<int>(N3ds3DAspect::STRETCH));
AtomicVar<int> current_shift(-8);

bool is_split_mode(N3ds3DMode mode) {
    return mode == N3ds3DMode::HSBS || mode == N3ds3DMode::HSBS_SWAP ||
           mode == N3ds3DMode::OU || mode == N3ds3DMode::OU_SWAP;
}
} // namespace

N3ds3DSettings n3ds_3d_settings_snapshot() {
    return {static_cast<N3ds3DMode>(current_mode.load()),
            static_cast<N3ds3DAspect>(current_aspect.load()),
            current_shift.load()};
}

void n3ds_3d_reset_settings() {
    current_mode.store(static_cast<int>(N3ds3DMode::MONO_DUP));
    current_aspect.store(static_cast<int>(N3ds3DAspect::STRETCH));
    current_shift.store(-8);
}

void n3ds_3d_cycle_mode() {
    current_mode.store((current_mode.load() + 1) % 6);
}

void n3ds_3d_cycle_aspect() {
    current_aspect.store((current_aspect.load() + 1) % 3);
}

void n3ds_3d_adjust_shift(int delta) {
    current_shift.store(
        std::max(-MAX_HORIZONTAL_SHIFT,
                 std::min(MAX_HORIZONTAL_SHIFT, current_shift.load() + delta)));
}

const char *n3ds_3d_mode_name(N3ds3DMode mode) {
    switch (mode) {
    case N3ds3DMode::OFF_2D:
        return "OFF/2D";
    case N3ds3DMode::HSBS:
        return "HSBS";
    case N3ds3DMode::HSBS_SWAP:
        return "HSBS SWAP";
    case N3ds3DMode::MONO_DUP:
        return "MONO DUP";
    case N3ds3DMode::OU:
        return "OU";
    case N3ds3DMode::OU_SWAP:
        return "OU SWAP";
    }
    return "OFF/2D";
}

const char *n3ds_3d_aspect_name(N3ds3DAspect aspect) {
    switch (aspect) {
    case N3ds3DAspect::STRETCH:
        return "STRETCH";
    case N3ds3DAspect::FIT:
        return "FIT";
    case N3ds3DAspect::CROP:
        return "CROP";
    }
    return "STRETCH";
}

N3ds3DEyePresentation n3ds_3d_eye_presentation(
    const N3ds3DSettings &settings, int image_width, int image_height,
    int eye_index) {
    int left = 0;
    int right = image_width;
    int top = 0;
    int bottom = image_height;

    switch (settings.mode) {
    case N3ds3DMode::HSBS:
    case N3ds3DMode::HSBS_SWAP: {
        const bool use_right_half =
            (eye_index == 0) == (settings.mode == N3ds3DMode::HSBS_SWAP);
        const int midpoint = image_width / 2;
        left = use_right_half ? midpoint : 0;
        right = use_right_half ? image_width : midpoint;
        break;
    }
    case N3ds3DMode::OU:
    case N3ds3DMode::OU_SWAP: {
        const bool use_bottom_half =
            (eye_index == 1) != (settings.mode == N3ds3DMode::OU_SWAP);
        const int midpoint = image_height / 2;
        top = use_bottom_half ? midpoint : 0;
        bottom = use_bottom_half ? image_height : midpoint;
        break;
    }
    case N3ds3DMode::OFF_2D:
    case N3ds3DMode::MONO_DUP:
        break;
    }

    int region_width = right - left;
    int region_height = bottom - top;
    float vertical_scale = 1.0f;
    float horizontal_scale = 1.0f;

    if (is_split_mode(settings.mode) && region_width > 0 &&
        region_height > 0) {
        const float region_aspect =
            static_cast<float>(region_width) / region_height;
        if (settings.aspect == N3ds3DAspect::FIT) {
            if (region_aspect > SCREEN_ASPECT) {
                vertical_scale = SCREEN_ASPECT / region_aspect;
            } else {
                horizontal_scale = region_aspect / SCREEN_ASPECT;
            }
        } else if (settings.aspect == N3ds3DAspect::CROP) {
            if (region_aspect > SCREEN_ASPECT) {
                const float cropped_width = region_height * SCREEN_ASPECT;
                const float inset = (region_width - cropped_width) / 2.0f;
                left += static_cast<int>(inset);
                right -= static_cast<int>(inset);
            } else {
                const float cropped_height = region_width / SCREEN_ASPECT;
                const float inset = (region_height - cropped_height) / 2.0f;
                top += static_cast<int>(inset);
                bottom -= static_cast<int>(inset);
            }
        }
    }

    const int eye_shift = eye_index == 0 ? -settings.shift : settings.shift;
    return {static_cast<float>(left) / 1024.0f,
            static_cast<float>(right) / 1024.0f,
            static_cast<float>(top) / 512.0f,
            static_cast<float>(bottom) / 512.0f,
            vertical_scale,
            horizontal_scale,
            (2.0f * eye_shift) / 400.0f};
}

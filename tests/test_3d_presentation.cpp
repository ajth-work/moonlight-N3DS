#include "../src/video/renderer/N3ds3DPresentation.hpp"

#include <cassert>
#include <cmath>
#include <cstring>

static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

int main() {
    N3ds3DSettings settings{N3ds3DMode::HSBS, N3ds3DAspect::STRETCH, 0};
    const auto left = n3ds_3d_eye_presentation(settings, 1024, 512, 0);
    const auto right = n3ds_3d_eye_presentation(settings, 1024, 512, 1);
    assert(near(left.u0, 0.0f) && near(left.u1, 0.5f));
    assert(near(right.u0, 0.5f) && near(right.u1, 1.0f));
    assert(near(left.v0, 0.0f) && near(left.v1, 1.0f));

    settings.mode = N3ds3DMode::HSBS_SWAP;
    const auto swapped_left = n3ds_3d_eye_presentation(settings, 1024, 512, 0);
    const auto swapped_right = n3ds_3d_eye_presentation(settings, 1024, 512, 1);
    assert(near(swapped_left.u0, 0.5f) && near(swapped_left.u1, 1.0f));
    assert(near(swapped_right.u0, 0.0f) && near(swapped_right.u1, 0.5f));

    settings.mode = N3ds3DMode::OU;
    const auto upper_left = n3ds_3d_eye_presentation(settings, 1024, 512, 0);
    const auto lower_right = n3ds_3d_eye_presentation(settings, 1024, 512, 1);
    assert(near(upper_left.v0, 0.0f) && near(upper_left.v1, 0.5f));
    assert(near(lower_right.v0, 0.5f) && near(lower_right.v1, 1.0f));

    settings.mode = N3ds3DMode::MONO_DUP;
    const auto mono_left = n3ds_3d_eye_presentation(settings, 1024, 512, 0);
    const auto mono_right = n3ds_3d_eye_presentation(settings, 1024, 512, 1);
    assert(near(mono_left.u0, 0.0f) && near(mono_left.u1, 1.0f));
    assert(near(mono_right.u0, 0.0f) && near(mono_right.u1, 1.0f));

    settings.mode = N3ds3DMode::HSBS;
    settings.aspect = N3ds3DAspect::FIT;
    const auto fitted = n3ds_3d_eye_presentation(settings, 1024, 512, 0);
    assert(near(fitted.horizontal_scale, 0.6f));
    assert(near(fitted.vertical_scale, 1.0f));

    settings.aspect = N3ds3DAspect::CROP;
    const auto cropped = n3ds_3d_eye_presentation(settings, 1024, 512, 0);
    assert(cropped.v0 > 0.18f && cropped.v0 < 0.22f);
    assert(cropped.v1 > 0.78f && cropped.v1 < 0.82f);

    const N3ds3DSettings defaults = n3ds_3d_settings_snapshot();
    assert(defaults.mode == N3ds3DMode::OFF_2D);
    assert(defaults.aspect == N3ds3DAspect::FIT);
    assert(defaults.shift == 0);
    n3ds_3d_adjust_shift(100);
    assert(n3ds_3d_settings_snapshot().shift == 8);
    n3ds_3d_adjust_shift(-100);
    assert(n3ds_3d_settings_snapshot().shift == -8);
    assert(std::strcmp(n3ds_3d_mode_name(N3ds3DMode::HSBS), "HSBS") == 0);
    return 0;
}

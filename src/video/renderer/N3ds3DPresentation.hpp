#pragma once

enum class N3ds3DMode : int {
    OFF_2D = 0,
    HSBS,
    HSBS_SWAP,
    MONO_DUP,
    OU,
    OU_SWAP,
};

enum class N3ds3DAspect : int {
    STRETCH = 0,
    FIT,
    CROP,
};

struct N3ds3DSettings {
    N3ds3DMode mode;
    N3ds3DAspect aspect;
    int shift;
};

struct N3ds3DEyePresentation {
    float u0;
    float u1;
    float v0;
    float v1;
    float vertical_scale;
    float horizontal_scale;
    float horizontal_offset;
};

N3ds3DSettings n3ds_3d_settings_snapshot();
void n3ds_3d_cycle_mode();
void n3ds_3d_cycle_aspect();
void n3ds_3d_adjust_shift(int delta);
bool n3ds_3d_handle_touch(int x, int y);
const char *n3ds_3d_mode_name(N3ds3DMode mode);
const char *n3ds_3d_aspect_name(N3ds3DAspect aspect);
N3ds3DEyePresentation n3ds_3d_eye_presentation(
    const N3ds3DSettings &settings, int image_width, int image_height,
    int eye_index);

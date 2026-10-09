#pragma once

#include <cstddef>

namespace tobii::manager {

struct HeadViewState {
    bool tracked = false;
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    float translation_mm[3] = {0.0f, 0.0f, 0.0f};
    bool gaze_valid = false;
    float gaze_x = 0.5f, gaze_y = 0.5f;
    bool left_eye_valid = true, right_eye_valid = true;

    unsigned int camera_texture = 0;
    const float* landmarks = nullptr;
    std::size_t landmark_count = 0;
    int frame_width = 0;
};

void DrawHead(const HeadViewState& state, float size);

}

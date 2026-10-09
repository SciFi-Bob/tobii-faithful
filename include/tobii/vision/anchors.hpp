// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: The MediaPipe Authors
// SPDX-FileComment: Based on MediaPipe's SSD anchors calculator (mediapipe/calculators/tflite/ssd_anchors_calculator); modified for this project.
#pragma once

#include <cstddef>
#include <vector>

namespace tobii::vision {

struct Anchor {
    float x_center;
    float y_center;
    float width;
    float height;
};

struct AnchorConfig {
    int   input_width   = 128;
    int   input_height  = 128;
    int   num_layers    = 4;
    float min_scale     = 0.1484375f;
    float max_scale     = 0.75f;
    float anchor_offset_x = 0.5f;
    float anchor_offset_y = 0.5f;
    float interpolated_scale_aspect_ratio = 1.0f;
    bool  fixed_anchor_size = true;
    std::vector<int>   strides       = {8, 16, 16, 16};
    std::vector<float> aspect_ratios = {1.0f};
};

std::vector<Anchor> GenerateAnchors(const AnchorConfig& cfg = {});

}

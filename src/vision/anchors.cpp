// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: The MediaPipe Authors
// SPDX-FileComment: Based on MediaPipe's SSD anchors calculator (mediapipe/calculators/tflite/ssd_anchors_calculator); modified for this project.
#include "tobii/vision/anchors.hpp"

#include <cmath>

namespace tobii::vision {

std::vector<Anchor> GenerateAnchors(const AnchorConfig& cfg) {
    std::vector<Anchor> anchors;

    int layer_id = 0;
    while (layer_id < cfg.num_layers) {
        std::vector<float> anchor_height;
        std::vector<float> anchor_width;
        std::vector<float> aspect_ratios;
        std::vector<float> scales;

        int last_same_stride_layer = layer_id;
        while (last_same_stride_layer < cfg.num_layers &&
               cfg.strides[static_cast<std::size_t>(last_same_stride_layer)] ==
                   cfg.strides[static_cast<std::size_t>(layer_id)]) {
            const float scale =
                cfg.min_scale + (cfg.max_scale - cfg.min_scale) *
                                    static_cast<float>(last_same_stride_layer) /
                                    static_cast<float>(cfg.num_layers - 1);

            for (const float ratio : cfg.aspect_ratios) {
                aspect_ratios.push_back(ratio);
                scales.push_back(scale);
            }
            if (cfg.interpolated_scale_aspect_ratio > 0.0f) {
                const float scale_next =
                    (last_same_stride_layer == cfg.num_layers - 1)
                        ? 1.0f
                        : cfg.min_scale + (cfg.max_scale - cfg.min_scale) *
                                              static_cast<float>(last_same_stride_layer + 1) /
                                              static_cast<float>(cfg.num_layers - 1);
                scales.push_back(std::sqrt(scale * scale_next));
                aspect_ratios.push_back(cfg.interpolated_scale_aspect_ratio);
            }
            ++last_same_stride_layer;
        }

        for (std::size_t i = 0; i < aspect_ratios.size(); ++i) {
            const float ratio_sqrt = std::sqrt(aspect_ratios[i]);
            anchor_height.push_back(scales[i] / ratio_sqrt);
            anchor_width.push_back(scales[i] * ratio_sqrt);
        }

        const int stride = cfg.strides[static_cast<std::size_t>(layer_id)];
        const int feature_map_height =
            static_cast<int>(std::ceil(static_cast<float>(cfg.input_height) / static_cast<float>(stride)));
        const int feature_map_width =
            static_cast<int>(std::ceil(static_cast<float>(cfg.input_width) / static_cast<float>(stride)));

        for (int y = 0; y < feature_map_height; ++y) {
            for (int x = 0; x < feature_map_width; ++x) {
                for (std::size_t anchor_id = 0; anchor_id < anchor_height.size(); ++anchor_id) {
                    Anchor a{};
                    a.x_center = (static_cast<float>(x) + cfg.anchor_offset_x) /
                                 static_cast<float>(feature_map_width);
                    a.y_center = (static_cast<float>(y) + cfg.anchor_offset_y) /
                                 static_cast<float>(feature_map_height);
                    if (cfg.fixed_anchor_size) {
                        a.width  = 1.0f;
                        a.height = 1.0f;
                    } else {
                        a.width  = anchor_width[anchor_id];
                        a.height = anchor_height[anchor_id];
                    }
                    anchors.push_back(a);
                }
            }
        }
        layer_id = last_same_stride_layer;
    }
    return anchors;
}

}

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tobii::usb {

struct ImageFrame {
    std::uint32_t object_id = 0;
    std::uint64_t timestamp = 0;
    std::uint32_t bpp    = 0;
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::vector<std::uint8_t> pixels;
};

struct GazeSample {
    bool has_timestamp = false;
    bool has_frame_counter = false;
    bool has_gaze = false;
    bool has_gaze_unfiltered = false;
    bool has_left_origin = false;
    bool has_right_origin = false;

    std::int64_t  timestamp_us = 0;
    std::uint32_t frame_counter = 0;

    double gaze_x = 0.0, gaze_y = 0.0;
    double gaze_unfiltered_x = 0.0, gaze_unfiltered_y = 0.0;
    double eye_left_x = 0.0,  eye_left_y = 0.0,  eye_left_z = 0.0;
    double eye_right_x = 0.0, eye_right_y = 0.0, eye_right_z = 0.0;

    std::uint32_t combined_valid = 0xffffffffu;
    std::uint32_t gaze_valid = 0xffffffffu;
    std::uint32_t validity_left = 0xffffffffu;
    std::uint32_t validity_right = 0xffffffffu;
    std::uint32_t eye_present_left = 0xffffffffu;
    std::uint32_t eye_present_right = 0xffffffffu;

    bool LeftEyeValid() const { return has_left_origin && validity_left == 0; }
    bool RightEyeValid() const { return has_right_origin && validity_right == 0; }

    bool GazeIsValid() const {
        if (!has_gaze) return false;
        if (combined_valid != 0xffffffffu) return combined_valid == 1;
        return LeftEyeValid() || RightEyeValid();
    }
    bool FaceIsPresent() const {
        return eye_present_left == 1 || eye_present_right == 1 ||
               validity_left == 1 || validity_right == 1;
    }
};

bool DecodeImagePayload(std::uint32_t object_id, const std::uint8_t* payload,
                        std::uint32_t len, ImageFrame& out);

bool DecodeGazePayload(const std::uint8_t* payload, std::uint32_t len, GazeSample& out);

}

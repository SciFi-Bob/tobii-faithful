#pragma once

#include "tobii/calibration/profile.hpp"
#include "tobii/pose/head_pose.hpp"
#include "tobii/usb/decode.hpp"
#include "tobii/vision/tracker.hpp"

#include <opencv2/core.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tobii {

struct EyeMidpoint {
    double x = 0.0, y = 0.0, z = 0.0;
    bool valid = false;
};
EyeMidpoint HeadMidpointFromEyes(const double left[3], bool left_ok,
                                 const double right[3], bool right_ok,
                                 double tracker_pitch_deg);

class Pipeline {
public:
    Pipeline(const std::string& detector_model, const std::string& landmark_model);

    bool OnImage(const usb::ImageFrame& frame, double now_s);

    void OnGaze(const usb::GazeSample& sample);

    void Reset();

    bool face_tracked() const { return face_tracked_; }

    bool face_present(double now_s) const {
        return last_tracked_s_ > 0.0 && (now_s - last_tracked_s_) <= presence_hold_s_;
    }

    double presence_hold_s() const { return presence_hold_s_; }
    void set_presence_hold_s(double seconds) { presence_hold_s_ = seconds; }

    const std::vector<cv::Point3f>& landmarks() const { return landmarks_; }

    const cv::Mat& last_frame() const { return last_frame_; }
    float presence() const { return presence_; }

    float distance_mm() const { return distance_mm_; }

    struct EyeGeometry {
        double left[3]{}, right[3]{};
        std::uint32_t validity_left = 0xffffffffu, validity_right = 0xffffffffu;
        std::uint32_t present_left = 0xffffffffu, present_right = 0xffffffffu;
        bool has_left = false, has_right = false;
        std::int64_t timestamp_us = 0;
    };
    const EyeGeometry& eyes() const { return eyes_; }

    const pose::HeadPoseSample& official_pose() const { return official_sample_; }

    double eye_distance_mm() const {
        const bool l = eyes_.validity_left == 0;
        const bool r = eyes_.validity_right == 0;
        if (l && r) return (eyes_.left[2] + eyes_.right[2]) * 0.5;
        if (l) return eyes_.left[2];
        if (r) return eyes_.right[2];
        return 0.0;
    }

    void raw_gaze(float& x, float& y, bool& valid) const {
        x = raw_gaze_x_; y = raw_gaze_y_; valid = raw_gaze_valid_;
    }

    const calibration::Profile& profile() const { return profile_; }

    void set_profile(const calibration::Profile& profile);

private:
    vision::FaceTracker tracker_;
    pose::OfficialHeadPose official_{pose::ScreenFrame{}};
    pose::HeadPoseSample official_sample_{};
    bool face_tracked_ = false;
    float presence_ = 0.0f;
    float distance_mm_ = 0.0f;
    double last_tracked_s_ = 0.0;
    float raw_gaze_x_ = 0.5f, raw_gaze_y_ = 0.5f;
    bool raw_gaze_valid_ = false;
    EyeGeometry eyes_{};
    double presence_hold_s_ = 0.75;
    calibration::Profile profile_{};
    std::vector<cv::Point3f> landmarks_;
    cv::Mat last_frame_;
};

}

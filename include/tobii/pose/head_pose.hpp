#pragma once

#include "tobii/pose/screen_frame.hpp"

#include <Eigen/Dense>
#include <cstdint>

namespace tobii::pose {

struct HeadPoseSample {
    std::uint64_t ts_us = 0;
    double pos[3] = {0.0, 0.0, 0.0};
    double rot[3] = {0.0, 0.0, 0.0};
    bool pos_valid = false;
    bool rot_valid[3] = {false, false, false};
};

struct HeadPoseInput {
    std::uint64_t ts_us = 0;
    bool has_mesh = false;
    Eigen::Matrix3d mesh_rotation = Eigen::Matrix3d::Identity();
    bool has_left = false;
    bool has_right = false;
    Eigen::Vector3d left_eye = Eigen::Vector3d::Zero();
    Eigen::Vector3d right_eye = Eigen::Vector3d::Zero();
};

struct OfficialHeadPoseConfig {
    double z_min_mm = 250.0;
    double z_max_mm = 1200.0;
};

class OfficialHeadPose {
public:
    explicit OfficialHeadPose(ScreenFrame frame, OfficialHeadPoseConfig cfg = {})
        : frame_(frame), cfg_(cfg) {}

    HeadPoseSample Update(const HeadPoseInput& in);

    void set_screen_frame(ScreenFrame frame) { frame_ = frame; }
    void Reset() { have_sep_ = false; }

private:
    ScreenFrame frame_;
    OfficialHeadPoseConfig cfg_;
    Eigen::Vector3d half_sep_ = Eigen::Vector3d::Zero();
    bool have_sep_ = false;
};

}

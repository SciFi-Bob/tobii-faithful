#include "tobii/pose/head_pose.hpp"

#include "tobii/pose/euler.hpp"

namespace tobii::pose {
namespace {

constexpr double kSepEma = 0.15;

const Eigen::Matrix3d& MeshToDevice() {
    static const Eigen::Matrix3d s = [] {
        Eigen::Matrix3d m = Eigen::Matrix3d::Identity();
        m(0, 0) = -1.0;
        m(2, 2) = -1.0;
        return m;
    }();
    return s;
}

}

HeadPoseSample OfficialHeadPose::Update(const HeadPoseInput& in) {
    HeadPoseSample out;
    out.ts_us = in.ts_us;

    Eigen::Vector3d mid = Eigen::Vector3d::Zero();
    bool pos_valid = false;
    if (in.has_left && in.has_right) {
        mid = 0.5 * (in.left_eye + in.right_eye);
        const Eigen::Vector3d sep = 0.5 * (in.left_eye - in.right_eye);
        half_sep_ = have_sep_ ? (1.0 - kSepEma) * half_sep_ + kSepEma * sep : sep;
        have_sep_ = true;
        pos_valid = true;
    } else if (in.has_left && have_sep_) {
        mid = in.left_eye - half_sep_;
        pos_valid = true;
    } else if (in.has_right && have_sep_) {
        mid = in.right_eye + half_sep_;
        pos_valid = true;
    }

    if (pos_valid) {
        const Eigen::Vector3d screen_pos = frame_.ToScreen(mid);
        if (screen_pos.z() < cfg_.z_min_mm || screen_pos.z() > cfg_.z_max_mm) {
            pos_valid = false;
        } else {
            out.pos[0] = screen_pos.x();
            out.pos[1] = screen_pos.y();
            out.pos[2] = screen_pos.z();
        }
    }
    out.pos_valid = pos_valid;

    if (in.has_mesh) {
        const Eigen::Matrix3d r_sh =
            frame_.R_sd * MeshToDevice() * in.mesh_rotation * MeshToDevice();
        const Euler e = EulerYXZ(r_sh);
        out.rot[0] = e.pitch;
        out.rot[1] = e.yaw;
        out.rot[2] = e.roll;
        out.rot_valid[0] = out.rot_valid[1] = out.rot_valid[2] = true;
    }

    return out;
}

}

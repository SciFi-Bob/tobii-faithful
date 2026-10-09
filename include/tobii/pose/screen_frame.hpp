#pragma once

#include <Eigen/Dense>

namespace tobii::pose {

struct ScreenFrame {
    Eigen::Matrix3d R_sd = Eigen::Matrix3d::Identity();
    Eigen::Vector3d centre = Eigen::Vector3d::Zero();

    static ScreenFrame FromCorners(const Eigen::Vector3d& tl,
                                   const Eigen::Vector3d& tr,
                                   const Eigen::Vector3d& bl) {
        ScreenFrame f;
        const Eigen::Vector3d ex = (tr - tl).normalized();
        const Eigen::Vector3d up = tl - bl;
        Eigen::Vector3d ey = up - up.dot(ex) * ex;
        ey.normalize();
        const Eigen::Vector3d ez = ex.cross(ey);
        f.R_sd.row(0) = ex;
        f.R_sd.row(1) = ey;
        f.R_sd.row(2) = ez;
        f.centre = 0.5 * (tr + bl);
        return f;
    }

    Eigen::Vector3d ToScreen(const Eigen::Vector3d& p_device) const {
        return R_sd * (p_device - centre);
    }
};

}

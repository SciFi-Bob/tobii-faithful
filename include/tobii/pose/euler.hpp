#pragma once

#include <Eigen/Dense>
#include <cmath>

namespace tobii::pose {

struct Euler {
    double yaw = 0.0;
    double pitch = 0.0;
    double roll = 0.0;
};

inline Euler EulerYXZ(const Eigen::Matrix3d& r) {
    Euler e;
    e.yaw = std::atan2(r(0, 2), r(2, 2));
    e.pitch = std::atan2(-r(1, 2), std::hypot(r(0, 2), r(2, 2)));
    e.roll = std::atan2(r(1, 0), r(1, 1));
    return e;
}

}

#pragma once

#include "tobii/device/calibration_layout.hpp"

#include <vector>

namespace tobii::manager {

struct CalAction {
    enum class Kind { kStart, kClear, kCollect, kDiscard, kCompute, kStop, kSave };
    Kind kind;
    double x = 0.0;
    double y = 0.0;
};

class DeviceCalibrationController {
public:
    explicit DeviceCalibrationController(std::vector<device::CalibrationTarget> targets)
        : targets_(std::move(targets)) {}

    std::vector<CalAction> Begin();

    bool begun() const { return begun_; }
    bool done() const { return done_; }
    bool active() const { return begun_ && !done_; }

    int target_count() const { return static_cast<int>(targets_.size()); }
    int current_index() const { return index_; }
    device::CalibrationTarget current() const { return targets_[static_cast<std::size_t>(index_)]; }

    std::vector<CalAction> Capture();

    std::vector<CalAction> Discard();

private:
    bool ClosesStage(int i) const {
        return i + 1 >= target_count() ||
               targets_[static_cast<std::size_t>(i)].stage !=
                   targets_[static_cast<std::size_t>(i + 1)].stage;
    }

    std::vector<device::CalibrationTarget> targets_;
    int index_ = 0;
    bool begun_ = false;
    bool done_ = false;
};

}

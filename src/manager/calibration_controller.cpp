#include "tobii/manager/calibration_controller.hpp"

namespace tobii::manager {

std::vector<CalAction> DeviceCalibrationController::Begin() {
    begun_ = true;
    done_ = targets_.empty();
    index_ = 0;
    return {{CalAction::Kind::kStart}, {CalAction::Kind::kClear}};
}

std::vector<CalAction> DeviceCalibrationController::Capture() {
    std::vector<CalAction> out;
    if (!active()) return out;

    const auto& t = targets_[static_cast<std::size_t>(index_)];
    out.push_back({CalAction::Kind::kCollect, t.x, t.y});
    if (ClosesStage(index_)) out.push_back({CalAction::Kind::kCompute});

    ++index_;
    if (index_ >= target_count()) {
        out.push_back({CalAction::Kind::kStop});
        out.push_back({CalAction::Kind::kSave});
        done_ = true;
    }
    return out;
}

std::vector<CalAction> DeviceCalibrationController::Discard() {
    if (!active()) return {};
    const auto& t = targets_[static_cast<std::size_t>(index_)];
    return {{CalAction::Kind::kDiscard, t.x, t.y}};
}

}

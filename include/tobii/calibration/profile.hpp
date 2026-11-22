#pragma once

#include "tobii/pose_solve.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tobii::calibration {

enum Axis : std::size_t { kX = 0, kY, kZ, kYaw, kPitch, kRoll, kAxisCount };

inline constexpr float kTrackerMarkSpacingMm = 185.0f;

inline constexpr float kTrackerLengthMm = 285.0f;

inline constexpr float kTrackerBelowScreenMm = 15.0f;

inline constexpr float kScreenBehindTrackerMm = -9.0f;

struct DisplayArea {
    bool measured = false;
    float width_mm  = 600.0f;
    float height_mm = 340.0f;
    float offset_x_mm = 0.0f;
    float offset_y_mm = 190.0f;
    float offset_z_mm = 0.0f;
    std::int32_t pixel_width  = 1920;
    std::int32_t pixel_height = 1080;
    std::int32_t monitor_index = 0;
    void ScreenCorners(double top_left[3], double top_right[3],
                       double bottom_left[3]) const;

    bool SetFromMarkAlignment(float left_fraction, float right_fraction,
                              int pixel_w, int pixel_h);

    float tracker_drop_mm = kTrackerBelowScreenMm;
    bool drop_measured = false;

    float tracker_pitch_deg = 10.26f;

    void SetTrackerDrop(float drop_mm);

    std::string monitor_name;

    bool monitor_chosen() const { return !monitor_name.empty(); }
};

enum class TrackedEyes { kLeft = 1, kRight = 2, kBoth = 3 };

inline const char* TrackedEyesName(TrackedEyes e) {
    return e == TrackedEyes::kLeft ? "left" : e == TrackedEyes::kRight ? "right" : "both";
}

inline bool ParseTrackedEyes(const std::string& name, TrackedEyes& out) {
    if (name == "left") { out = TrackedEyes::kLeft; return true; }
    if (name == "right") { out = TrackedEyes::kRight; return true; }
    if (name == "both") { out = TrackedEyes::kBoth; return true; }
    return false;
}

struct Profile {
    DisplayArea display;
    TrackedEyes eyes = TrackedEyes::kBoth;
    static constexpr PoseSolve kDefaultPoseSolve = PoseSolve::kRigid;
    PoseSolve pose_solve = kDefaultPoseSolve;

    bool Load(const std::string& path);
    bool Save(const std::string& path) const;

    static std::string DefaultPath();
};

}

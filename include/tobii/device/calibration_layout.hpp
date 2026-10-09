#pragma once

#include <vector>

namespace tobii::device {

struct ScreenSizeMm {
    double width = 0.0;
    double height = 0.0;
};

struct CalibrationTarget {
    double x = 0.0;
    double y = 0.0;
    int stage = 0;
};

inline constexpr double kDesignSpan = 0.8;
inline constexpr double kRegionWidthMm = 600.0;
inline constexpr double kRegionHeightMm = 340.0;
inline constexpr double kRescaleDiagonalMm = 700.0;
inline constexpr double kRescaleHeightMm = 360.0;

bool LargeScreenRescale(ScreenSizeMm screen);

std::vector<CalibrationTarget> CalibrationTargets(ScreenSizeMm screen);

}

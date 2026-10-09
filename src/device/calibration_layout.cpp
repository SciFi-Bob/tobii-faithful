#include "tobii/device/calibration_layout.hpp"

#include <algorithm>
#include <cmath>

namespace tobii::device {

bool LargeScreenRescale(ScreenSizeMm screen) {
    if (screen.width <= 0.0 || screen.height <= 0.0) return false;
    const double diagonal = std::hypot(screen.width, screen.height);
    return diagonal >= kRescaleDiagonalMm || screen.height >= kRescaleHeightMm;
}

std::vector<CalibrationTarget> CalibrationTargets(ScreenSizeMm screen) {
    double span_x = kDesignSpan;
    double span_y = kDesignSpan;
    if (LargeScreenRescale(screen)) {
        span_x = kDesignSpan * std::min(1.0, kRegionWidthMm / screen.width);
        span_y = kDesignSpan * std::min(1.0, kRegionHeightMm / screen.height);
    }

    const double lo_x = 0.5 - span_x / 2.0, hi_x = 0.5 + span_x / 2.0;
    const double lo_y = 0.5 - span_y / 2.0, hi_y = 0.5 + span_y / 2.0;

    return {
        {0.5, 0.5, 0},
        {0.5, lo_y, 1}, {lo_x, hi_y, 1}, {hi_x, hi_y, 1},
        {0.5, hi_y, 2}, {lo_x, lo_y, 2}, {hi_x, lo_y, 2},
    };
}

}

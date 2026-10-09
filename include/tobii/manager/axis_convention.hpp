#pragma once

#include <cstddef>

namespace tobii::manager {

struct AxisConvention {
    const char* name;
    const char* positive_means;
    bool mirror_for_display;
};

inline constexpr AxisConvention kAxes[6] = {
    {"Lean left / right",     "lean to your left",    true},
    {"Rise / crouch",         "sit up taller",        false},
    {"Lean forward / back",   "lean toward the screen", false},
    {"Turn head left / right", "turn to your left",   true},
    {"Look up / down",        "tilt your head down",  false},
    {"Tilt head (ear to shoulder)", "tilt your head right", true},
};

inline constexpr std::size_t kAxisCount = 6;

inline constexpr float DisplaySign(std::size_t axis) {
    return axis < kAxisCount && kAxes[axis].mirror_for_display ? -1.0f : 1.0f;
}

}

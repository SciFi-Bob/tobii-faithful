#pragma once

#include <cstdint>
#include <vector>

namespace tobii::tray {

enum class EyeState {
    kNoDaemon,
    kSleeping,
    kWatching,
};

std::vector<std::uint8_t> RenderEye(int size, EyeState state);

}

#pragma once

#include <string>

namespace tobii {

enum class PoseSolve {
    kMediaPipe,
    kRigid,
};

inline const char* PoseSolveName(PoseSolve s) {
    return s == PoseSolve::kRigid ? "rigid" : "mediapipe";
}

inline bool ParsePoseSolve(const std::string& name, PoseSolve& out) {
    if (name == "rigid") { out = PoseSolve::kRigid; return true; }
    if (name == "mediapipe") { out = PoseSolve::kMediaPipe; return true; }
    return false;
}

}

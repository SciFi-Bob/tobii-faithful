#include "tobii/manager/head_view.hpp"

#include "imgui.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace tobii::manager {
namespace {

constexpr float kPi = 3.14159265358979f;

struct Vec3 { float x, y, z; };

Vec3 Rotate(const Vec3& v, float yaw_deg, float pitch_deg, float roll_deg) {
    const float y = yaw_deg * kPi / 180.0f;
    const float p = pitch_deg * kPi / 180.0f;
    const float r = roll_deg * kPi / 180.0f;

    float x = v.x * std::cos(r) - v.y * std::sin(r);
    float yy = v.x * std::sin(r) + v.y * std::cos(r);
    float z = v.z;

    const float y2 = yy * std::cos(p) - z * std::sin(p);
    const float z2 = yy * std::sin(p) + z * std::cos(p);

    const float x3 = x * std::cos(y) + z2 * std::sin(y);
    const float z3 = -x * std::sin(y) + z2 * std::cos(y);

    return {x3, y2, z3};
}

}

void DrawHead(const HeadViewState& state, float size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 centre{origin.x + size * 0.5f, origin.y + size * 0.5f};
    const float scale = size * 0.24f;

    draw->AddRectFilled(origin, {origin.x + size, origin.y + size},
                        IM_COL32(18, 22, 28, 255), 6.0f);

    if (state.camera_texture != 0) {
        draw->AddImage(static_cast<ImTextureID>(
                           static_cast<std::intptr_t>(state.camera_texture)),
                       origin, {origin.x + size, origin.y + size},
                       {1.0f, 0.0f}, {0.0f, 1.0f}, IM_COL32(255, 255, 255, 105));
        if (state.landmarks != nullptr && state.frame_width > 0) {
            const float to_widget = size / static_cast<float>(state.frame_width);
            for (std::size_t i = 0; i + 1 < state.landmark_count; i += 2) {
                const float x = origin.x + size - state.landmarks[i] * to_widget;
                const float y = origin.y + state.landmarks[i + 1] * to_widget;
                draw->AddRectFilled({x, y}, {x + 1.5f, y + 1.5f},
                                    IM_COL32(120, 255, 170, 150));
            }
        }
    }

    const ImU32 grid = IM_COL32(45, 52, 62, 255);
    for (int i = 1; i < 4; ++i) {
        const float t = size * static_cast<float>(i) / 4.0f;
        draw->AddLine({origin.x, origin.y + t}, {origin.x + size, origin.y + t}, grid);
        draw->AddLine({origin.x + t, origin.y}, {origin.x + t, origin.y + size}, grid);
    }

    if (!state.tracked) {
        const char* text = "no face";
        const ImVec2 extent = ImGui::CalcTextSize(text);
        draw->AddText({centre.x - extent.x * 0.5f, centre.y - extent.y * 0.5f},
                      IM_COL32(120, 130, 145, 255), text);
        ImGui::Dummy({size, size});
        return;
    }

    const float shift_x = state.translation_mm[0] * size * 0.0016f;
    const float shift_y = -state.translation_mm[1] * size * 0.0016f;
    const float depth = 1.0f + state.translation_mm[2] * 0.0012f;

    const auto project = [&](const Vec3& v) {
        const Vec3 r = Rotate(v, state.yaw, state.pitch, state.roll);
        const float w = 1.0f / (1.0f + r.z * 0.35f);
        return ImVec2{centre.x + shift_x + r.x * scale * w * depth,
                      centre.y + shift_y + r.y * scale * w * depth};
    };

    const float hw = 0.62f, hh = 0.80f, hd = 0.62f;
    const Vec3 corners[8] = {
        {-hw, -hh, -hd}, { hw, -hh, -hd}, { hw,  hh, -hd}, {-hw,  hh, -hd},
        {-hw, -hh,  hd}, { hw, -hh,  hd}, { hw,  hh,  hd}, {-hw,  hh,  hd},
    };
    ImVec2 p[8];
    for (int i = 0; i < 8; ++i) p[i] = project(corners[i]);

    const ImU32 back = IM_COL32(70, 82, 98, 255);
    const ImU32 front = IM_COL32(120, 190, 255, 255);
    const int edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},
                              {0,4},{1,5},{2,6},{3,7}};
    for (const auto& e : edges) {
        const bool is_front = e[0] >= 4 && e[1] >= 4;
        draw->AddLine(p[e[0]], p[e[1]], is_front ? front : back, is_front ? 2.5f : 1.5f);
    }

    const ImVec2 left_eye = project({-0.26f, -0.20f, hd});
    const ImVec2 right_eye = project({0.26f, -0.20f, hd});
    const ImVec2 nose = project({0.0f, 0.10f, hd + 0.18f});
    const ImU32 eye_ok = IM_COL32(90, 220, 130, 255);
    const ImU32 eye_off = IM_COL32(150, 70, 70, 255);
    draw->AddCircleFilled(left_eye, size * 0.026f,
                          state.left_eye_valid ? eye_ok : eye_off);
    draw->AddCircleFilled(right_eye, size * 0.026f,
                          state.right_eye_valid ? eye_ok : eye_off);
    draw->AddCircleFilled(nose, size * 0.016f, IM_COL32(255, 190, 120, 255));

    if (state.gaze_valid) {
        const ImVec2 between{(left_eye.x + right_eye.x) * 0.5f, (left_eye.y + right_eye.y) * 0.5f};
        const ImVec2 target{origin.x + state.gaze_x * size, origin.y + state.gaze_y * size};
        draw->AddLine(between, target, IM_COL32(120, 255, 170, 160), 1.5f);
        draw->AddCircle(target, size * 0.030f, IM_COL32(120, 255, 170, 220), 0, 2.0f);
    }

    ImGui::Dummy({size, size});
}

}

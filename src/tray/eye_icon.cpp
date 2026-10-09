#include "tobii/tray/eye_icon.hpp"

#include <algorithm>
#include <cmath>

namespace tobii::tray {
namespace {

struct Colour { float r, g, b, a; };

Colour Over(const Colour& top, const Colour& bottom) {
    const float a = top.a + bottom.a * (1.0f - top.a);
    if (a <= 0.0f) return {0, 0, 0, 0};
    return {(top.r * top.a + bottom.r * bottom.a * (1.0f - top.a)) / a,
            (top.g * top.a + bottom.g * bottom.a * (1.0f - top.a)) / a,
            (top.b * top.a + bottom.b * bottom.a * (1.0f - top.a)) / a, a};
}

float Coverage(float signed_distance, float softness) {
    if (signed_distance <= -softness) return 1.0f;
    if (signed_distance >= softness) return 0.0f;
    return 0.5f - signed_distance / (2.0f * softness);
}

float SegmentDistance(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay;
    const float wx = px - ax, wy = py - ay;
    const float len2 = vx * vx + vy * vy;
    const float t = len2 > 0.0f ? std::clamp((wx * vx + wy * vy) / len2, 0.0f, 1.0f) : 0.0f;
    return std::hypot(px - (ax + t * vx), py - (ay + t * vy));
}

}

std::vector<std::uint8_t> RenderEye(int size, EyeState state) {
    if (size <= 0) return {};
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size) *
                                  static_cast<std::size_t>(size) * 4, 0);

    const float s = static_cast<float>(size);
    const float cx = s * 0.5f;
    const float cy = s * 0.5f;
    const float soft = s * 0.035f;

    const float lid_radius = s * 0.62f;
    const float lid_offset = s * 0.42f;
    const float iris_r = s * 0.20f;
    const float pupil_r = s * 0.085f;

    const bool open = state == EyeState::kWatching;
    const Colour sclera{0.93f, 0.95f, 0.98f, 1.0f};
    const Colour iris = open ? Colour{0.29f, 0.62f, 0.93f, 1.0f}
                             : Colour{0.45f, 0.49f, 0.55f, 1.0f};
    const Colour pupil{0.05f, 0.06f, 0.09f, 1.0f};
    const Colour outline{0.08f, 0.10f, 0.13f, 1.0f};
    const Colour lid{0.62f, 0.66f, 0.72f, 1.0f};
    const Colour cross{0.90f, 0.22f, 0.22f, 1.0f};

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;

            const float upper = std::hypot(px - cx, py - (cy + lid_offset)) - lid_radius;
            const float lower = std::hypot(px - cx, py - (cy - lid_offset)) - lid_radius;
            const float eye = std::max(upper, lower);

            Colour c{0, 0, 0, 0};

            if (open) {
                const float d_iris = std::hypot(px - cx, py - cy) - iris_r;
                const float d_pupil = std::hypot(px - cx, py - cy) - pupil_r;
                c = Over({sclera.r, sclera.g, sclera.b, Coverage(eye, soft)}, c);
                c = Over({iris.r, iris.g, iris.b,
                          Coverage(d_iris, soft) * Coverage(eye, soft)}, c);
                c = Over({pupil.r, pupil.g, pupil.b,
                          Coverage(d_pupil, soft) * Coverage(eye, soft)}, c);
                const float d_glint =
                    std::hypot(px - (cx - iris_r * 0.45f), py - (cy - iris_r * 0.45f)) - s * 0.035f;
                c = Over({1.0f, 1.0f, 1.0f,
                          0.85f * Coverage(d_glint, soft) * Coverage(d_iris, soft)}, c);
                const float rim = 1.0f - std::min(1.0f, std::fabs(eye) / (s * 0.045f));
                c = Over({outline.r, outline.g, outline.b, outline.a * rim * 0.9f}, c);
            } else {
                const float lash = std::hypot(px - cx, py - (cy - lid_offset * 0.55f)) -
                                   lid_radius * 0.82f;
                const float band = 1.0f - std::min(1.0f, std::fabs(lash) / (s * 0.05f));
                const bool inside = std::fabs(px - cx) < s * 0.36f && py > cy - s * 0.12f;
                if (inside) c = Over({lid.r, lid.g, lid.b, band}, c);
                const float rim = 1.0f - std::min(1.0f, std::fabs(eye) / (s * 0.045f));
                c = Over({outline.r, outline.g, outline.b, outline.a * rim * 0.45f}, c);
            }

            if (state == EyeState::kNoDaemon) {
                const float arm = s * 0.055f;
                const float d1 = SegmentDistance(px, py, s * 0.20f, s * 0.20f,
                                                 s * 0.80f, s * 0.80f) - arm;
                const float d2 = SegmentDistance(px, py, s * 0.80f, s * 0.20f,
                                                 s * 0.20f, s * 0.80f) - arm;
                const float mark = std::max(Coverage(d1, soft), Coverage(d2, soft));
                c = Over({cross.r, cross.g, cross.b, mark}, c);
            }

            const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 4;
            out[i + 0] = static_cast<std::uint8_t>(c.a * 255.0f);
            out[i + 1] = static_cast<std::uint8_t>(c.r * c.a * 255.0f);
            out[i + 2] = static_cast<std::uint8_t>(c.g * c.a * 255.0f);
            out[i + 3] = static_cast<std::uint8_t>(c.b * c.a * 255.0f);
        }
    }
    return out;
}

}

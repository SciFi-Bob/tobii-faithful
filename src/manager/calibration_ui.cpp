#include "tobii/eventlog.hpp"
#include "tobii/manager/axis_convention.hpp"
#include "tobii/manager/calibration_ui.hpp"

#include "imgui.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <limits>

namespace tobii::manager {
namespace {

constexpr double kFixationHold = 0.30;
constexpr std::size_t kFixationWindow = 14;
constexpr double kPatienceHint = 4.0;

constexpr double kGazeHold = 0.6;

constexpr float kAcceptRadius = 0.35f;

constexpr float kMaxGazeAngleDeg = 18.0f;

constexpr float kArrivalDistance = 0.06f;
constexpr double kCapturedFlash = 0.45;

constexpr float kRadiusSizeModifier = 0.45f;
constexpr double kProximityGain = 20.0 / 9.0;
constexpr double kProximityAlpha = 0.6;
constexpr double kFocusChangeHoldS = 0.150;
constexpr double kGazeLeftGraceS = 0.250;
constexpr double kSaccadeSettleS = 0.55;
constexpr double kTrackCentreMm = 636.0;
constexpr double kTrackHalfDepthMm = 287.0;

}

bool GazeLooksAtTarget(float gaze_x, float gaze_y, float target_x, float target_y,
                       float last_x, float last_y, bool have_last) {
    if (std::hypot(gaze_x - target_x, gaze_y - target_y) >= kAcceptRadius) return false;
    if (!have_last) return true;
    return std::hypot(gaze_x - last_x, gaze_y - last_y) >= kArrivalDistance;
}

std::vector<std::string> CalibrationUi::MonitorNames() {
    std::vector<std::string> names;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        const char* name = glfwGetMonitorName(monitors[i]);
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s  %dx%d", name != nullptr ? name : "display",
                      mode != nullptr ? mode->width : 0, mode != nullptr ? mode->height : 0);
        names.emplace_back(buf);
    }
    return names;
}

std::vector<std::string> CalibrationUi::MonitorKeys() {
    std::vector<std::string> keys;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        const char* name = glfwGetMonitorName(monitors[i]);
        keys.emplace_back(name != nullptr ? name : "");
    }
    return keys;
}

int CalibrationUi::MonitorContainingWindow(GLFWwindow* window) {
    if (window == nullptr) return -1;
    glfwGetError(nullptr);
    int wx = 0, wy = 0;
    glfwGetWindowPos(window, &wx, &wy);
    if (glfwGetError(nullptr) != GLFW_NO_ERROR) return -1;

    int ww = 0, wh = 0;
    glfwGetWindowSize(window, &ww, &wh);
    const int cx = wx + ww / 2;
    const int cy = wy + wh / 2;

    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        if (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh) return i;
    }
    return -1;
}

void CalibrationUi::ResolveInitialMonitor(GLFWwindow* window) {
    (void)window;
    int count = 0;
    glfwGetMonitors(&count);
    if (count == 0) { monitor_index_ = -1; return; }

    if (profile_.display.monitor_chosen()) {
        const auto keys = MonitorKeys();
        for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
            if (keys[static_cast<std::size_t>(i)] == profile_.display.monitor_name) {
                monitor_index_ = i;
                return;
            }
        }
        monitor_index_ = std::clamp(profile_.display.monitor_index, 0, count - 1);
        return;
    }

    monitor_index_ = -1;
}

CalibrationUi::CalibrationUi(Client& client, std::string profile_path)
    : client_(client), profile_path_(std::move(profile_path)) {
    ReloadFromDisk();
}

void CalibrationUi::ReloadFromDisk() { profile_.Load(profile_path_); }

void CalibrationUi::SetMonitor(int index) {
    const auto keys = MonitorKeys();
    const std::string name =
        index >= 0 && index < static_cast<int>(keys.size())
            ? keys[static_cast<std::size_t>(index)] : std::string();
    if (index == profile_.display.monitor_index &&
        name == profile_.display.monitor_name) {
        return;
    }
    monitor_index_ = index;
    calibration::Profile current;
    if (current.Load(profile_path_)) profile_ = current;
    profile_.display.monitor_index = index;
    profile_.display.monitor_name = name;
    profile_.Save(profile_path_);
    client_.ReloadProfile();
}

void CalibrationUi::StartScreenSetup() {
    mark_left_ = 0.42f;
    mark_right_ = 0.58f;
    dragging_mark_ = -1;
    mode_ = Mode::kScreen;
    status_ = "line the two marks up with the tracker";
}

void CalibrationUi::StartSetup() {
    improve_ = false;
    std::remove(profile_path_.c_str());
    profile_ = calibration::Profile{};
    client_.ReloadProfile();
    StartScreenSetup();
    chain_to_gaze_ = true;
    status_ = "step 1 of 2: line the marks up";
}

void CalibrationUi::StartGaze(bool improve) {
    improve_ = improve;
    const double w = profile_.display.width_mm > 1.0f ? profile_.display.width_mm : 600.0;
    const double h = profile_.display.height_mm > 1.0f ? profile_.display.height_mm : 340.0;
    cal_targets_ = device::CalibrationTargets(device::ScreenSizeMm{w, h});
    cal_collected_.assign(cal_targets_.size(), false);
    cal_stage_ = 0;
    cal_busy_until_ = 0.0;
    cal_focus_ = -1;
    cal_focus_since_ = 0.0;
    cal_focus_changed_ = 0.0;
    cal_in_zone_s_ = 0.0;
    cal_proximity_x_ = 0.0;
    cal_proximity_init_ = false;
    have_last_point_ = false;
    captured_s_ = 0.0;
    client_.CalStart();
    client_.SetEnabledEye(enabled_eye_);
    client_.CalClear();
    if (improve_) client_.CalApply();
    mode_ = Mode::kGaze;
    status_ = improve_ ? "improving the calibration" : "calibrating the tracker";
}

void CalibrationUi::LayOutTargets(int grid, float eye_distance_mm) {
    targets_.clear();
    const float width = profile_.display.width_mm > 1.0f ? profile_.display.width_mm : 600.0f;
    const float height = profile_.display.height_mm > 1.0f ? profile_.display.height_mm : 340.0f;
    const float distance = eye_distance_mm > 200.0f ? eye_distance_mm : 650.0f;

    const float reach = distance * std::tan(kMaxGazeAngleDeg * 3.14159265f / 180.0f);
    const float half_diagonal = std::hypot(width * 0.5f, height * 0.5f);
    const float scale = std::min(reach / half_diagonal, 0.88f);
    const float fx = scale;
    const float fy = scale;

    for (int row = 0; row < grid; ++row) {
        for (int col = 0; col < grid; ++col) {
            const float u = grid > 1 ? static_cast<float>(col) / (grid - 1) * 2.0f - 1.0f : 0.0f;
            const float v = grid > 1 ? static_cast<float>(row) / (grid - 1) * 2.0f - 1.0f : 0.0f;
            targets_.push_back({0.5f + u * fx * 0.5f, 0.5f + v * fy * 0.5f});
        }
    }
}

void CalibrationUi::Cancel() {
    if (mode_ == Mode::kGaze) client_.CalStop();
    chain_to_gaze_ = false;
    gaze_preroll_ = false;
    mode_ = Mode::kIdle;
    status_ = "stopped, nothing was changed";
}

void CalibrationUi::Save() {
    calibration::Profile current;
    if (current.Load(profile_path_)) {
        current.display.monitor_index = profile_.display.monitor_index;
        current.display.monitor_name = profile_.display.monitor_name;
        profile_ = current;
    }
    if (profile_.Save(profile_path_)) {
        client_.ReloadProfile();
        status_ += "  (saved)";
    } else {
        status_ = "could not write " + profile_path_;
    }
}

void CalibrationUi::EnterFullscreen(GLFWwindow* window) {
    if (fullscreen_ || window == nullptr) return;
    glfwGetWindowPos(window, &saved_x_, &saved_y_);
    glfwGetWindowSize(window, &saved_w_, &saved_h_);

    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    if (count == 0) return;
    const int index = std::clamp(monitor_index_, 0, count - 1);
    GLFWmonitor* chosen = monitors[index];

    const GLFWvidmode* mode = glfwGetVideoMode(chosen);
    if (mode == nullptr) return;
    glfwSetWindowMonitor(window, chosen, 0, 0, mode->width, mode->height, mode->refreshRate);
    fullscreen_ = true;
}

void CalibrationUi::LeaveFullscreen(GLFWwindow* window) {
    if (!fullscreen_ || window == nullptr) return;
    glfwSetWindowMonitor(window, nullptr, saved_x_, saved_y_,
                         saved_w_ > 0 ? saved_w_ : 1180,
                         saved_h_ > 0 ? saved_h_ : 760, 0);
    fullscreen_ = false;
}

void CalibrationUi::FinishGaze() {
    bool all = !cal_collected_.empty();
    for (const bool c : cal_collected_) if (!c) all = false;
    status_ = all ? "tracker calibrated" : "calibration cancelled";
    mode_ = Mode::kIdle;
}

bool CalibrationUi::Draw(GLFWwindow* window, const Snapshot& snapshot, double now_s) {
    if (mode_ == Mode::kIdle) {
        LeaveFullscreen(window);
        return false;
    }

    if (mode_ == Mode::kGaze || mode_ == Mode::kScreen) EnterFullscreen(window);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("calibration", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = viewport->WorkPos;
    const ImVec2 size = viewport->WorkSize;
    draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(12, 14, 18, 255));

    const auto centred_text = [&](const char* text, float y_fraction, ImU32 colour) {
        const ImVec2 extent = ImGui::CalcTextSize(text);
        draw->AddText({origin.x + (size.x - extent.x) * 0.5f, origin.y + size.y * y_fraction},
                      colour, text);
    };

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        Cancel();
        LeaveFullscreen(window);
        ImGui::End();
        return true;
    }

    if (mode_ == Mode::kScreen) {
        const float line_w = 3.0f;
        const float grab = 28.0f;
        const ImVec2 mouse = ImGui::GetMousePos();

        float* marks[2] = {&mark_left_, &mark_right_};
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            float best = grab;
            for (int i = 0; i < 2; ++i) {
                const float x = origin.x + size.x * *marks[i];
                if (std::fabs(mouse.x - x) < best) { best = std::fabs(mouse.x - x); dragging_mark_ = i; }
            }
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging_mark_ = -1;
        if (dragging_mark_ >= 0 && size.x > 0.0f) {
            *marks[dragging_mark_] =
                std::clamp((mouse.x - origin.x) / size.x, 0.02f, 0.98f);
        }
        const float nudge = ImGui::IsKeyDown(ImGuiMod_Shift) ? 0.0002f : 0.002f;
        int focused = dragging_mark_ >= 0 ? dragging_mark_ : 0;
        if (ImGui::IsKeyPressed(ImGuiKey_Tab)) focused = 1 - focused;
        if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) {
            *marks[focused] = std::clamp(*marks[focused] - nudge, 0.02f, 0.98f);
        }
        if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) {
            *marks[focused] = std::clamp(*marks[focused] + nudge, 0.02f, 0.98f);
        }
        if (mark_right_ < mark_left_) std::swap(mark_left_, mark_right_);

        if (chain_to_gaze_) {
            centred_text("Step 1 of 2 - where your screen is", 0.24f,
                         IM_COL32(150, 162, 180, 255));
        }
        centred_text("Drag each line onto one of the two white marks on your tracker.",
                     0.30f, IM_COL32(235, 242, 252, 255));
        centred_text("Arrow keys nudge, shift for fine. Space when both line up, Esc to cancel.",
                     0.345f, IM_COL32(150, 162, 180, 255));

        for (int i = 0; i < 2; ++i) {
            const float x = origin.x + size.x * *marks[i];
            draw->AddRectFilled({x - line_w * 0.5f, origin.y + size.y * 0.42f},
                                {x + line_w * 0.5f, origin.y + size.y},
                                IM_COL32(120, 200, 255, 255));
            draw->AddTriangleFilled({x - 12.0f, origin.y + size.y * 0.42f},
                                    {x + 12.0f, origin.y + size.y * 0.42f},
                                    {x, origin.y + size.y * 0.42f + 16.0f},
                                    IM_COL32(120, 200, 255, 255));
        }

        calibration::DisplayArea preview = profile_.display;
        int px_w = 0, px_h = 0;
        if (GLFWmonitor* monitor = glfwGetWindowMonitor(window); monitor != nullptr) {
            if (const GLFWvidmode* mode = glfwGetVideoMode(monitor); mode != nullptr) {
                px_w = mode->width;
                px_h = mode->height;
            }
        }
        const bool usable = preview.SetFromMarkAlignment(mark_left_, mark_right_, px_w, px_h);
        char buf[220];
        if (usable) {
            std::snprintf(buf, sizeof(buf),
                          "screen %.0f x %.0f mm,  tracker %+.0f mm from centre",
                          preview.width_mm, preview.height_mm, -preview.offset_x_mm);
        } else {
            std::snprintf(buf, sizeof(buf), "move the lines further apart");
        }
        centred_text(buf, 0.37f, usable ? IM_COL32(120, 255, 170, 255)
                                        : IM_COL32(255, 180, 90, 255));

        if (usable && ImGui::IsKeyPressed(ImGuiKey_Space)) {
            calibration::Profile current;
            if (current.Load(profile_path_)) profile_ = current;
            profile_.display.SetFromMarkAlignment(mark_left_, mark_right_, px_w, px_h);
            const auto keys = MonitorKeys();
            profile_.display.monitor_index = monitor_index_;
            if (monitor_index_ >= 0 && monitor_index_ < static_cast<int>(keys.size())) {
                profile_.display.monitor_name = keys[static_cast<std::size_t>(monitor_index_)];
            }
            std::snprintf(buf, sizeof(buf), "screen set: %.0f x %.0f mm, tracker %+.0f mm off centre",
                          profile_.display.width_mm, profile_.display.height_mm,
                          -profile_.display.offset_x_mm);
            status_ = buf;
            if (profile_.Save(profile_path_)) {
                client_.ReloadProfile();
                eventlog::Record("Calibration saved (screen setup)");
            }
            if (chain_to_gaze_) {
                chain_to_gaze_ = false;
                StartGaze();
                status_ = "step 2 of 2: look at each dot";
            } else {
                mode_ = Mode::kIdle;
                LeaveFullscreen(window);
            }
        }
        ImGui::End();
        return true;
    }

    if (mode_ != Mode::kGaze && !snapshot.face) {
        centred_text("I cannot see your face - move back into view", 0.48f,
                     IM_COL32(255, 180, 90, 255));
        ImGui::End();
        return true;
    }

    if (mode_ == Mode::kGaze) {
        if (cal_busy_until_ > 0.0) {
            const ImVec2 mid{origin.x + size.x * 0.5f, origin.y + size.y * 0.5f};
            const double spin = now_s * 3.5;
            for (int i = 0; i < 12; ++i) {
                const double a = spin + i * 0.5236;
                const int alpha = 40 + (i * 200) / 12;
                draw->AddCircleFilled({mid.x + 26.0f * static_cast<float>(std::cos(a)),
                                       mid.y + 26.0f * static_cast<float>(std::sin(a))},
                                      4.0f, IM_COL32(150, 200, 255, alpha));
            }
            centred_text("saving...", 0.60f, IM_COL32(150, 162, 180, 255));
            if (now_s >= cal_busy_until_) {
                cal_busy_until_ = 0.0;
                if (cal_stage_ >= 2) {
                    FinishGaze();
                    LeaveFullscreen(window);
                    ImGui::End();
                    return true;
                }
                ++cal_stage_;
                cal_focus_ = -1;
                cal_focus_since_ = 0.0;
                cal_focus_changed_ = 0.0;
                cal_in_zone_s_ = 0.0;
            }
            ImGui::End();
            return true;
        }

        bool flashing = false;
        if (captured_s_ > 0.0) {
            const double since = now_s - captured_s_;
            const float t = static_cast<float>(std::min(since / kCapturedFlash, 1.0));
            draw->AddCircle({captured_at_.x, captured_at_.y}, 30.0f + t * 45.0f,
                            IM_COL32(120, 220, 160, static_cast<int>(220.0f * (1.0f - t))), 48, 4.0f);
            if (since >= kCapturedFlash) captured_s_ = 0.0;
            else flashing = true;
        }

        const float px_w = profile_.display.pixel_width > 0
                               ? static_cast<float>(profile_.display.pixel_width) : 1920.0f;
        const float px_h = profile_.display.pixel_height > 0
                               ? static_cast<float>(profile_.display.pixel_height) : 1080.0f;

        float base_radius_px = std::max(px_w, px_h);
        {
            float closest = std::numeric_limits<float>::max();
            for (std::size_t i = 0; i < cal_targets_.size(); ++i) {
                if (cal_targets_[i].stage != cal_stage_) continue;
                for (std::size_t j = i + 1; j < cal_targets_.size(); ++j) {
                    if (cal_targets_[j].stage != cal_stage_) continue;
                    const float d = std::hypot(
                        static_cast<float>(cal_targets_[i].x - cal_targets_[j].x) * px_w,
                        static_cast<float>(cal_targets_[i].y - cal_targets_[j].y) * px_h);
                    closest = std::min(closest, d);
                }
            }
            if (closest != std::numeric_limits<float>::max())
                base_radius_px = closest * 0.5f * kRadiusSizeModifier;
        }

        if (snapshot.eye_distance_mm > 1.0) {
            const double z = (kTrackCentreMm - snapshot.eye_distance_mm) / kTrackHalfDepthMm;
            const double x = z < 0.0 ? std::min(-z, 1.0) : 0.0;
            cal_proximity_x_ = cal_proximity_init_
                ? cal_proximity_x_ + kProximityAlpha * (x - cal_proximity_x_) : x;
            cal_proximity_init_ = true;
        }
        const double proximity = 1.0 + kProximityGain * cal_proximity_x_;
        const float eff_radius_px = static_cast<float>(base_radius_px * proximity);

        const float gaze_px_x = snapshot.raw_gaze_x * px_w;
        const float gaze_px_y = snapshot.raw_gaze_y * px_h;
        const float last_dist = have_last_point_
            ? std::hypot(gaze_px_x - last_point_x_ * px_w, gaze_px_y - last_point_y_ * px_h)
            : std::numeric_limits<float>::max();
        int candidate = -1;
        float best = eff_radius_px;
        for (std::size_t i = 0; i < cal_targets_.size(); ++i) {
            if (cal_targets_[i].stage != cal_stage_ || cal_collected_[i] ||
                !snapshot.raw_gaze_valid)
                continue;
            const float d = std::hypot(gaze_px_x - static_cast<float>(cal_targets_[i].x) * px_w,
                                       gaze_px_y - static_cast<float>(cal_targets_[i].y) * px_h);
            if (d < best && d < last_dist) { best = d; candidate = static_cast<int>(i); }
        }

        if (candidate >= 0 && cal_focus_ < 0) {
            cal_focus_ = candidate;
            cal_focus_since_ = now_s;
            cal_focus_changed_ = now_s;
            cal_in_zone_s_ = now_s;
        } else if (candidate >= 0 && candidate != cal_focus_ &&
                   (now_s - cal_focus_changed_) >= kFocusChangeHoldS) {
            cal_focus_ = candidate;
            cal_focus_since_ = now_s;
            cal_focus_changed_ = now_s;
            cal_in_zone_s_ = now_s;
        } else if (candidate == cal_focus_ && candidate >= 0) {
            cal_in_zone_s_ = now_s;
        }
        if (cal_focus_ >= 0 && (now_s - cal_in_zone_s_) >= kGazeLeftGraceS) cal_focus_ = -1;

        for (std::size_t i = 0; i < cal_targets_.size(); ++i) {
            if (cal_targets_[i].stage != cal_stage_) continue;
            const ImVec2 c{origin.x + static_cast<float>(cal_targets_[i].x) * size.x,
                           origin.y + static_cast<float>(cal_targets_[i].y) * size.y};
            if (cal_collected_[i]) {
                draw->AddCircleFilled(c, 12.0f, IM_COL32(120, 220, 160, 220));
                continue;
            }
            draw->AddCircleFilled(c, 30.0f, IM_COL32(32, 38, 48, 255));
            draw->AddCircle(c, 30.0f, IM_COL32(85, 140, 210, 255), 0, 2.0f);
            if (static_cast<int>(i) == cal_focus_) {
                const float hold = static_cast<float>(
                    std::min((now_s - cal_focus_since_) / kSaccadeSettleS, 1.0));
                draw->PathArcTo(c, 22.0f, -1.5708f, -1.5708f + hold * 6.2832f, 48);
                draw->PathStroke(IM_COL32(120, 220, 160, 255), 0, 5.0f);
            }
            draw->AddCircleFilled(c, 6.0f, IM_COL32(235, 245, 255, 255));
        }

        if (cal_focus_ >= 0 && !flashing && (now_s - cal_focus_since_) >= kSaccadeSettleS) {
            const auto& dot = cal_targets_[static_cast<std::size_t>(cal_focus_)];
            client_.CalCollect(dot.x, dot.y);
            cal_collected_[static_cast<std::size_t>(cal_focus_)] = true;
            captured_s_ = now_s;
            captured_at_ = {origin.x + static_cast<float>(dot.x) * size.x,
                            origin.y + static_cast<float>(dot.y) * size.y};
            last_point_x_ = static_cast<float>(dot.x);
            last_point_y_ = static_cast<float>(dot.y);
            have_last_point_ = true;
            cal_focus_ = -1;

            bool stage_done = true;
            for (std::size_t i = 0; i < cal_targets_.size(); ++i)
                if (cal_targets_[i].stage == cal_stage_ && !cal_collected_[i]) stage_done = false;
            if (stage_done) {
                client_.CalCompute();
                if (cal_stage_ >= 2) { client_.CalStop(); client_.CalSave(); }
                cal_busy_until_ = now_s + 1.8;
            }
        }

        char caption[128];
        std::snprintf(caption, sizeof(caption),
                      "Look at each dot and hold still        stage %d of 3        escape to cancel",
                      cal_stage_ + 1);
        centred_text(caption, 0.94f, IM_COL32(150, 160, 175, 255));
        ImGui::End();
        return true;
    }

    ImGui::End();
    return true;
}

}

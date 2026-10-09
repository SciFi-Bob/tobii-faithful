#pragma once

#include "tobii/calibration/profile.hpp"
#include "tobii/device/calibration_layout.hpp"
#include "tobii/manager/calibration_controller.hpp"
#include "tobii/manager/client.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace tobii::manager {

struct ImVec2Lite { float x = 0.0f, y = 0.0f; };

bool GazeLooksAtTarget(float gaze_x, float gaze_y, float target_x, float target_y,
                       float last_x, float last_y, bool have_last);

class CalibrationUi {
public:
    enum class Mode { kIdle, kScreen, kGaze };

    CalibrationUi(Client& client, std::string profile_path);

    bool Draw(GLFWwindow* window, const Snapshot& snapshot, double now_s);

    static std::vector<std::string> MonitorNames();
    static std::vector<std::string> MonitorKeys();

    static int MonitorContainingWindow(GLFWwindow* window);

    void ResolveInitialMonitor(GLFWwindow* window);

    void set_monitor(int index) { monitor_index_ = index; }
    void SetMonitor(int index);
    int monitor() const { return monitor_index_; }

    void StartScreenSetup();

    void StartSetup();

    void StartGaze(bool improve = false);

    void set_enabled_eye(int eye) { enabled_eye_ = eye; }
    int enabled_eye() const { return enabled_eye_; }

    void LayOutTargets(int grid, float eye_distance_mm);
    void Cancel();

    Mode mode() const { return mode_; }
    const std::string& status() const { return status_; }

    const calibration::Profile& profile() const { return profile_; }
    void ReloadFromDisk();

private:
    struct Target { float x, y; };

    void EnterFullscreen(GLFWwindow* window);
    void LeaveFullscreen(GLFWwindow* window);
    void FinishGaze();
    void Save();

    std::vector<device::CalibrationTarget> cal_targets_;
    std::vector<bool> cal_collected_;
    int cal_stage_ = 0;
    double cal_busy_until_ = 0.0;

    double cal_proximity_x_ = 0.0;
    bool cal_proximity_init_ = false;
    int cal_focus_ = -1;
    double cal_focus_since_ = 0.0;
    double cal_focus_changed_ = 0.0;
    double cal_in_zone_s_ = 0.0;

    Client& client_;
    std::string profile_path_;
    calibration::Profile profile_;
    Mode mode_ = Mode::kIdle;
    std::string status_;

    std::vector<Target> targets_;
    std::size_t target_index_ = 0;
    double target_started_s_ = 0.0;
    double fixation_started_s_ = 0.0;
    std::vector<float> recent_x_, recent_y_;
    float last_point_x_ = 0.0f, last_point_y_ = 0.0f;
    bool have_last_point_ = false;
    double last_gaze_seen_s_ = 0.0;

    bool gaze_preroll_ = false;
    double preroll_started_s_ = 0.0;
    bool preroll_recentred_ = false;
    std::uint64_t preroll_counter_ = 0;
    std::string preroll_problem_;
    bool chain_to_gaze_ = false;
    bool improve_ = false;
    int enabled_eye_ = 3;

    float mark_left_ = 0.42f, mark_right_ = 0.58f;
    int dragging_mark_ = -1;
    double captured_s_ = 0.0;
    ImVec2Lite captured_at_{};

    int saved_x_ = 0, saved_y_ = 0, saved_w_ = 0, saved_h_ = 0;
    bool fullscreen_ = false;
    int monitor_index_ = -1;

};

}

#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <utility>
#include <string>
#include <thread>
#include <vector>

namespace tobii::manager {

struct Snapshot {
    bool connected = false;
    bool face = false;
    bool present = false;
    bool gaze_valid = false;
    bool device_open = false;
    bool game = false;
    std::string last_reply;
    bool last_reply_ok = true;

    std::array<float, 6> pose{};
    bool pos_valid = false;

    float gaze_x = 0.5f, gaze_y = 0.5f;
    float raw_gaze_x = 0.5f, raw_gaze_y = 0.5f;
    bool raw_gaze_valid = false;
    bool left_eye_valid = false, right_eye_valid = false;
    float eye_distance_mm = 0.0f;
    float presence = 0.0f;
    float distance_mm = 0.0f;
    float fps = 0.0f;
    std::uint64_t counter = 0;

    std::string error;
};

struct FrameData {
    bool available = false;
    int width = 0, height = 0;
    std::vector<std::uint8_t> jpeg;
    std::vector<float> landmarks;
    std::uint64_t sequence = 0;
};

class Client {
public:
    explicit Client(std::string socket_path);
    ~Client();

    void Start();
    void Stop();

    Snapshot GetSnapshot();
    FrameData GetFrame();

    void RequestFrame();

    void Send(const std::string& json_line);
    void ReloadProfile() { Send("{\"cmd\":\"reload-profile\"}\n"); }

    void CalStart() { Send("{\"cmd\":\"cal-start\"}\n"); }
    void CalClear() { Send("{\"cmd\":\"cal-clear\"}\n"); }
    void CalCompute() { Send("{\"cmd\":\"cal-compute\"}\n"); }
    void CalStop() { Send("{\"cmd\":\"cal-stop\"}\n"); }
    void CalSave() { Send("{\"cmd\":\"cal-save\"}\n"); }
    void CalApply() { Send("{\"cmd\":\"cal-apply\"}\n"); }
    void CalCollect(double x, double y) { Send(CalPoint("cal-collect", x, y)); }
    void CalDiscard(double x, double y) { Send(CalPoint("cal-discard", x, y)); }
    void SetEnabledEye(int eye) {
        Send("{\"cmd\":\"enabled-eye\",\"eye\":" + std::to_string(eye) + "}\n");
    }
    void SetHold(bool hold) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hold_wanted_ = hold;
        }
        Send(std::string("{\"cmd\":\"hold\",\"device\":") + (hold ? "true" : "false") + "}\n");
    }

private:
    void Run();
    void HandleLine(const std::string& line);

    static std::string CalPoint(const char* cmd, double x, double y) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "{\"cmd\":\"%s\",\"x\":%.6f,\"y\":%.6f}\n", cmd, x, y);
        return buf;
    }

    std::string socket_path_;
    std::thread thread_;
    std::mutex mutex_;
    Snapshot snapshot_;
    FrameData frame_;
    std::vector<std::string> outbox_;
    bool stop_ = false;
    bool frame_wanted_ = false;
    bool hold_wanted_ = true;
};

}

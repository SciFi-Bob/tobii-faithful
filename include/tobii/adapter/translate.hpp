#pragma once

#include <cstdint>
#include <string>

namespace tobii::adapter {

struct PosePacket {
    double x_cm = 0, y_cm = 0, z_cm = 0;
    double yaw_deg = 0, pitch_deg = 0, roll_deg = 0;
    double source_code = 0, packet_counter = 0;
    double gaze_x = 0, gaze_y = 0, gaze_valid = 0, gaze_counter = 0;
};
static_assert(sizeof(PosePacket) == 12 * sizeof(double), "bridge expects 12 packed doubles");

bool PosePacketFromLine(const std::string& line, std::uint64_t counter, PosePacket& out);

struct HeadPoseHold {
    bool have_pos = false;
    double pos[3] = {0, 0, 600};
    bool have_rot[3] = {false, false, false};
    double rot[3] = {0, 0, 0};
};
bool HeadPosePacketFromLine(const std::string& line, std::uint64_t counter, PosePacket& out,
                            HeadPoseHold* hold = nullptr);

void ApplyAxisMask(PosePacket& packet, std::uint8_t axes);

bool PresentFromLine(const std::string& line);

struct DisplayGeometry {
    int width = 0, height = 0, x = 0, y = 0;
    bool found = false;
};

DisplayGeometry ParseXrandr(const std::string& output);

}

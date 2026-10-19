#include "tobii/adapter/translate.hpp"

#include "tobii/games/catalogue.hpp"

#include <cstdio>
#include <cstdlib>

namespace tobii::adapter {
namespace {

bool JsonNumber(const std::string& line, const char* key, double& out) {
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t pos = line.find(needle);
    if (pos == std::string::npos) return false;
    out = std::strtod(line.c_str() + pos + needle.size(), nullptr);
    return true;
}

bool JsonTrue(const std::string& line, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t pos = line.find(needle);
    return pos != std::string::npos && line.compare(pos + needle.size(), 4, "true") == 0;
}

}

bool PosePacketFromLine(const std::string& line, std::uint64_t counter, PosePacket& out) {
    if (line.find("\"type\":\"pose\"") == std::string::npos) return false;

    PosePacket packet{};
    JsonNumber(line, "x", packet.x_cm);
    JsonNumber(line, "y", packet.y_cm);
    JsonNumber(line, "z", packet.z_cm);
    JsonNumber(line, "yaw", packet.yaw_deg);
    JsonNumber(line, "pitch", packet.pitch_deg);
    JsonNumber(line, "roll", packet.roll_deg);
    JsonNumber(line, "gaze_x", packet.gaze_x);
    JsonNumber(line, "gaze_y", packet.gaze_y);
    packet.gaze_valid = JsonTrue(line, "gaze_valid") ? 1.0 : 0.0;
    packet.packet_counter = static_cast<double>(counter);
    packet.gaze_counter = static_cast<double>(counter);
    out = packet;
    return true;
}

namespace {
bool JsonNumberArray(const std::string& line, const char* key, double* out, int n) {
    const std::string needle = std::string("\"") + key + "\":[";
    const std::size_t pos = line.find(needle);
    if (pos == std::string::npos) return false;
    const char* p = line.c_str() + pos + needle.size();
    for (int i = 0; i < n; ++i) {
        char* end = nullptr;
        out[i] = std::strtod(p, &end);
        if (end == p) return false;
        p = end;
        while (*p == ',' || *p == ' ') ++p;
    }
    return true;
}

bool JsonBoolArray(const std::string& line, const char* key, bool* out, int n) {
    const std::string needle = std::string("\"") + key + "\":[";
    std::size_t at = line.find(needle);
    if (at == std::string::npos) return false;
    at += needle.size();
    for (int i = 0; i < n; ++i) {
        if (line.compare(at, 4, "true") == 0) { out[i] = true; at += 4; }
        else if (line.compare(at, 5, "false") == 0) { out[i] = false; at += 5; }
        else return false;
        while (at < line.size() && (line[at] == ',' || line[at] == ' ')) ++at;
    }
    return true;
}
}

bool HeadPosePacketFromLine(const std::string& line, std::uint64_t counter, PosePacket& out,
                            HeadPoseHold* hold) {
    if (line.find("\"type\":\"head_pose\"") == std::string::npos) return false;
    double pos[3] = {0, 0, 600};
    double rot[3] = {0, 0, 0};
    JsonNumberArray(line, "pos", pos, 3);
    JsonNumberArray(line, "rot", rot, 3);

    if (hold) {
        const bool pos_valid =
            line.find("\"pos_valid\":") == std::string::npos || JsonTrue(line, "pos_valid");
        bool rot_valid[3] = {true, true, true};
        JsonBoolArray(line, "rot_valid", rot_valid, 3);
        if (pos_valid) {
            for (int i = 0; i < 3; ++i) hold->pos[i] = pos[i];
            hold->have_pos = true;
        } else {
            for (int i = 0; i < 3; ++i) pos[i] = hold->pos[i];
        }
        for (int i = 0; i < 3; ++i) {
            if (rot_valid[i]) { hold->rot[i] = rot[i]; hold->have_rot[i] = true; }
            else rot[i] = hold->rot[i];
        }
    }

    constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
    PosePacket packet{};
    packet.x_cm = -pos[0] / 10.0;
    packet.y_cm = pos[1] / 10.0;
    packet.z_cm = (pos[2] - 600.0) / 10.0;
    packet.pitch_deg = rot[0] * kRadToDeg;
    packet.yaw_deg = rot[1] * kRadToDeg;
    packet.roll_deg = rot[2] * kRadToDeg;
    packet.gaze_x = 0.5;
    packet.gaze_y = 0.5;
    packet.gaze_valid = 0.0;
    packet.packet_counter = static_cast<double>(counter);
    packet.gaze_counter = static_cast<double>(counter);
    out = packet;
    return true;
}

void ApplyAxisMask(PosePacket& packet, std::uint8_t axes) {
    using namespace tobii::games;
    if (!(axes & kAxisLeanLR)) packet.x_cm = 0.0;
    if (!(axes & kAxisLeanUD)) packet.y_cm = 0.0;
    if (!(axes & kAxisLeanFB)) packet.z_cm = 0.0;
    if (!(axes & kAxisYaw)) packet.yaw_deg = 0.0;
    if (!(axes & kAxisPitch)) packet.pitch_deg = 0.0;
    if (!(axes & kAxisRoll)) packet.roll_deg = 0.0;
}

bool PresentFromLine(const std::string& line) { return JsonTrue(line, "present"); }

DisplayGeometry ParseXrandr(const std::string& output) {
    DisplayGeometry out;
    std::size_t at = 0;
    while (at < output.size()) {
        const std::size_t end = output.find('\n', at);
        const std::string text = output.substr(at, end == std::string::npos ? end : end - at);
        at = end == std::string::npos ? output.size() : end + 1;

        if (text.find(" connected") == std::string::npos) continue;
        const bool primary = text.find(" primary ") != std::string::npos;
        if (!primary && out.found) continue;
        const std::size_t geom = text.find(primary ? " primary " : " connected ");
        if (geom == std::string::npos) continue;
        int w = 0, h = 0, x = 0, y = 0;
        if (std::sscanf(text.c_str() + geom,
                        primary ? " primary %dx%d+%d+%d" : " connected %dx%d+%d+%d",
                        &w, &h, &x, &y) == 4 && w > 0 && h > 0) {
            out = {w, h, x, y, true};
            if (primary) break;
        }
    }
    return out;
}

}

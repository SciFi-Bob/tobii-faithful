#include "tobii/manager/client.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace tobii::manager {
namespace {

bool Number(const std::string& line, const char* key, float& out) {
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t pos = line.find(needle);
    if (pos == std::string::npos) return false;
    out = std::strtof(line.c_str() + pos + needle.size(), nullptr);
    return true;
}

bool Integer(const std::string& line, const char* key, std::uint64_t& out) {
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t pos = line.find(needle);
    if (pos == std::string::npos) return false;
    out = std::strtoull(line.c_str() + pos + needle.size(), nullptr, 10);
    return true;
}

bool Flag(const std::string& line, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t pos = line.find(needle);
    return pos != std::string::npos && line.compare(pos + needle.size(), 4, "true") == 0;
}

std::string StringField(const std::string& line, const char* key) {
    const std::string needle = std::string("\"") + key + "\":\"";
    const std::size_t pos = line.find(needle);
    if (pos == std::string::npos) return {};
    const std::size_t start = pos + needle.size();
    const std::size_t end = line.find('"', start);
    return end == std::string::npos ? std::string() : line.substr(start, end - start);
}

std::vector<float> Array(const std::string& line, const char* key) {
    std::vector<float> out;
    const std::string needle = std::string("\"") + key + "\":[";
    const std::size_t pos = line.find(needle);
    if (pos == std::string::npos) return out;
    const std::size_t start = pos + needle.size();
    const std::size_t end = line.find(']', start);
    if (end == std::string::npos) return out;
    const char* p = line.c_str() + start;
    const char* stop = line.c_str() + end;
    while (p < stop) {
        char* next = nullptr;
        const float value = std::strtof(p, &next);
        if (next == p) break;
        out.push_back(value);
        p = next;
        while (p < stop && (*p == ',' || *p == ' ')) ++p;
    }
    return out;
}

std::vector<std::uint8_t> DecodeBase64(const std::string& text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    int buffer = 0, bits = 0;
    for (const char c : text) {
        const int v = value(c);
        if (v < 0) continue;
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xff));
        }
    }
    return out;
}

}

Client::Client(std::string socket_path) : socket_path_(std::move(socket_path)) {}

Client::~Client() { Stop(); }

void Client::Start() {
    if (thread_.joinable()) return;
    thread_ = std::thread([this] { Run(); });
}

void Client::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    if (thread_.joinable()) thread_.join();
}

Snapshot Client::GetSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

FrameData Client::GetFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    return frame_;
}

void Client::RequestFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_wanted_ = true;
}

void Client::Send(const std::string& json_line) {
    std::lock_guard<std::mutex> lock(mutex_);
    outbox_.push_back(json_line);
}

void Client::HandleLine(const std::string& line) {
    const std::string type = StringField(line, "type");

    if (type == "pose") {
        std::lock_guard<std::mutex> lock(mutex_);
        const char* keys[6] = {"x", "y", "z", "yaw", "pitch", "roll"};
        for (int i = 0; i < 6; ++i) Number(line, keys[i], snapshot_.pose[static_cast<std::size_t>(i)]);
        Number(line, "gaze_x", snapshot_.gaze_x);
        Number(line, "gaze_y", snapshot_.gaze_y);
        const auto raw_gaze = Array(line, "raw_gaze");
        if (raw_gaze.size() >= 2) {
            snapshot_.raw_gaze_x = raw_gaze[0];
            snapshot_.raw_gaze_y = raw_gaze[1];
        }
        snapshot_.raw_gaze_valid = Flag(line, "raw_gaze_valid");
        snapshot_.pos_valid = Flag(line, "pos_valid");
        snapshot_.left_eye_valid = Flag(line, "left_eye_valid");
        snapshot_.right_eye_valid = Flag(line, "right_eye_valid");
        Number(line, "eye_distance_mm", snapshot_.eye_distance_mm);
        Number(line, "presence", snapshot_.presence);
        Number(line, "distance_mm", snapshot_.distance_mm);
        Integer(line, "counter", snapshot_.counter);
        snapshot_.face = Flag(line, "face");
        snapshot_.present = Flag(line, "present");
        snapshot_.gaze_valid = Flag(line, "gaze_valid");
        return;
    }

    if (type == "status") {
        std::lock_guard<std::mutex> lock(mutex_);
        Number(line, "fps", snapshot_.fps);
        snapshot_.device_open = StringField(line, "device") == "open";
        snapshot_.error = StringField(line, "error");
        return;
    }

    if (type == "ok" || type == "error") {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string message = StringField(line, "message");
        snapshot_.last_reply = message.empty() ? StringField(line, "cmd") : message;
        snapshot_.last_reply_ok = type == "ok";
        return;
    }

    if (type == "frame") {
        FrameData frame;
        frame.available = Flag(line, "available");
        if (frame.available) {
            float w = 0.0f, h = 0.0f;
            Number(line, "width", w);
            Number(line, "height", h);
            frame.width = static_cast<int>(w);
            frame.height = static_cast<int>(h);
            frame.jpeg = DecodeBase64(StringField(line, "jpeg"));
            frame.landmarks = Array(line, "landmarks");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        frame.sequence = frame_.sequence + 1;
        frame_ = std::move(frame);
    }
}

void Client::Run() {
    std::string buffer;
    int fd = -1;
    auto last_status = std::chrono::steady_clock::now();

    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) break;
        }

        if (fd < 0) {
            fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                ::close(fd);
                fd = -1;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    snapshot_.connected = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            buffer.clear();
            const std::string sub = "{\"cmd\":\"subscribe\",\"stream\":\"pose\",\"hold\":true}\n";
            ::send(fd, sub.data(), sub.size(), MSG_NOSIGNAL);
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.connected = true;
        }

        {
            std::vector<std::string> pending;
            bool want_frame = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending.swap(outbox_);
                want_frame = frame_wanted_;
                frame_wanted_ = false;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_status > std::chrono::milliseconds(500)) {
                last_status = now;
                pending.emplace_back("{\"cmd\":\"status\"}\n");
            }
            if (want_frame) pending.emplace_back("{\"cmd\":\"frame\"}\n");
            for (const auto& line : pending) {
                if (::send(fd, line.data(), line.size(), MSG_NOSIGNAL) < 0) {
                    ::close(fd);
                    fd = -1;
                    break;
                }
            }
            if (fd < 0) continue;
        }

        pollfd p{fd, POLLIN, 0};
        const int ready = ::poll(&p, 1, 50);
        if (ready < 0 && errno != EINTR) { ::close(fd); fd = -1; continue; }
        if (ready <= 0) continue;
        if ((p.revents & (POLLHUP | POLLERR)) != 0) { ::close(fd); fd = -1; continue; }

        char chunk[1 << 16];
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            ::close(fd);
            fd = -1;
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.connected = false;
            continue;
        }
        buffer.append(chunk, static_cast<std::size_t>(n));

        std::size_t start = 0;
        for (;;) {
            const std::size_t nl = buffer.find('\n', start);
            if (nl == std::string::npos) break;
            HandleLine(buffer.substr(start, nl - start));
            start = nl + 1;
        }
        buffer.erase(0, start);
    }

    if (fd >= 0) ::close(fd);
}

}

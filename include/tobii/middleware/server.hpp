#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tobii::middleware {

inline constexpr std::uint16_t kDefaultMiddlewarePort = 4455;

inline constexpr std::uint32_t kStreamGaze     = 0x0500;
inline constexpr std::uint32_t kStreamPresence = 0x0504;

std::vector<std::uint8_t> BuildGazeRow(std::uint32_t frame_counter, double elapsed_s,
                                       double gaze_x, double gaze_y, bool valid);

std::vector<std::uint8_t> BuildPresenceRow(double elapsed_s, bool present);

const struct MiddlewareReply* FindReply(std::uint32_t object);

inline constexpr std::uint32_t kObjectDisplayArea = 0x0596;

std::vector<std::uint8_t> BuildDisplayAreaPayload(double width, double height);

bool ContainsLicence(const std::uint8_t* data, std::size_t len);

class Server {
public:
    Server();
    ~Server();

    bool Listen(const std::string& host = "127.0.0.1",
                std::uint16_t port = kDefaultMiddlewarePort);
    void Close();

    bool Poll(int timeout_ms);

    void PublishGaze(double gaze_x, double gaze_y, bool valid);

    void PublishPresence(bool present);

    void OverrideReply(std::uint32_t object, std::vector<std::uint8_t> payload);

    std::size_t client_count() const;
    std::size_t subscriber_count() const;

    bool TakeLicenceOffered();

    const std::string& error() const { return error_; }

private:
    struct Client;
    void Drop(std::size_t index);
    void Dispatch(Client& client, const std::uint8_t* frame, std::size_t len);
    void SendEvent(Client& client, std::uint32_t stream_id,
                   const std::vector<std::uint8_t>& payload, bool force = false);

    int listen_fd_ = -1;
    std::string error_;
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> overrides_;
    std::vector<Client>* clients_ = nullptr;
    double start_s_ = 0.0;
    std::uint32_t gaze_counter_ = 0;
    bool licence_offered_ = false;
};

}

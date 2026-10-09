#include "tobii/middleware/server.hpp"

#include "tobii/middleware/payloads.hpp"
#include "tobii/ttp/frame.hpp"
#include "tobii/ttp/xds.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tobii::middleware {
namespace {

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

double Clamp01(double v) {
    if (!std::isfinite(v)) return 0.5;
    return std::min(1.0, std::max(0.0, v));
}

constexpr double kEyeOffsetNorm = 0.01;
constexpr double kEyeSeparationMm = 30.0;
constexpr double kEyeDistanceMm = 635.0;
constexpr double kPupilMm = 4.1;

}

struct Server::Client {
    int fd = -1;
    std::vector<std::uint8_t> pending;
    std::vector<std::uint8_t> outbox;
    std::vector<std::uint32_t> subscriptions;
    std::uint32_t event_seq = 1;
    double presence_due_s = 0.0;

    bool Subscribed(std::uint32_t stream) const {
        return std::find(subscriptions.begin(), subscriptions.end(), stream) !=
               subscriptions.end();
    }

    static constexpr std::size_t kMaxBacklogBytes = 64 * 1024;

    void Queue(const std::uint8_t* data, std::size_t len) {
        if (fd < 0) return;
        if (outbox.size() + len > kMaxBacklogBytes) {
            ::close(fd);
            fd = -1;
            outbox.clear();
            return;
        }
        outbox.insert(outbox.end(), data, data + len);
        Flush();
    }

    void Flush() {
        while (fd >= 0 && !outbox.empty()) {
            const ssize_t n = ::send(fd, outbox.data(), outbox.size(), MSG_NOSIGNAL);
            if (n > 0) {
                outbox.erase(outbox.begin(), outbox.begin() + static_cast<std::ptrdiff_t>(n));
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            ::close(fd);
            fd = -1;
            outbox.clear();
            return;
        }
    }
};

std::vector<std::uint8_t> BuildGazeRow(std::uint32_t frame_counter, double elapsed_s,
                                       double gaze_x, double gaze_y, bool valid) {
    gaze_x = Clamp01(gaze_x);
    gaze_y = Clamp01(gaze_y);
    const std::uint32_t flag = valid ? 1u : 0u;
    const double left_x  = Clamp01(gaze_x - kEyeOffsetNorm);
    const double right_x = Clamp01(gaze_x + kEyeOffsetNorm);
    const auto timestamp_us = static_cast<std::int64_t>(elapsed_s * 1000000.0);

    std::vector<std::uint8_t> out{0x00, 0x00};
    ttp::AppendRowHeader(out, 19);

    const auto column = [&out](std::uint32_t id) { ttp::AppendColumnHeader(out, id); };

    column(0x01); ttp::AppendI64(out, timestamp_us);
    column(0x02); ttp::AppendPoint3d(out, -kEyeSeparationMm, 0.0, kEyeDistanceMm);
    column(0x05); ttp::AppendPoint2d(out, left_x, gaze_y);
    column(0x06); ttp::AppendFixed16(out, kPupilMm);
    column(0x07); ttp::AppendU32(out, 0);
    column(0x08); ttp::AppendPoint3d(out, kEyeSeparationMm, 0.0, kEyeDistanceMm);
    column(0x0b); ttp::AppendPoint2d(out, right_x, gaze_y);
    column(0x0c); ttp::AppendFixed16(out, kPupilMm);
    column(0x0d); ttp::AppendU32(out, 0);
    column(0x14); ttp::AppendU32(out, frame_counter);
    column(0x15); ttp::AppendU32(out, flag);
    column(0x16); ttp::AppendU32(out, flag);
    column(0x1b); ttp::AppendU32(out, flag);
    column(0x1c); ttp::AppendPoint2d(out, gaze_x, gaze_y);
    column(0x1d); ttp::AppendU32(out, flag);
    column(0x1e); ttp::AppendU32(out, flag);
    column(0x1f); ttp::AppendU32(out, flag);
    column(0x20); ttp::AppendPoint2d(out, gaze_x, gaze_y);
    column(0x21); ttp::AppendU32(out, flag);
    return out;
}

std::vector<std::uint8_t> BuildPresenceRow(double elapsed_s, bool present) {
    const auto timestamp_us = static_cast<std::int64_t>(elapsed_s * 1000000.0);
    std::vector<std::uint8_t> out{0x00, 0x00};
    ttp::AppendRowHeader(out, 2);
    ttp::AppendColumnHeader(out, 0x01);
    ttp::AppendI64(out, timestamp_us);
    ttp::AppendColumnHeader(out, 0x02);
    const std::uint8_t flag[4] = {0, 0, 0, static_cast<std::uint8_t>(present ? 1 : 0)};
    ttp::AppendTlv(out, 0x01, flag, sizeof(flag));
    return out;
}

std::vector<std::uint8_t> BuildDisplayAreaPayload(double width, double height) {
    std::vector<std::uint8_t> out{0x00, 0x00};
    const double half = width / 2.0;
    ttp::AppendPoint3d(out, -half, height, 0.0);
    ttp::AppendPoint3d(out,  half, height, 0.0);
    ttp::AppendPoint3d(out, -half, 0.0,    0.0);
    ttp::AppendTag(out, 0x010100);
    ttp::AppendU32(out, 0x3039);
    return out;
}

const MiddlewareReply* FindReply(std::uint32_t object) {
    for (const auto& reply : kMiddlewareReplies) {
        if (reply.object == object) return &reply;
    }
    return nullptr;
}

bool ContainsLicence(const std::uint8_t* data, std::size_t len) {
    static const char marker[] = "--SE-SIGNATURE--";
    const std::size_t m = sizeof(marker) - 1;
    for (std::size_t i = 0; i + m <= len; ++i)
        if (std::memcmp(data + i, marker, m) == 0) return true;
    for (std::size_t i = 0; i + 2 * m <= len; ++i) {
        std::size_t k = 0;
        while (k < m && data[i + 2 * k] == static_cast<std::uint8_t>(marker[k]) &&
               data[i + 2 * k + 1] == 0)
            ++k;
        if (k == m) return true;
    }
    return false;
}

Server::Server() : clients_(new std::vector<Client>()) {
    start_s_ = NowSeconds();
}

Server::~Server() {
    Close();
    delete clients_;
}

std::size_t Server::client_count() const { return clients_->size(); }

std::size_t Server::subscriber_count() const {
    std::size_t n = 0;
    for (const auto& c : *clients_) {
        if (!c.subscriptions.empty()) ++n;
    }
    return n;
}

bool Server::TakeLicenceOffered() {
    const bool offered = licence_offered_;
    licence_offered_ = false;
    return offered;
}

bool Server::Listen(const std::string& host, std::uint16_t port) {
    Close();
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0) { error_ = std::string("socket: ") + std::strerror(errno); return false; }

    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        error_ = "bad listen address: " + host;
        Close();
        return false;
    }
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error_ = "bind " + host + ":" + std::to_string(port) + ": " + std::strerror(errno);
        Close();
        return false;
    }
    if (::listen(listen_fd_, 8) != 0) {
        error_ = std::string("listen: ") + std::strerror(errno);
        Close();
        return false;
    }
    return true;
}

void Server::Close() {
    for (const auto& c : *clients_) {
        if (c.fd >= 0) ::close(c.fd);
    }
    clients_->clear();
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
}

void Server::Drop(std::size_t index) {
    if (index >= clients_->size()) return;
    if ((*clients_)[index].fd >= 0) ::close((*clients_)[index].fd);
    clients_->erase(clients_->begin() + static_cast<std::ptrdiff_t>(index));
}

void Server::SendEvent(Client& client, std::uint32_t stream_id,
                                 const std::vector<std::uint8_t>& payload, bool force) {
    if (client.fd < 0) return;
    if (!force && !client.Subscribed(stream_id)) return;
    const auto frame = ttp::BuildEvent(client.event_seq++, stream_id,
                                       payload.data(), payload.size());
    client.Queue(frame.data(), frame.size());
}

void Server::OverrideReply(std::uint32_t object, std::vector<std::uint8_t> payload) {
    for (auto& entry : overrides_) {
        if (entry.first == object) { entry.second = std::move(payload); return; }
    }
    overrides_.emplace_back(object, std::move(payload));
}

void Server::PublishGaze(double gaze_x, double gaze_y, bool valid) {
    const auto payload = BuildGazeRow(gaze_counter_++, NowSeconds() - start_s_,
                                      gaze_x, gaze_y, valid);
    for (std::size_t i = 0; i < clients_->size();) {
        SendEvent((*clients_)[i], kStreamGaze, payload);
        if ((*clients_)[i].fd < 0) Drop(i); else ++i;
    }
}

void Server::PublishPresence(bool present) {
    const auto payload = BuildPresenceRow(NowSeconds() - start_s_, present);
    for (std::size_t i = 0; i < clients_->size();) {
        SendEvent((*clients_)[i], kStreamPresence, payload);
        if ((*clients_)[i].fd < 0) Drop(i); else ++i;
    }
}

void Server::Dispatch(Client& client, const std::uint8_t* frame, std::size_t len) {
    ttp::Header header;
    if (!ttp::ReadHeader(frame, len, header)) return;
    if (header.magic != ttp::kMagicRequest) return;

    const std::uint8_t* payload = frame + ttp::kHeaderSize;
    const std::size_t payload_len = len - ttp::kHeaderSize;

    if (header.object == 0x06a4) {
        client.presence_due_s = NowSeconds() + 0.02;
    }

    if (header.object == 0x04c4) {
        const std::uint32_t stream = ttp::ParseSubscribeStreamId(payload, payload_len);
        if (stream != 0 && !client.Subscribed(stream)) client.subscriptions.push_back(stream);
        if (stream == kStreamPresence) client.presence_due_s = NowSeconds() + 0.05;
    } else if (header.object == 0x04ce) {
        const std::uint32_t stream = ttp::ParseSubscribeStreamId(payload, payload_len);
        client.subscriptions.erase(
            std::remove(client.subscriptions.begin(), client.subscriptions.end(), stream),
            client.subscriptions.end());
    }

    std::vector<std::uint8_t> response;
    const auto override_it = std::find_if(overrides_.begin(), overrides_.end(),
        [&](const auto& entry) { return entry.first == header.object; });

    const MiddlewareReply* reply = FindReply(header.object);
    if (override_it != overrides_.end()) {
        response = ttp::BuildResponse(header.seq, header.object,
                                      override_it->second.data(), override_it->second.size());
    } else if (reply != nullptr) {
        response = ttp::BuildResponse(header.seq, header.object,
                                      reply->size > 0 ? reply->payload : nullptr,
                                      reply->size, reply->status);
    } else {
        response = ttp::BuildResponse(header.seq, header.object, nullptr, 0);
    }

    client.Queue(response.data(), response.size());
}

bool Server::Poll(int timeout_ms) {
    if (listen_fd_ < 0) { error_ = "not listening"; return false; }

    std::vector<pollfd> fds;
    fds.reserve(clients_->size() + 1);
    fds.push_back({listen_fd_, POLLIN, 0});
    for (const auto& c : *clients_) {
        const short events =
            static_cast<short>(POLLIN | (c.outbox.empty() ? 0 : POLLOUT));
        fds.push_back({c.fd, events, 0});
    }

    {
        const double now = NowSeconds();
        for (auto& client : *clients_) {
            if (client.presence_due_s == 0.0 || now < client.presence_due_s) continue;
            client.presence_due_s = 0.0;
            SendEvent(client, kStreamPresence, BuildPresenceRow(now - start_s_, true), true);
        }
    }

    const int ready = ::poll(fds.data(), fds.size(), timeout_ms);
    if (ready < 0) {
        if (errno == EINTR) return true;
        error_ = std::string("poll: ") + std::strerror(errno);
        return false;
    }
    if (ready == 0) return true;

    if ((fds[0].revents & POLLIN) != 0) {
        for (;;) {
            const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd < 0) break;
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            Client client;
            client.fd = fd;
            clients_->push_back(std::move(client));
        }
    }

    for (std::size_t i = clients_->size(); i-- > 0;) {
        const std::size_t slot = i + 1;
        if (slot >= fds.size()) continue;
        const short revents = fds[slot].revents;
        if ((revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) { Drop(i); continue; }
        if ((revents & POLLOUT) != 0) {
            (*clients_)[i].Flush();
            if ((*clients_)[i].fd < 0) { Drop(i); continue; }
        }
        if ((revents & POLLIN) == 0) continue;

        std::uint8_t buf[8192];
        const ssize_t n = ::read((*clients_)[i].fd, buf, sizeof(buf));
        if (n == 0) { Drop(i); continue; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            Drop(i);
            continue;
        }

        auto& pending = (*clients_)[i].pending;
        pending.insert(pending.end(), buf, buf + n);
        if (ContainsLicence(pending.data(), pending.size())) licence_offered_ = true;

        for (;;) {
            ttp::Header header;
            if (!ttp::ReadHeader(pending.data(), pending.size(), header)) break;
            const std::size_t total = ttp::kHeaderSize + header.length;
            if (pending.size() < total) break;
            Dispatch((*clients_)[i], pending.data(), total);
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(total));
            if ((*clients_)[i].fd < 0) break;
        }
        if ((*clients_)[i].fd < 0) Drop(i);
    }
    return true;
}

}

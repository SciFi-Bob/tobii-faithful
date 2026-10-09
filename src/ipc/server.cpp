#include "tobii/ipc/server.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace tobii::ipc {
namespace {

constexpr std::size_t kMaxLineBytes = 64 * 1024;

}

Server::Server() = default;
Server::~Server() { Close(); }

bool Server::Acquire(const std::string& path) {
    const std::string lock_path = path + ".lock";
    const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        error_ = std::string("open ") + lock_path + ": " + std::strerror(errno);
        return false;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        error_ = "another tobiid already holds " + path;
        return false;
    }
    lock_fd_ = fd;
    lock_path_ = lock_path;
    return true;
}

bool Server::Listen(const std::string& path) {
    CloseListener();

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0) { error_ = std::string("socket: ") + std::strerror(errno); return false; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        error_ = "socket path too long: " + path;
        Close();
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    ::unlink(path.c_str());

    const mode_t saved = ::umask(0177);
    const int bound = ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const int bind_errno = errno;
    ::umask(saved);
    if (bound != 0) {
        error_ = std::string("bind ") + path + ": " + std::strerror(bind_errno);
        Close();
        return false;
    }
    ::chmod(path.c_str(), 0600);

    if (::listen(listen_fd_, 8) != 0) {
        error_ = std::string("listen: ") + std::strerror(errno);
        Close();
        return false;
    }
    path_ = path;
    return true;
}

void Server::CloseListener() {
    for (const auto& c : clients_) {
        if (c.fd >= 0) ::close(c.fd);
    }
    clients_.clear();
    pending_.clear();
    outbox_.clear();
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    if (!path_.empty()) { ::unlink(path_.c_str()); path_.clear(); }
}

void Server::Close() {
    CloseListener();
    if (lock_fd_ >= 0) { ::close(lock_fd_); lock_fd_ = -1; }
    lock_path_.clear();
}

std::size_t Server::device_holders() const {
    std::size_t n = 0;
    for (const auto& c : clients_) {
        if (c.holds_device) ++n;
    }
    return n;
}

void Server::Drop(std::size_t index) {
    if (index >= clients_.size()) return;
    if (clients_[index].fd >= 0) ::close(clients_[index].fd);
    clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(index));
    pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
    outbox_.erase(outbox_.begin() + static_cast<std::ptrdiff_t>(index));
}

void Server::Flush(std::size_t index) {
    if (index >= clients_.size()) return;
    Client& client = clients_[index];
    std::string& out = outbox_[index];
    while (client.fd >= 0 && !out.empty()) {
        const ssize_t n = ::send(client.fd, out.data(), out.size(), MSG_NOSIGNAL);
        if (n > 0) {
            out.erase(0, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        ::close(client.fd);
        client.fd = -1;
        out.clear();
        return;
    }
}

void Server::Send(Client& client, const std::string& line) {
    if (client.fd < 0) return;
    for (std::size_t i = 0; i < clients_.size(); ++i) {
        if (&clients_[i] != &client) continue;
        if (outbox_[i].size() + line.size() > kMaxBacklogBytes) {
            ::close(client.fd);
            client.fd = -1;
            outbox_[i].clear();
            return;
        }
        outbox_[i] += line;
        Flush(i);
        return;
    }
}

void Server::BroadcastPose(const std::string& line) {
    for (std::size_t i = 0; i < clients_.size();) {
        if (clients_[i].wants_pose) Send(clients_[i], line);
        if (clients_[i].fd < 0) Drop(i); else ++i;
    }
}

void Server::BroadcastDiagnostics(const std::string& line) {
    for (std::size_t i = 0; i < clients_.size();) {
        if (clients_[i].wants_diagnostics) Send(clients_[i], line);
        if (clients_[i].fd < 0) Drop(i); else ++i;
    }
}

bool Server::any_diagnostics() const {
    for (const auto& client : clients_) {
        if (client.wants_diagnostics) return true;
    }
    return false;
}

bool Server::Poll(int timeout_ms,
                  const std::function<void(Client&, const std::string&)>& on_command) {
    if (listen_fd_ < 0) { error_ = "not listening"; return false; }

    std::vector<pollfd> fds;
    fds.reserve(clients_.size() + 1);
    fds.push_back({listen_fd_, POLLIN, 0});
    for (std::size_t i = 0; i < clients_.size(); ++i) {
        const short events =
            static_cast<short>(POLLIN | (outbox_[i].empty() ? 0 : POLLOUT));
        fds.push_back({clients_[i].fd, events, 0});
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
            clients_.push_back(Client{fd, false, false});
            pending_.emplace_back();
            outbox_.emplace_back();
        }
    }

    for (std::size_t i = clients_.size(); i-- > 0;) {
        const std::size_t slot = i + 1;
        if (slot >= fds.size()) continue;
        const short revents = fds[slot].revents;
        if ((revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) { Drop(i); continue; }
        if ((revents & POLLOUT) != 0) {
            Flush(i);
            if (clients_[i].fd < 0) { Drop(i); continue; }
        }
        if ((revents & POLLIN) == 0) continue;

        char buf[4096];
        const ssize_t n = ::read(clients_[i].fd, buf, sizeof(buf));
        if (n == 0) { Drop(i); continue; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            Drop(i);
            continue;
        }

        pending_[i].append(buf, static_cast<std::size_t>(n));
        if (pending_[i].size() > kMaxLineBytes) { Drop(i); continue; }

        std::size_t start = 0;
        for (;;) {
            const std::size_t nl = pending_[i].find('\n', start);
            if (nl == std::string::npos) break;
            std::string line = pending_[i].substr(start, nl - start);
            start = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && on_command) on_command(clients_[i], line);
        }
        pending_[i].erase(0, start);
        if (clients_[i].fd < 0) Drop(i);
    }
    return true;
}

}

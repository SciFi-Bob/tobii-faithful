#include "tobii/manager/single_instance.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <utility>

namespace tobii::manager {
namespace {

socklen_t MakeAddress(const std::string& name, sockaddr_un& addr) {
    addr = {};
    addr.sun_family = AF_UNIX;
    const std::string full =
        "tobii-faithful/" + std::to_string(::getuid()) + "/" + name;
    const std::size_t len = std::min(full.size(), sizeof(addr.sun_path) - 2);
    std::memcpy(addr.sun_path + 1, full.data(), len);
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + len);
}

}

SingleInstance::SingleInstance(std::string name) : name_(std::move(name)) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    sockaddr_un addr{};
    const socklen_t len = MakeAddress(name_, addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0 ||
        ::listen(fd, 4) != 0) {
        ::close(fd);
        return;
    }
    listen_fd_ = fd;
}

SingleInstance::~SingleInstance() {
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

void SingleInstance::NudgeOwner(const std::string& request) const {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sockaddr_un addr{};
    const socklen_t len = MakeAddress(name_, addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), len) == 0 && !request.empty()) {
        ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    }
    ::close(fd);
}

bool SingleInstance::TakeNudge(std::string* request) {
    if (listen_fd_ < 0) return false;
    bool nudged = false;
    while (true) {
        const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) break;
        nudged = true;
        if (request != nullptr) {
            for (int attempt = 0; attempt < 20; ++attempt) {
                char buf[256];
                const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                if (n > 0) { request->assign(buf, static_cast<std::size_t>(n)); break; }
                if (n == 0) break;
                if (errno != EAGAIN && errno != EWOULDBLOCK) break;
                const timespec ts{0, 1000 * 1000};
                ::nanosleep(&ts, nullptr);
            }
        }
        ::close(fd);
    }
    return nudged;
}

}

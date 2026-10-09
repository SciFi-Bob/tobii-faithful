#include "tobii/ipc/protocol.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

int Connect(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { std::fprintf(stderr, "socket: %s\n", std::strerror(errno)); return -1; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "tobiictl: cannot reach tobiid at %s: %s\n",
                     path.c_str(), std::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

bool SendLine(int fd, const std::string& line) {
    return ::send(fd, line.data(), line.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(line.size());
}

int Pump(int fd, bool once) {
    std::string buffer;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) return n == 0 ? 0 : 1;
        buffer.append(buf, static_cast<std::size_t>(n));
        for (;;) {
            const std::size_t nl = buffer.find('\n');
            if (nl == std::string::npos) break;
            std::printf("%s\n", buffer.substr(0, nl).c_str());
            std::fflush(stdout);
            buffer.erase(0, nl + 1);
            if (once) return 0;
        }
    }
}

void Usage() {
    std::printf(
        "usage: tobiictl [--socket PATH] <command>\n"
        "\n"
        "  status    one status line from the daemon\n"
        "  watch     stream pose, holding the device open\n"
        "  observe   stream pose without holding the device open\n"
        "  recenter  take the current head position as the new centre\n");
}

}

int main(int argc, char** argv) {
    std::string path = tobii::ipc::DefaultSocketPath();
    std::string command;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--socket" && i + 1 < argc) path = argv[++i];
        else if (a == "--help" || a == "-h") { Usage(); return 0; }
        else if (command.empty()) command = a;
        else { std::fprintf(stderr, "unexpected argument: %s\n", a.c_str()); return 2; }
    }
    if (command.empty()) { Usage(); return 2; }

    const int fd = Connect(path);
    if (fd < 0) return 1;

    int rc = 2;
    if (command == "status") {
        rc = SendLine(fd, "{\"cmd\":\"status\"}\n") ? Pump(fd, true) : 1;
    } else if (command == "watch") {
        rc = SendLine(fd, "{\"cmd\":\"subscribe\",\"stream\":\"pose\",\"hold\":true}\n")
             ? Pump(fd, false) : 1;
    } else if (command == "observe") {
        rc = SendLine(fd, "{\"cmd\":\"subscribe\",\"stream\":\"pose\",\"hold\":false}\n")
             ? Pump(fd, false) : 1;
    } else if (command == "recenter") {
        if (SendLine(fd, "{\"cmd\":\"subscribe\",\"stream\":\"pose\",\"hold\":true}\n") &&
            SendLine(fd, "{\"cmd\":\"recenter\"}\n")) {
            struct timespec ts{2, 0};
            nanosleep(&ts, nullptr);
            rc = 0;
        } else {
            rc = 1;
        }
    } else {
        std::fprintf(stderr, "unknown command: %s\n", command.c_str());
        Usage();
    }
    ::close(fd);
    return rc;
}

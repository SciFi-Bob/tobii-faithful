#include "tobii/eventlog.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <string>

namespace tobii::eventlog {
namespace {

class Lock {
public:
    explicit Lock(const std::string& path)
        : fd_(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)) {
        if (fd_ >= 0) ::flock(fd_, LOCK_EX);
    }
    ~Lock() {
        if (fd_ >= 0) ::close(fd_);
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    int fd_;
};

void MakeParents(const std::string& path) {
    for (std::size_t slash = path.find('/', 1); slash != std::string::npos;
         slash = path.find('/', slash + 1)) {
        ::mkdir(path.substr(0, slash).c_str(), 0700);
    }
}

bool ReadAll(const std::string& path, std::string& out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT;
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return true;
}

bool WriteAll(int fd, const std::string& data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

std::string Stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_r(&now, &local);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    return buf;
}

}

std::string DefaultPath() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state != nullptr && *state != '\0') {
        return std::string(state) + "/tobii-faithful/events.log";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::string(home) + "/.local/state/tobii-faithful/events.log";
    }
    return "/tmp/tobii-faithful-events.log";
}

void Record(const std::string& event) { Record(event, DefaultPath(), kMaxBytes); }

void Record(const std::string& event, const std::string& path, std::size_t max_bytes) {
    std::string line = Stamp() + "  " + event;
    for (char& c : line) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    line += '\n';
    if (line.size() > max_bytes) return;

    MakeParents(path);
    Lock lock(path + ".lock");

    struct stat st{};
    const std::size_t size =
        ::stat(path.c_str(), &st) == 0 ? static_cast<std::size_t>(st.st_size) : 0;

    if (size + line.size() <= max_bytes) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd < 0) return;
        WriteAll(fd, line);
        ::close(fd);
        return;
    }

    std::string kept;
    if (!ReadAll(path, kept)) return;
    const std::size_t budget = max_bytes - max_bytes / 10;
    const std::size_t room = budget > line.size() ? budget - line.size() : 0;
    if (kept.size() > room) {
        const std::size_t cut = kept.size() - room;
        const std::size_t newline = kept.find('\n', cut - 1);
        kept.erase(0, newline == std::string::npos ? kept.size() : newline + 1);
    }
    kept += line;

    const std::string temp = path + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    const bool ok = WriteAll(fd, kept);
    ::close(fd);
    if (!ok || ::rename(temp.c_str(), path.c_str()) != 0) ::unlink(temp.c_str());
}

}

#include "tobii/wine/prefix.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tobii::wine {
namespace {

std::string ReadProcFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    std::string out;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
        if (out.size() > 1u << 20) break;
    }
    ::close(fd);
    return out;
}

std::string EnvValue(const std::string& environ_blob, const char* key) {
    const std::string needle = std::string(key) + "=";
    std::size_t pos = 0;
    while (pos < environ_blob.size()) {
        const std::size_t end = environ_blob.find('\0', pos);
        const std::size_t len = (end == std::string::npos ? environ_blob.size() : end) - pos;
        if (environ_blob.compare(pos, needle.size(), needle) == 0) {
            return environ_blob.substr(pos + needle.size(), len - needle.size());
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return {};
}

std::string WineBinaryFrom(const std::string& exe_path) {
    const auto try_root = [](const std::string& root) -> std::string {
        for (const char* name : {"wine", "wine64"}) {
            const std::string candidate = root + "/bin/" + name;
            if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        return {};
    };

    const std::size_t bin = exe_path.rfind("/bin/");
    if (bin != std::string::npos) {
        if (std::string found = try_root(exe_path.substr(0, bin)); !found.empty()) return found;
    }
    const std::size_t lib = exe_path.rfind("/lib/wine/");
    if (lib != std::string::npos) {
        if (std::string found = try_root(exe_path.substr(0, lib)); !found.empty()) return found;
    }
    return {};
}

bool IsWineService(const std::string& cmdline) {
    static const char* kServices[] = {
        "wineserver",
        "services.exe", "winedevice.exe", "plugplay.exe", "explorer.exe",
        "svchost.exe", "rpcss.exe", "lsass.exe", "conhost.exe", "wineboot.exe",
        "tabtip.exe", "start.exe", "winemenubuilder.exe", "rundll32.exe",
    };
    for (const char* service : kServices) {
        if (cmdline.find(service) != std::string::npos) return true;
    }
    return false;
}

bool IsOurHelper(const std::string& cmdline) {
    return cmdline.find("tobii-sesp-bridge") != std::string::npos;
}

}

bool IsGameProcess(const std::string& cmdline) {
    return !cmdline.empty() && !IsWineService(cmdline) && !IsOurHelper(cmdline);
}

std::string ServerDirFor(const std::string& prefix_path) {
    struct stat st{};
    if (::stat(prefix_path.c_str(), &st) != 0) return {};
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/.wine-%u/server-%llx-%llx",
                  static_cast<unsigned>(::getuid()),
                  static_cast<unsigned long long>(st.st_dev),
                  static_cast<unsigned long long>(st.st_ino));
    return buf;
}

std::vector<Prefix> FindPrefixes() {
    std::vector<Prefix> found;
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return found;

    while (const dirent* entry = ::readdir(proc)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        const std::string pid_dir = std::string("/proc/") + entry->d_name;

        const std::string environ_blob = ReadProcFile(pid_dir + "/environ");
        if (environ_blob.empty()) continue;
        const std::string prefix = EnvValue(environ_blob, "WINEPREFIX");
        if (prefix.empty()) continue;

        const std::string cmdline = ReadProcFile(pid_dir + "/cmdline");

        std::string wine_binary;
        {
            char link[4096];
            const ssize_t n = ::readlink((pid_dir + "/exe").c_str(), link, sizeof(link) - 1);
            if (n > 0) {
                link[n] = '\0';
                wine_binary = WineBinaryFrom(link);
            }
        }

        const bool game_process = IsGameProcess(cmdline);

        const auto existing = std::find_if(found.begin(), found.end(),
            [&](const Prefix& p) { return p.path == prefix; });
        if (existing != found.end()) {
            existing->has_game_process = existing->has_game_process || game_process;
            if (existing->wine_binary.empty()) existing->wine_binary = wine_binary;
            continue;
        }

        Prefix p;
        p.path = prefix;
        p.server_dir = ServerDirFor(prefix);
        p.pid = static_cast<pid_t>(std::atoi(entry->d_name));
        p.has_game_process = game_process;
        p.wine_binary = wine_binary;
        found.push_back(std::move(p));
    }
    ::closedir(proc);

    found.erase(std::remove_if(found.begin(), found.end(), [](const Prefix& p) {
        struct stat st{};
        return p.server_dir.empty() || ::stat(p.server_dir.c_str(), &st) != 0;
    }), found.end());

    return found;
}

pid_t LaunchInPrefix(const std::string& prefix_path, const std::string& exe_path,
                     const std::vector<std::string>& args, const std::string& wine_binary,
                     int* stdout_read) {
    int out[2] = {-1, -1};
    if (stdout_read != nullptr) {
        *stdout_read = -1;
        if (::pipe2(out, O_CLOEXEC) != 0) return -1;
    }
    const pid_t pid = ::fork();
    if (pid != 0) {
        if (stdout_read != nullptr) {
            ::close(out[1]);
            if (pid > 0) {
                ::fcntl(out[0], F_SETFL, ::fcntl(out[0], F_GETFL) | O_NONBLOCK);
                *stdout_read = out[0];
            } else {
                ::close(out[0]);
            }
        }
        return pid;
    }

    ::umask(0077);
    ::setenv("WINEPREFIX", prefix_path.c_str(), 1);
    if (::getenv("WINEDEBUG") == nullptr) ::setenv("WINEDEBUG", "-all", 1);

    const int null_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd >= 0) {
        ::dup2(null_fd, STDERR_FILENO);
        if (stdout_read == nullptr) ::dup2(null_fd, STDOUT_FILENO);
    }
    if (stdout_read != nullptr) ::dup2(out[1], STDOUT_FILENO);

    const std::string wine = wine_binary.empty() ? std::string("wine") : wine_binary;
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(wine.c_str()));
    argv.push_back(const_cast<char*>(exe_path.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    ::execvp(wine.c_str(), argv.data());
    std::_Exit(127);
}

}

#include "tobii/wine/prefix.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
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

std::string ReadWholeFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    std::string out;
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
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

std::string WineBinaryBeside(const std::string& exe_path) {
    const std::string_view name = std::string_view(exe_path).substr(exe_path.rfind('/') + 1);
    if (name != "wine" && name != "wine64" && name != "wineserver" &&
        name != "wine-preloader" && name != "wine64-preloader") {
        return {};
    }
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

int PidRank(bool game_process, bool wine_process) {
    return (game_process ? 2 : 0) + (wine_process ? 1 : 0);
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

std::string WineBinaryFrom(const std::string& exe_path) {
    constexpr std::string_view kHostRoot = "/run/host/";
    if (exe_path.rfind(kHostRoot, 0) == 0) {
        if (std::string found = WineBinaryBeside(exe_path.substr(kHostRoot.size() - 1));
            !found.empty()) {
            return found;
        }
    }
    return WineBinaryBeside(exe_path);
}

std::vector<Prefix> GroupByPrefix(const std::vector<ProcessInPrefix>& processes) {
    std::vector<Prefix> found;
    std::vector<int> pid_ranks;
    for (const auto& process : processes) {
        if (process.prefix.empty()) continue;
        const std::string wine_binary = WineBinaryFrom(process.exe);
        const bool game_process = IsGameProcess(process.cmdline);
        const int rank = PidRank(game_process, !wine_binary.empty());

        const std::string server_dir = ServerDirFor(process.prefix);
        const auto existing = std::find_if(found.begin(), found.end(), [&](const Prefix& p) {
            return server_dir.empty() ? p.path == process.prefix : p.server_dir == server_dir;
        });
        if (existing != found.end()) {
            int& existing_rank = pid_ranks[static_cast<std::size_t>(existing - found.begin())];
            if (rank > existing_rank) {
                existing->pid = process.pid;
                existing_rank = rank;
            }
            existing->has_game_process = existing->has_game_process || game_process;
            if (existing->wine_binary.empty()) existing->wine_binary = wine_binary;
            continue;
        }

        Prefix p;
        p.path = process.prefix;
        p.server_dir = server_dir;
        p.pid = process.pid;
        p.has_game_process = game_process;
        p.wine_binary = wine_binary;
        found.push_back(std::move(p));
        pid_ranks.push_back(rank);
    }
    return found;
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
    std::vector<ProcessInPrefix> processes;
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return {};

    while (const dirent* entry = ::readdir(proc)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        const std::string pid_dir = std::string("/proc/") + entry->d_name;

        const std::string environ_blob = ReadProcFile(pid_dir + "/environ");
        if (environ_blob.empty()) continue;
        ProcessInPrefix process;
        process.prefix = EnvValue(environ_blob, "WINEPREFIX");
        if (process.prefix.empty()) continue;

        process.pid = static_cast<pid_t>(std::atoi(entry->d_name));
        process.cmdline = ReadProcFile(pid_dir + "/cmdline");
        char link[4096];
        const ssize_t n = ::readlink((pid_dir + "/exe").c_str(), link, sizeof(link) - 1);
        if (n > 0) process.exe.assign(link, static_cast<std::size_t>(n));
        processes.push_back(std::move(process));
    }
    ::closedir(proc);

    std::vector<Prefix> found = GroupByPrefix(processes);

    found.erase(std::remove_if(found.begin(), found.end(), [](const Prefix& p) {
        struct stat st{};
        return p.server_dir.empty() || ::stat(p.server_dir.c_str(), &st) != 0;
    }), found.end());

    return found;
}

std::string StageForPrefix(const Prefix& prefix, const std::string& exe_path) {
    const std::string seen_from_game =
        "/proc/" + std::to_string(prefix.pid) + "/root" + exe_path;
    if (prefix.pid > 0 && ::access(seen_from_game.c_str(), R_OK) == 0) return exe_path;

    const std::string source = ReadWholeFile(exe_path);
    if (source.empty()) return exe_path;

    const std::size_t slash = exe_path.rfind('/');
    const std::string name = slash == std::string::npos ? exe_path : exe_path.substr(slash + 1);
    const std::string dir = prefix.path + "/drive_c/ProgramData/tobii-faithful";
    const std::string staged = dir + "/" + name;
    if (ReadWholeFile(staged) == source) return staged;

    ::mkdir((prefix.path + "/drive_c/ProgramData").c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    const std::string partial = staged + ".partial";
    const int fd = ::open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (fd < 0) return exe_path;
    const bool written =
        ::write(fd, source.data(), source.size()) == static_cast<ssize_t>(source.size());
    ::close(fd);
    if (!written || ::rename(partial.c_str(), staged.c_str()) != 0) {
        ::unlink(partial.c_str());
        return exe_path;
    }
    return staged;
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

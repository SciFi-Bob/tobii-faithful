#include "tobii/paths.hpp"

#include <array>
#include <limits.h>
#include <unistd.h>

namespace tobii::paths {

std::string FirstExisting(const std::vector<std::string>& candidates) {
    for (const auto& candidate : candidates) {
        if (candidate.empty()) continue;
        if (::access(candidate.c_str(), F_OK) == 0) return candidate;
    }
    return {};
}

std::string ExecutableDir() {
    std::array<char, PATH_MAX> buf{};
    const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return {};
    std::string path(buf.data(), static_cast<std::size_t>(n));
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    return path.substr(0, slash);
}

std::string SiblingDir(const std::string& relative) {
    const std::string dir = ExecutableDir();
    if (dir.empty()) return {};
    const std::size_t slash = dir.find_last_of('/');
    if (slash == std::string::npos) return {};
    return dir.substr(0, slash) + "/" + relative;
}

std::string Describe(const std::vector<std::string>& candidates) {
    std::string out;
    for (const auto& candidate : candidates) {
        if (candidate.empty()) continue;
        if (!out.empty()) out += "\n  ";
        out += candidate;
    }
    return out;
}

}

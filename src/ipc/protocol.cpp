#include "tobii/ipc/protocol.hpp"

#include <cstdlib>
#include <unistd.h>

namespace tobii::ipc {

std::string DefaultSocketPath() {
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && *runtime != '\0') {
        return std::string(runtime) + "/tobiid.sock";
    }
    return "/tmp/tobiid-" + std::to_string(getuid()) + ".sock";
}

std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) out += ' ';
                else out += c;
        }
    }
    return out;
}

std::string JsonField(const std::string& line, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = line.find(needle);
    if (pos == std::string::npos) return {};
    pos = line.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = line.find('"', pos);
    if (pos == std::string::npos) return {};
    const std::size_t end = line.find('"', pos + 1);
    if (end == std::string::npos) return {};
    return line.substr(pos + 1, end - pos - 1);
}

bool JsonBool(const std::string& line, const std::string& key, bool fallback) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = line.find(needle);
    if (pos == std::string::npos) return fallback;
    pos = line.find(':', pos + needle.size());
    if (pos == std::string::npos) return fallback;
    ++pos;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    if (line.compare(pos, 4, "true") == 0) return true;
    if (line.compare(pos, 5, "false") == 0) return false;
    return fallback;
}

}

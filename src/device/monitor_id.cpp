#include "tobii/device/monitor_id.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <glob.h>

namespace tobii::device {
namespace {

bool HasEdidHeader(const std::vector<std::uint8_t>& e) {
    static const std::array<std::uint8_t, 8> kHeader = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    if (e.size() < 128) return false;
    for (std::size_t i = 0; i < kHeader.size(); ++i) {
        if (e[i] != kHeader[i]) return false;
    }
    return true;
}

std::string Manufacturer(const std::vector<std::uint8_t>& e) {
    const unsigned v = (static_cast<unsigned>(e[8]) << 8) | e[9];
    const char c1 = static_cast<char>('A' - 1 + ((v >> 10) & 0x1f));
    const char c2 = static_cast<char>('A' - 1 + ((v >> 5) & 0x1f));
    const char c3 = static_cast<char>('A' - 1 + (v & 0x1f));
    std::string out;
    for (char c : {c1, c2, c3}) {
        if (c >= 'A' && c <= 'Z') out.push_back(c);
    }
    return out;
}

std::string DescriptorSerial(const std::vector<std::uint8_t>& e) {
    for (std::size_t base : {54u, 72u, 90u, 108u}) {
        if (base + 18 > e.size()) break;
        if (e[base] != 0 || e[base + 1] != 0 || e[base + 2] != 0) continue;
        if (e[base + 3] != 0xff) continue;
        std::string s;
        for (std::size_t i = base + 5; i < base + 18; ++i) {
            const std::uint8_t ch = e[i];
            if (ch == 0x0a) break;
            if (ch >= 0x20 && ch < 0x7f) s.push_back(static_cast<char>(ch));
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        if (!s.empty()) return s;
    }
    return {};
}

std::string Sanitize(const std::string& in) {
    std::string out;
    bool last_dash = false;
    for (char c : in) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            out.push_back(static_cast<char>(std::tolower(u)));
            last_dash = false;
        } else if (!last_dash) {
            out.push_back('-');
            last_dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    std::size_t start = 0;
    while (start < out.size() && out[start] == '-') ++start;
    return out.substr(start);
}

}

std::string DecodeEdidMonitorId(const std::vector<std::uint8_t>& edid) {
    if (!HasEdidHeader(edid)) return {};

    const std::string mfg = Manufacturer(edid);
    const unsigned product = (static_cast<unsigned>(edid[11]) << 8) | edid[10];

    std::string serial = DescriptorSerial(edid);
    if (serial.empty()) {
        const std::uint32_t numeric = static_cast<std::uint32_t>(edid[12]) |
                                      (static_cast<std::uint32_t>(edid[13]) << 8) |
                                      (static_cast<std::uint32_t>(edid[14]) << 16) |
                                      (static_cast<std::uint32_t>(edid[15]) << 24);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u", numeric);
        serial = buf;
    }

    char product_hex[8];
    std::snprintf(product_hex, sizeof(product_hex), "%04X", product);

    std::string id = (mfg.empty() ? std::string("---") : mfg);
    id += '-';
    id += product_hex;
    id += '-';
    id += Sanitize(serial);
    return id;
}

std::optional<std::vector<std::uint8_t>> ReadConnectorEdid(const std::string& connector) {
    if (connector.empty()) return std::nullopt;

    const std::string pattern = "/sys/class/drm/card*-" + connector + "/edid";
    glob_t g{};
    const int rc = glob(pattern.c_str(), 0, nullptr, &g);
    if (rc != 0) {
        globfree(&g);
        return std::nullopt;
    }

    std::optional<std::vector<std::uint8_t>> found;
    for (std::size_t i = 0; i < g.gl_pathc; ++i) {
        std::ifstream f(g.gl_pathv[i], std::ios::binary);
        if (!f) continue;
        std::vector<std::uint8_t> bytes(
            (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.size() >= 128) {
            found = std::move(bytes);
            break;
        }
    }
    globfree(&g);
    return found;
}

std::string MonitorIdForConnector(const std::string& connector) {
    if (const auto edid = ReadConnectorEdid(connector)) {
        const std::string id = DecodeEdidMonitorId(*edid);
        if (!id.empty()) return id;
    }
    return Sanitize(connector);
}

}

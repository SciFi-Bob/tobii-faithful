#include "tobii/ttp/frame.hpp"

#include <cstring>

namespace tobii::ttp {
namespace {

std::uint32_t ReadBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  | static_cast<std::uint32_t>(p[3]);
}

void PutBe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

std::vector<std::uint8_t> Build(std::uint32_t magic, std::uint32_t seq, std::uint32_t object,
                                std::uint32_t status, const std::uint8_t* payload,
                                std::size_t len) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + len);
    PutBe32(out, magic);
    PutBe32(out, seq);
    PutBe32(out, 1);
    PutBe32(out, object);
    PutBe32(out, status);
    PutBe32(out, static_cast<std::uint32_t>(len));
    if (len > 0 && payload != nullptr) out.insert(out.end(), payload, payload + len);
    return out;
}

}

bool ReadHeader(const std::uint8_t* data, std::size_t len, Header& out) {
    if (len < kHeaderSize) return false;
    out.magic  = ReadBe32(data + 0);
    out.seq    = ReadBe32(data + 4);
    out.kind   = ReadBe32(data + 8);
    out.object = ReadBe32(data + 12);
    out.status = ReadBe32(data + 16);
    out.length = ReadBe32(data + 20);
    return true;
}

std::vector<std::uint8_t> BuildResponse(std::uint32_t seq, std::uint32_t object,
                                        const std::uint8_t* payload, std::size_t len,
                                        std::uint32_t status) {
    return Build(kMagicResponse, seq, object, status, payload, len);
}

std::vector<std::uint8_t> BuildEvent(std::uint32_t seq, std::uint32_t stream_id,
                                     const std::uint8_t* payload, std::size_t len) {
    return Build(kMagicEvent, seq, stream_id, 0, payload, len);
}

std::uint32_t ParseSubscribeStreamId(const std::uint8_t* payload, std::size_t len) {
    for (std::size_t i = 0; i + 9 <= len; ++i) {
        if (payload[i] != 0x02) continue;
        if (ReadBe32(payload + i + 1) != 4) continue;
        const std::uint32_t value = ReadBe32(payload + i + 5);
        if (value != 0 && value < 0x10000) return value;
    }
    return 0;
}

}

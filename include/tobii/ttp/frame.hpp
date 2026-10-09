#pragma once

#include <cstdint>
#include <vector>

namespace tobii::ttp {

inline constexpr std::size_t kHeaderSize = 24;

inline constexpr std::uint32_t kMagicRequest  = 0x51;
inline constexpr std::uint32_t kMagicResponse = 0x52;
inline constexpr std::uint32_t kMagicEvent    = 0x53;

struct Header {
    std::uint32_t magic  = 0;
    std::uint32_t seq    = 0;
    std::uint32_t kind   = 1;
    std::uint32_t object = 0;
    std::uint32_t status = 0;
    std::uint32_t length = 0;
};

bool ReadHeader(const std::uint8_t* data, std::size_t len, Header& out);

std::vector<std::uint8_t> BuildResponse(std::uint32_t seq, std::uint32_t object,
                                        const std::uint8_t* payload, std::size_t len,
                                        std::uint32_t status = 0);

std::vector<std::uint8_t> BuildEvent(std::uint32_t seq, std::uint32_t stream_id,
                                     const std::uint8_t* payload, std::size_t len);

std::uint32_t ParseSubscribeStreamId(const std::uint8_t* payload, std::size_t len);

}

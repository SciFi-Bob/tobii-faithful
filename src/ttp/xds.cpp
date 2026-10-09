#include "tobii/ttp/xds.hpp"

#include <cmath>

namespace tobii::ttp {
namespace {

void PutBe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void PutBe64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    PutBe32(out, static_cast<std::uint32_t>(v >> 32));
    PutBe32(out, static_cast<std::uint32_t>(v));
}

std::int64_t RoundToInt64(double v) {
    return static_cast<std::int64_t>(v >= 0.0 ? v + 0.5 : v - 0.5);
}

}

void AppendTlv(std::vector<std::uint8_t>& out, std::uint8_t type,
               const std::uint8_t* value, std::size_t len) {
    out.push_back(type);
    PutBe32(out, static_cast<std::uint32_t>(len));
    out.insert(out.end(), value, value + len);
}

void AppendTag(std::vector<std::uint8_t>& out, std::uint32_t tag) {
    out.push_back(0x05);
    PutBe32(out, 4);
    PutBe32(out, tag);
}

void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(0x02);
    PutBe32(out, 4);
    PutBe32(out, value);
}

void AppendI64(std::vector<std::uint8_t>& out, std::int64_t value) {
    out.push_back(0x06);
    PutBe32(out, 8);
    PutBe64(out, static_cast<std::uint64_t>(value));
}

void AppendFixed16(std::vector<std::uint8_t>& out, double value) {
    out.push_back(0x03);
    PutBe32(out, 4);
    PutBe32(out, static_cast<std::uint32_t>(
        static_cast<std::int32_t>(RoundToInt64(value * kFixed16Scale))));
}

void AppendQ42(std::vector<std::uint8_t>& out, double value) {
    out.push_back(0x04);
    PutBe32(out, 8);
    PutBe64(out, static_cast<std::uint64_t>(RoundToInt64(value * kQ42Scale)));
}

void AppendPoint2d(std::vector<std::uint8_t>& out, double x, double y) {
    AppendTag(out, 0x021f40);
    AppendQ42(out, x);
    AppendQ42(out, y);
}

void AppendPoint3d(std::vector<std::uint8_t>& out, double x, double y, double z) {
    AppendTag(out, 0x031f41);
    AppendQ42(out, x);
    AppendQ42(out, y);
    AppendQ42(out, z);
}

void AppendRowHeader(std::vector<std::uint8_t>& out, std::uint32_t columns) {
    AppendTag(out, (columns << 16) | 0x0bb8u);
}

void AppendColumnHeader(std::vector<std::uint8_t>& out, std::uint32_t column_id) {
    AppendTag(out, 0x020bb9u);
    AppendU32(out, column_id);
}

}

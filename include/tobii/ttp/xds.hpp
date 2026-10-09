#pragma once

#include <cstdint>
#include <vector>

namespace tobii::ttp {

inline constexpr double kQ42Scale = 4398046511104.0;
inline constexpr double kFixed16Scale = 65536.0;

void AppendTlv(std::vector<std::uint8_t>& out, std::uint8_t type,
               const std::uint8_t* value, std::size_t len);

void AppendTag(std::vector<std::uint8_t>& out, std::uint32_t tag);
void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value);
void AppendI64(std::vector<std::uint8_t>& out, std::int64_t value);
void AppendFixed16(std::vector<std::uint8_t>& out, double value);
void AppendQ42(std::vector<std::uint8_t>& out, double value);
void AppendPoint2d(std::vector<std::uint8_t>& out, double x, double y);
void AppendPoint3d(std::vector<std::uint8_t>& out, double x, double y, double z);

void AppendRowHeader(std::vector<std::uint8_t>& out, std::uint32_t columns);
void AppendColumnHeader(std::vector<std::uint8_t>& out, std::uint32_t column_id);

}

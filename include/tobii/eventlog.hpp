#pragma once

#include <cstddef>
#include <string>

namespace tobii::eventlog {

inline constexpr std::size_t kMaxBytes = 10u * 1024u * 1024u;

std::string DefaultPath();

void Record(const std::string& event);

void Record(const std::string& event, const std::string& path, std::size_t max_bytes);

}

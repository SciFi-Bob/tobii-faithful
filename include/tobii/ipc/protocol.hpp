#pragma once

#include <string>

namespace tobii::ipc {

std::string DefaultSocketPath();

inline constexpr int kProtocolVersion = 1;

std::string EscapeJson(const std::string& s);

std::string JsonField(const std::string& line, const std::string& key);

bool JsonBool(const std::string& line, const std::string& key, bool fallback);

}

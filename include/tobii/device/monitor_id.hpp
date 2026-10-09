#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tobii::device {

std::string DecodeEdidMonitorId(const std::vector<std::uint8_t>& edid);

std::optional<std::vector<std::uint8_t>> ReadConnectorEdid(const std::string& connector);

std::string MonitorIdForConnector(const std::string& connector);

}

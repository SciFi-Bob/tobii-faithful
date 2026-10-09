#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tobii::device {

struct CalibrationRecord {
    std::string serial;
    std::string monitor_id;

    std::vector<std::uint8_t> blob;
    std::vector<std::uint8_t> stimulus;

    std::uint64_t data_size = 0;
    std::string content_hash;
    std::uint32_t num_points = 9;
    std::int64_t timestamp_unix = 0;

    std::string Key() const;
};

std::string ContentHash(const std::vector<std::uint8_t>& blob);

enum class ReconcileAction {
    kNothing,
    kClear,
    kApply,
};

struct DeviceCalibrationState {
    bool present = false;
    std::uint64_t size = 0;
    std::string hash;
};

ReconcileAction ReconcileDecision(bool have_record, std::uint64_t record_size,
                                  const std::string& record_hash,
                                  const DeviceCalibrationState& device);

class CalibrationStore {
public:
    CalibrationStore();
    explicit CalibrationStore(std::string dir);

    const std::string& dir() const { return dir_; }

    static std::string DefaultDir();

    bool Save(CalibrationRecord rec);

    std::optional<CalibrationRecord> Load(const std::string& serial,
                                          const std::string& monitor_id) const;

private:
    std::string dir_;
    std::string PathFor(const std::string& serial, const std::string& monitor_id) const;
};

}

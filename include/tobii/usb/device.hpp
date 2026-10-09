#pragma once

#include "tobii/usb/decode.hpp"
#include "tobii/usb/protocol.hpp"
#include "tobii/usb/transport.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tobii::usb {

enum class HandshakeOrder {
    kLegacy,
    kOfficial,
};

struct DeviceConfig {
    bool subscribe_images = true;
    int  read_timeout_ms  = 200;
    double handshake_timeout_s = 5.0;
    std::function<bool()> should_stop;
    DisplayCorners display = CollectionPlane();
    HandshakeOrder handshake = HandshakeOrder::kLegacy;
    bool read_device_calibration = false;
    bool read_device_geometry = false;
    bool enabled_eye_both = false;
    std::function<std::vector<std::uint8_t>(const std::string& serial)> calibration_blob;
    std::function<std::unique_ptr<UsbTransport>()> transport_factory;
};

struct DeviceInfo {
    bool valid = false;

    std::string serial;
    std::string model;
    std::string platform;
    std::string firmware;

    std::vector<std::uint8_t> device_info_payload;
    std::vector<std::uint8_t> device_config_payload;
    std::vector<std::uint8_t> capabilities_payload;
    std::vector<std::uint8_t> stream_catalog_payload;

    std::map<std::uint32_t, std::vector<std::uint8_t>> replies;
};

struct DeviceCalibration {
    bool present = false;
    std::vector<std::uint8_t> blob;
    std::vector<std::uint8_t> stimulus;
};

enum class PollResult { kFrame, kTimeout, kError };

class Device {
public:
    Device();
    ~Device();
    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;

    bool Open(const DeviceConfig& cfg = {});

    const DisplayCorners& display_area() const;

    const DisplayCorners& previous_display_area() const;

    const DeviceInfo& device_info() const;

    std::uint32_t opened_realm() const;

    const DeviceCalibration& device_calibration() const;

    const DeviceGeometry& device_geometry() const;

    bool SendRaw(const std::vector<std::uint8_t>& packet);

    bool RequestRaw(const std::vector<std::uint8_t>& packet, std::uint32_t seq,
                    Frame& response);

    bool CalibrationStart();
    bool CalibrationStop();
    bool CalibrationClear();
    bool CalibrationCompute();
    bool CalibrationCollect(double x, double y);
    bool CalibrationDiscard(double x, double y);
    bool CalibrationRetrieve(std::vector<std::uint8_t>& blob);
    bool CalibrationApply(const std::vector<std::uint8_t>& blob);
    bool SetEnabledEye(EnabledEye eye);

    bool Subscribe(std::uint16_t stream);
    bool Unsubscribe(std::uint16_t stream);

    void Close();
    bool is_open() const;

    bool Heartbeat();

    PollResult Poll(const std::function<void(const ImageFrame&)>& on_image,
                    const std::function<void(const GazeSample&)>& on_gaze);

    const std::string& error() const;

    static bool IsPresent();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void DeclineRealm2712(const std::string& serial);

void ClearRealm2712Declines();

}

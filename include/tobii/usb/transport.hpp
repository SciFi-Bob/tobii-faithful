#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace tobii::usb {

inline constexpr int kTransportOk = 0;
inline constexpr int kTransportTimeout = -7;
inline constexpr int kTransportNoDevice = -4;
inline constexpr int kTransportError = -99;

class UsbTransport {
public:
    virtual ~UsbTransport() = default;

    virtual bool Open(std::string& error) = 0;

    virtual void Close() = 0;

    virtual int Control(std::uint8_t request_type, std::uint8_t request,
                        std::uint16_t value, std::uint16_t index, int timeout_ms) = 0;

    virtual int ControlData(std::uint8_t request_type, std::uint8_t request,
                            std::uint16_t value, std::uint16_t index,
                            std::uint8_t* data, std::uint16_t len, int timeout_ms) {
        (void)request_type; (void)request; (void)value; (void)index;
        (void)data; (void)len; (void)timeout_ms;
        return -1;
    }

    virtual int Write(const std::uint8_t* data, std::size_t len, int timeout_ms,
                      int& transferred) = 0;

    virtual int Read(std::uint8_t* buf, std::size_t capacity, int timeout_ms,
                     int& transferred) = 0;

    virtual std::string ErrorName(int err) const = 0;
};

std::unique_ptr<UsbTransport> MakeLibusbTransport();

}

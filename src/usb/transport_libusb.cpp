#include "tobii/usb/protocol.hpp"
#include "tobii/usb/transport.hpp"

#include <libusb.h>

#include <cstdio>

namespace tobii::usb {
namespace {

static_assert(kTransportTimeout == LIBUSB_ERROR_TIMEOUT,
              "the transport's timeout must read as libusb's");
static_assert(kTransportNoDevice == LIBUSB_ERROR_NO_DEVICE,
              "the transport's no-device must read as libusb's");

std::string Describe(const char* context, int err) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s: %s (%d)", context, libusb_error_name(err), err);
    return buf;
}

class LibusbTransport final : public UsbTransport {
public:
    ~LibusbTransport() override { Close(); }

    bool Open(std::string& error) override {
        Close();
        int err = libusb_init(&ctx_);
        if (err != 0) { error = Describe("libusb_init", err); ctx_ = nullptr; return false; }

        handle_ = libusb_open_device_with_vid_pid(ctx_, kVendorId, kProductId);
        if (handle_ == nullptr) {
            error = "device not found (vid 0x2104 pid 0x0313)";
            Close();
            return false;
        }

        err = libusb_set_auto_detach_kernel_driver(handle_, 1);
        if (err != 0 && err != LIBUSB_ERROR_NOT_SUPPORTED) {
            error = Describe("set_auto_detach_kernel_driver", err);
        }

        err = libusb_claim_interface(handle_, kVendorInterface);
        if (err != 0) {
            error = Describe("claim_interface", err);
            Close();
            return false;
        }
        claimed_ = true;
        return true;
    }

    void Close() override {
        if (handle_ != nullptr) {
            if (claimed_) libusb_release_interface(handle_, kVendorInterface);
            libusb_close(handle_);
        }
        if (ctx_ != nullptr) libusb_exit(ctx_);
        handle_ = nullptr;
        ctx_ = nullptr;
        claimed_ = false;
    }

    int Control(std::uint8_t request_type, std::uint8_t request, std::uint16_t value,
                std::uint16_t index, int timeout_ms) override {
        if (handle_ == nullptr) return LIBUSB_ERROR_NO_DEVICE;
        return libusb_control_transfer(handle_, request_type, request, value, index,
                                       nullptr, 0, static_cast<unsigned>(timeout_ms));
    }

    int ControlData(std::uint8_t request_type, std::uint8_t request, std::uint16_t value,
                    std::uint16_t index, std::uint8_t* data, std::uint16_t len,
                    int timeout_ms) override {
        if (handle_ == nullptr) return LIBUSB_ERROR_NO_DEVICE;
        return libusb_control_transfer(handle_, request_type, request, value, index,
                                       data, len, static_cast<unsigned>(timeout_ms));
    }

    int Write(const std::uint8_t* data, std::size_t len, int timeout_ms,
              int& transferred) override {
        transferred = 0;
        if (handle_ == nullptr) return LIBUSB_ERROR_NO_DEVICE;
        return libusb_bulk_transfer(handle_, kEndpointOut, const_cast<std::uint8_t*>(data),
                                    static_cast<int>(len), &transferred,
                                    static_cast<unsigned>(timeout_ms));
    }

    int Read(std::uint8_t* buf, std::size_t capacity, int timeout_ms,
             int& transferred) override {
        transferred = 0;
        if (handle_ == nullptr) return LIBUSB_ERROR_NO_DEVICE;
        return libusb_bulk_transfer(handle_, kEndpointIn, buf, static_cast<int>(capacity),
                                    &transferred, static_cast<unsigned>(timeout_ms));
    }

    std::string ErrorName(int err) const override { return libusb_error_name(err); }

private:
    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    bool claimed_ = false;
};

}

std::unique_ptr<UsbTransport> MakeLibusbTransport() {
    return std::make_unique<LibusbTransport>();
}

}

#include "tobii/usb/device.hpp"
#include "tobii/usb/transport.hpp"

#include <libusb.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace tobii::usb {
namespace {

bool DecodeDisplayArea(const std::vector<std::uint8_t>& payload, DisplayCorners& out) {
    double* corners[3] = {out.top_left, out.top_right, out.bottom_left};
    std::size_t at = 2;
    for (int point = 0; point < 3; ++point) {
        if (at + 9 + 3 * 13 > payload.size()) return false;
        at += 9;
        for (int axis = 0; axis < 3; ++axis) {
            std::int64_t raw = 0;
            for (int b = 0; b < 8; ++b) raw = (raw << 8) | payload[at + 5 + b];
            corners[point][axis] = static_cast<double>(raw) / 4398046511104.0;
            at += 13;
        }
    }
    return true;
}

}

namespace {

constexpr std::size_t kReadBufferBytes = 16384;
constexpr int kControlTimeoutMs = 1000;
constexpr int kRealmResponseTimeoutMs = 3000;
constexpr std::size_t kMaxTransferContentBytes = 4087;
constexpr int kChunkWriteTimeoutMs = 8000;

std::set<std::string>& DeclinedRealmSerials() {
    static std::set<std::string> serials;
    return serials;
}

bool Realm2712Declined(const std::string& serial) {
    return DeclinedRealmSerials().count(serial) != 0;
}

constexpr std::uint8_t kControlRequestType = 0x41;
constexpr std::uint8_t kSessionOpen  = 0x41;
constexpr std::uint8_t kSessionClose = 0x42;
constexpr std::uint8_t kControlInType  = 0xc1;
constexpr std::uint8_t kVendorIdentity = 0x30;
constexpr std::uint8_t kHeartbeat      = 0x46;

}

void DeclineRealm2712(const std::string& serial) { DeclinedRealmSerials().insert(serial); }
void ClearRealm2712Declines() { DeclinedRealmSerials().clear(); }

struct Device::Impl {
    std::unique_ptr<UsbTransport> transport;
    bool session_open = false;
    FrameParser parser;
    std::uint32_t next_seq = 1;
    DeviceConfig cfg;
    DisplayCorners display_area_{};
    DisplayCorners previous_display_area_{};
    DeviceInfo device_info_{};
    std::chrono::steady_clock::time_point deadline{};
    std::string error;
    bool realm_retry_ = false;
    std::uint32_t opened_realm_ = 0;
    DeviceCalibration device_calibration_{};
    DeviceGeometry device_geometry_{};

    std::string Describe(const char* context, int err) const {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s: %s (%d)", context,
                      transport ? transport->ErrorName(err).c_str() : "no transport", err);
        return buf;
    }

    bool Send(const std::vector<std::uint8_t>& packet) {
        int transferred = 0;
        const int err = transport->Write(packet.data(), packet.size(), kControlTimeoutMs,
                                         transferred);
        if (err != 0) { error = Describe("bulk write", err); return false; }
        if (transferred != static_cast<int>(packet.size())) {
            error = "bulk write: short transfer";
            return false;
        }
        return true;
    }

    bool Request(const std::vector<std::uint8_t>& packet, std::uint32_t seq,
                 const char* what, Frame& response, int wait_ms = -1) {
        if (!Send(packet)) { error = std::string(what) + " send: " + error; return false; }
        return AwaitReply(seq, what, response, wait_ms);
    }

    bool RequestChunked(const std::vector<std::uint8_t>& ttp, std::uint32_t seq,
                        const char* what, Frame& response) {
        if (!SendChunked(ttp)) { error = std::string(what) + " send: " + error; return false; }
        return AwaitReply(seq, what, response, -1);
    }

    bool SendChunked(const std::vector<std::uint8_t>& ttp) {
        std::size_t offset = 0;
        do {
            const std::size_t n = std::min(kMaxTransferContentBytes, ttp.size() - offset);
            std::vector<std::uint8_t> xfer(kEnvelopeSize + n, 0);
            xfer[4] = static_cast<std::uint8_t>(n);
            xfer[5] = static_cast<std::uint8_t>(n >> 8);
            xfer[6] = static_cast<std::uint8_t>(n >> 16);
            xfer[7] = static_cast<std::uint8_t>(n >> 24);
            if (n > 0) std::memcpy(xfer.data() + kEnvelopeSize, ttp.data() + offset, n);
            int transferred = 0;
            const int err = transport->Write(xfer.data(), xfer.size(), kChunkWriteTimeoutMs,
                                             transferred);
            if (err != 0) { error = Describe("bulk write", err); return false; }
            if (transferred != static_cast<int>(xfer.size())) {
                error = "bulk write: short transfer";
                return false;
            }
            offset += n;
        } while (offset < ttp.size());
        return true;
    }

    bool AwaitReply(std::uint32_t seq, const char* what, Frame& response, int wait_ms) {
        const auto step_deadline = wait_ms >= 0
            ? std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms)
            : std::chrono::steady_clock::time_point{};
        for (int attempt = 0; attempt < 64; ++attempt) {
            if (parser.Pop(response)) {
                if (response.seq == seq && response.magic != kMagicNotify) return true;
                continue;
            }
            if (cfg.should_stop && cfg.should_stop()) {
                error = std::string(what) + ": stopping";
                return false;
            }
            const auto limit = step_deadline != std::chrono::steady_clock::time_point{}
                ? step_deadline : deadline;
            const auto now = std::chrono::steady_clock::now();
            if (limit != std::chrono::steady_clock::time_point{} && now > limit) {
                error = std::string(what) + ": timed out";
                return false;
            }
            int read_ms = kControlTimeoutMs;
            if (step_deadline != std::chrono::steady_clock::time_point{}) {
                const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                    step_deadline - now);
                if (remain.count() <= 0) {
                    error = std::string(what) + ": timed out";
                    return false;
                }
                if (remain.count() < read_ms) read_ms = static_cast<int>(remain.count());
            }
            std::array<std::uint8_t, kReadBufferBytes> buf{};
            int transferred = 0;
            const int err = transport->Read(buf.data(), buf.size(), read_ms, transferred);
            if (err == kTransportTimeout) continue;
            if (err != 0) { error = Describe(what, err); return false; }
            if (transferred > 0 && !parser.Feed(buf.data(), static_cast<std::size_t>(transferred))) {
                error = std::string(what) + ": frame parser overflow";
                return false;
            }
        }
        error = std::string(what) + ": no response";
        return false;
    }

    void ReadOptional(std::uint32_t op, const char* what, std::uint32_t& seq) {
        const std::string saved_error = error;
        Frame reply;
        if (Request(BuildEmptyRequest(op, seq), seq, what, reply)) {
            device_info_.replies[op] = std::move(reply.payload);
        } else {
            error = saved_error;
        }
        ++seq;
    }

    void ParseDeviceInfo() {
        const auto take = [&](std::uint32_t op, std::vector<std::uint8_t>& into) {
            const auto it = device_info_.replies.find(op);
            if (it != device_info_.replies.end()) into = it->second;
        };
        take(kOpGetDeviceInfo, device_info_.device_info_payload);
        take(kOpGetDeviceConfig, device_info_.device_config_payload);
        take(kOpGetCapabilities, device_info_.capabilities_payload);
        take(kOpGetStreamCatalog, device_info_.stream_catalog_payload);

        const auto& info = device_info_.device_info_payload;
        const std::vector<std::string> fields =
            TlvStrings(info.data(), static_cast<std::uint32_t>(info.size()));
        if (fields.size() > 0) device_info_.serial = fields[0];
        if (fields.size() > 1) device_info_.model = fields[1];
        if (fields.size() > 2) device_info_.platform = fields[2];
        if (fields.size() > 3) device_info_.firmware = fields[3];
        device_info_.valid = !device_info_.serial.empty();
    }

    void ReadDeviceInfo(std::uint32_t& seq) {
        ReadOptional(kOpGetDeviceInfo, "get_device_info", seq);
        ReadOptional(kOpGetDeviceConfig, "get_device_config", seq);
        ReadOptional(kOpGetCapabilities, "get_capabilities", seq);
        ReadOptional(kOpGetStreamCatalog, "get_stream_catalog", seq);
        ParseDeviceInfo();
    }

    bool Handshake() {
        deadline = cfg.handshake_timeout_s > 0.0
            ? std::chrono::steady_clock::now() +
                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::duration<double>(cfg.handshake_timeout_s))
            : std::chrono::steady_clock::time_point{};
        return cfg.handshake == HandshakeOrder::kOfficial ? HandshakeOfficial()
                                                          : HandshakeLegacy();
    }

    bool OpenSuggestedRealm(std::uint32_t& seq, std::uint32_t realm_type) {
        Frame response;
        if (!Request(BuildOpenRealm(seq, realm_type), seq, "open_realm", response)) return false;
        ++seq;
        opened_realm_ = 0;
        return true;
    }

    bool Exchange2712(std::uint32_t& seq) {
        Frame response;
        if (!Request(BuildOpenRealmObject(seq, kRealmObject2712), seq, "open_realm", response)) {
            return false;
        }
        ++seq;

        RealmChallenge challenge;
        if (!ParseRealmChallenge(response.payload.data(),
                                 static_cast<std::uint32_t>(response.payload.size()),
                                 challenge)) {
            error = "realm 0x2712: challenge did not parse";
            return false;
        }

        std::uint8_t digest[16];
        if (!RealmDigest2712(challenge.challenge.data(), challenge.challenge.size(), digest)) {
            error = "realm 0x2712: digest failed";
            return false;
        }

        if (!Request(BuildRealmResponse2712(seq, challenge.realm_id, challenge.field, digest),
                     seq, "realm_response", response, kRealmResponseTimeoutMs)) {
            return false;
        }
        if (response.word16 != 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "realm 0x2712: result %u", response.word16);
            error = buf;
            return false;
        }
        ++seq;

        if (cfg.read_device_geometry) {
            ReadDeviceGeometry(seq);
        }

        if (!Request(BuildRealmFinalize(seq, kRealmObject2712), seq, "realm_finalize", response)) {
            return false;
        }
        ++seq;
        opened_realm_ = kRealmObject2712;
        return true;
    }

    void ReadDeviceCalibration(std::uint32_t& seq) {
        const std::string saved_error = error;

        Frame retrieve;
        if (Request(BuildEmptyRequest(kOpGetCalibration, seq), seq, "get_calibration", retrieve)) {
            device_calibration_.present = true;
            device_calibration_.blob = std::move(retrieve.payload);
        } else {
            error = saved_error;
        }
        ++seq;

        Frame stimulus;
        if (Request(BuildEmptyRequest(kOpGetCalibrationStimulus, seq), seq,
                    "get_calibration_stimulus", stimulus)) {
            device_calibration_.stimulus = std::move(stimulus.payload);
        } else {
            error = saved_error;
        }
        ++seq;

    }

    void ReadDeviceGeometry(std::uint32_t& seq) {
        const std::string saved_error = error;
        Frame reply;
        if (Request(BuildEmptyRequest(kOpGetConfigKey, seq), seq, "get_config_key", reply)) {
            ParseDeviceGeometry(reply.payload.data(),
                                static_cast<std::uint32_t>(reply.payload.size()),
                                device_geometry_);
        } else {
            error = saved_error;
        }
        ++seq;
    }

    bool HandshakeOfficial() {
        Frame response;
        std::uint32_t seq = 1;

        if (!Request(BuildHello(seq), seq, "hello", response)) return false;
        ++seq;

        if (!Request(BuildEmptyRequest(kOpQueryRealm, seq), seq, "query_realm", response)) {
            return false;
        }
        const std::uint32_t realm_type = TlvFirstU32(response.payload.data(),
                                                     static_cast<std::uint32_t>(response.payload.size()));
        ++seq;

        ReadOptional(kOpGetDeviceInfo, "get_device_info", seq);
        for (const std::uint32_t op : kOfficialReadsBeforeDisplay) {
            char what[24];
            std::snprintf(what, sizeof(what), "read_0x%04x", op);
            ReadOptional(op, what, seq);
        }
        ParseDeviceInfo();

        Frame area_reply;
        if (Request(BuildGetDisplayArea(seq), seq, "get_display_area", area_reply)) {
            DecodeDisplayArea(area_reply.payload, previous_display_area_);
        }
        ++seq;

        for (const std::uint32_t op : kOfficialReadsAfterDisplay) {
            char what[24];
            std::snprintf(what, sizeof(what), "read_0x%04x", op);
            ReadOptional(op, what, seq);
        }

        Frame set_reply;
        if (!Request(BuildSetDisplayArea(seq, cfg.display), seq, "set_display_area", set_reply)) {
            return false;
        }
        ++seq;
        display_area_ = cfg.display;

        if (Realm2712Declined(device_info_.serial)) {
            if (!OpenSuggestedRealm(seq, realm_type)) return false;
        } else if (!Exchange2712(seq)) {
            realm_retry_ = true;
            return false;
        }

        if (cfg.read_device_calibration && opened_realm_ == kRealmObject2712) {
            ReadDeviceCalibration(seq);
        }

        if (opened_realm_ == kRealmObject2712) {
            if (cfg.calibration_blob) {
                const std::vector<std::uint8_t> blob = cfg.calibration_blob(device_info_.serial);
                if (!blob.empty()) {
                    Frame reply;
                    RequestChunked(BuildApplyCalibration(seq, blob), seq, "apply_calibration",
                                   reply);
                    ++seq;
                }
            }
            if (cfg.enabled_eye_both) {
                Frame reply;
                Request(BuildSetEnabledEye(seq, EnabledEye::kBoth), seq, "set_enabled_eye", reply);
                ++seq;
            }
        }

        if (!Send(BuildSubscribe(seq, kStreamGaze))) return false;
        ++seq;
        if (!Send(BuildSubscribe(seq, kStreamPresence))) return false;
        ++seq;
        if (cfg.subscribe_images && !Send(BuildSubscribe(seq, kStreamImagePrimary))) return false;
        ++seq;

        next_seq = seq;
        return true;
    }

    bool HandshakeLegacy() {
        Frame response;
        std::uint32_t seq = 1;

        if (!Request(BuildHello(seq), seq, "hello", response)) return false;
        ++seq;

        ReadDeviceInfo(seq);

        if (!Request(BuildQueryRealm(seq), seq, "query_realm", response)) return false;
        const std::uint32_t realm_type = TlvFirstU32(response.payload.data(),
                                                     static_cast<std::uint32_t>(response.payload.size()));
        ++seq;

        if (!Request(BuildOpenRealm(seq, realm_type), seq, "open_realm", response)) return false;
        ++seq;

        if (!Send(BuildSubscribe(seq, kStreamGaze))) return false;
        ++seq;

        Frame set_reply;
        if (!Request(BuildSetDisplayArea(seq, cfg.display), seq,
                     "set_display_area", set_reply)) {
            return false;
        }
        ++seq;

        Frame area_reply;
        if (Request(BuildGetDisplayArea(seq), seq, "get_display_area", area_reply)) {
            DecodeDisplayArea(area_reply.payload, display_area_);
        }
        ++seq;

        if (!Send(BuildSubscribe(seq, kStreamGaze))) return false;
        ++seq;

        if (cfg.subscribe_images && !Send(BuildSubscribe(seq, kStreamImagePrimary))) return false;
        ++seq;

        next_seq = seq;
        return true;
    }
};

Device::Device() : impl_(std::make_unique<Impl>()) {}
Device::~Device() { Close(); }
Device::Device(Device&&) noexcept = default;
Device& Device::operator=(Device&&) noexcept = default;

const std::string& Device::error() const { return impl_->error; }
const DisplayCorners& Device::display_area() const { return impl_->display_area_; }
const DisplayCorners& Device::previous_display_area() const {
    return impl_->previous_display_area_;
}
const DeviceInfo& Device::device_info() const { return impl_->device_info_; }
std::uint32_t Device::opened_realm() const { return impl_->opened_realm_; }

const DeviceCalibration& Device::device_calibration() const {
    return impl_->device_calibration_;
}

const DeviceGeometry& Device::device_geometry() const {
    return impl_->device_geometry_;
}

bool Device::SendRaw(const std::vector<std::uint8_t>& packet) {
    if (!is_open()) { impl_->error = "device not open"; return false; }
    return impl_->Send(packet);
}

bool Device::RequestRaw(const std::vector<std::uint8_t>& packet, std::uint32_t seq,
                        Frame& response) {
    if (!is_open()) { impl_->error = "device not open"; return false; }
    impl_->deadline = {};
    return impl_->Request(packet, seq, "request", response);
}

bool Device::Subscribe(std::uint16_t stream) {
    if (!is_open()) { impl_->error = "device not open"; return false; }
    return impl_->Send(BuildSubscribe(impl_->next_seq++, stream));
}

bool Device::Unsubscribe(std::uint16_t stream) {
    if (!is_open()) { impl_->error = "device not open"; return false; }
    return impl_->Send(BuildUnsubscribe(impl_->next_seq++, stream));
}

namespace {
bool CalibrationReady(const Device& d, std::string& why) {
    if (!d.is_open()) { why = "device not open"; return false; }
    if (d.opened_realm() != kRealmObject2712) { why = "not in realm 0x2712"; return false; }
    return true;
}
}

bool Device::CalibrationStart() {
    std::string why;
    if (!CalibrationReady(*this, why)) { impl_->error = why; return false; }
    impl_->deadline = {};
    const std::uint32_t seq = impl_->next_seq++;
    Frame reply;
    return impl_->Request(BuildEmptyRequest(kOpCalibrationStart, seq), seq,
                          "calibration_start", reply);
}

bool Device::CalibrationStop() {
    std::string why;
    if (!CalibrationReady(*this, why)) { impl_->error = why; return false; }
    impl_->deadline = {};
    const std::uint32_t seq = impl_->next_seq++;
    Frame reply;
    return impl_->Request(BuildEmptyRequest(kOpCalibrationStop, seq), seq,
                          "calibration_stop", reply);
}

bool Device::CalibrationClear() {
    std::string why;
    if (!CalibrationReady(*this, why)) { impl_->error = why; return false; }
    impl_->deadline = {};
    const std::uint32_t seq = impl_->next_seq++;
    Frame reply;
    return impl_->Request(BuildEmptyRequest(kOpCalibrationClear, seq), seq,
                          "calibration_clear", reply);
}

bool Device::CalibrationCompute() {
    std::string why;
    if (!CalibrationReady(*this, why)) { impl_->error = why; return false; }
    impl_->deadline = {};
    const std::uint32_t seq = impl_->next_seq++;
    Frame reply;
    return impl_->Request(BuildEmptyRequest(kOpCalibrationCompute, seq), seq,
                          "calibration_compute", reply);
}

bool Device::CalibrationCollect(double x, double y) {
    std::string why;
    if (!CalibrationReady(*this, why)) { impl_->error = why; return false; }
    impl_->deadline = {};
    const std::uint32_t seq = impl_->next_seq++;
    Frame reply;
    return impl_->Request(BuildCalibrationPoint(kOpCalibrationCollect, seq, x, y), seq,
                          "calibration_collect", reply);
}

bool Device::CalibrationDiscard(double x, double y) {
    std::string why;
    if (!CalibrationReady(*this, why)) { impl_->error = why; return false; }
    impl_->deadline = {};
    const std::uint32_t seq = impl_->next_seq++;
    Frame reply;
    return impl_->Request(BuildCalibrationPoint(kOpCalibrationDiscard, seq, x, y), seq,
                          "calibration_discard", reply);
}

bool Device::CalibrationRetrieve(std::vector<std::uint8_t>& blob) {
    std::string why;
    if (!CalibrationReady(*this, why)) { impl_->error = why; return false; }
    impl_->deadline = {};
    const std::uint32_t seq = impl_->next_seq++;
    Frame reply;
    if (!impl_->Request(BuildEmptyRequest(kOpGetCalibration, seq), seq,
                        "calibration_retrieve", reply)) {
        return false;
    }
    blob = std::move(reply.payload);
    return true;
}

bool Device::CalibrationApply(const std::vector<std::uint8_t>& blob) {
    std::string why;
    if (!CalibrationReady(*this, why)) { impl_->error = why; return false; }
    impl_->deadline = {};
    const std::uint32_t seq = impl_->next_seq++;
    Frame reply;
    return impl_->RequestChunked(BuildApplyCalibration(seq, blob), seq, "calibration_apply",
                                 reply);
}

bool Device::SetEnabledEye(EnabledEye eye) {
    std::string why;
    if (!CalibrationReady(*this, why)) { impl_->error = why; return false; }
    impl_->deadline = {};
    const std::uint32_t seq = impl_->next_seq++;
    Frame reply;
    return impl_->Request(BuildSetEnabledEye(seq, eye), seq, "set_enabled_eye", reply);
}

bool Device::is_open() const { return impl_->transport != nullptr && impl_->session_open; }

bool Device::IsPresent() {
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) return false;
    libusb_device_handle* h = libusb_open_device_with_vid_pid(ctx, kVendorId, kProductId);
    const bool present = h != nullptr;
    if (h != nullptr) libusb_close(h);
    libusb_exit(ctx);
    return present;
}

bool Device::Open(const DeviceConfig& cfg) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        Close();
        impl_->cfg = cfg;
        impl_->error.clear();
        impl_->parser.Clear();
        impl_->device_info_ = {};
        impl_->display_area_ = {};
        impl_->previous_display_area_ = {};
        impl_->realm_retry_ = false;
        impl_->opened_realm_ = 0;
        impl_->device_calibration_ = {};
        impl_->device_geometry_ = {};

        impl_->transport = cfg.transport_factory ? cfg.transport_factory()
                                                 : MakeLibusbTransport();
        if (!impl_->transport->Open(impl_->error)) {
            Close();
            return false;
        }

        const int err = impl_->transport->Control(kControlRequestType, kSessionOpen, 0, 0,
                                                  kControlTimeoutMs);
        if (err < 0) {
            impl_->error = impl_->Describe("session open", err);
            Close();
            return false;
        }
        impl_->session_open = true;

        {
            std::uint8_t req[24] = {0};
            const std::uint32_t words[6] = {1, 0, 1, 4, 0, 0};
            for (int i = 0; i < 6; ++i) {
                req[i * 4 + 0] = static_cast<std::uint8_t>(words[i]);
                req[i * 4 + 1] = static_cast<std::uint8_t>(words[i] >> 8);
                req[i * 4 + 2] = static_cast<std::uint8_t>(words[i] >> 16);
                req[i * 4 + 3] = static_cast<std::uint8_t>(words[i] >> 24);
            }
            impl_->transport->ControlData(kControlRequestType, kVendorIdentity, 0, 0,
                                          req, sizeof(req), kControlTimeoutMs);
            std::uint8_t manifest[512] = {0};
            impl_->transport->ControlData(kControlInType, kVendorIdentity, 0, 0,
                                          manifest, sizeof(manifest), kControlTimeoutMs);
        }

        if (impl_->Handshake()) return true;

        const bool retry = impl_->realm_retry_ && attempt == 0;
        const std::string serial = impl_->device_info_.serial;
        Close();
        if (!retry) return false;
        DeclineRealm2712(serial);
    }
    return false;
}

bool Device::Heartbeat() {
    if (impl_->transport == nullptr) return false;
    std::uint8_t buf[8] = {0};
    return impl_->transport->ControlData(kControlInType, kHeartbeat, 1, 0,
                                         buf, sizeof(buf), kControlTimeoutMs) == 8;
}

void Device::Close() {
    if (impl_->transport != nullptr) {
        if (impl_->session_open) {
            impl_->transport->Control(kControlRequestType, kSessionClose, 0, 0, 500);
        }
        impl_->transport->Close();
    }
    impl_->transport.reset();
    impl_->session_open = false;
    impl_->parser.Clear();
}

PollResult Device::Poll(const std::function<void(const ImageFrame&)>& on_image,
                        const std::function<void(const GazeSample&)>& on_gaze) {
    if (!is_open()) { impl_->error = "device not open"; return PollResult::kError; }

    Frame frame;
    if (!impl_->parser.Pop(frame)) {
        std::array<std::uint8_t, kReadBufferBytes> buf{};
        int transferred = 0;
        const int err = impl_->transport->Read(buf.data(), buf.size(),
                                               impl_->cfg.read_timeout_ms, transferred);
        if (err == kTransportTimeout) return PollResult::kTimeout;
        if (err != 0) {
            impl_->error = impl_->Describe("bulk read", err);
            return PollResult::kError;
        }
        if (transferred <= 0) return PollResult::kTimeout;
        if (!impl_->parser.Feed(buf.data(), static_cast<std::size_t>(transferred))) {
            impl_->error = "frame parser overflow";
            return PollResult::kError;
        }
        if (!impl_->parser.Pop(frame)) return PollResult::kTimeout;
    }

    if (frame.magic != kMagicNotify) return PollResult::kFrame;

    const auto* payload = frame.payload.data();
    const auto len = static_cast<std::uint32_t>(frame.payload.size());
    if (frame.op == kStreamImagePrimary) {
        ImageFrame image;
        if (DecodeImagePayload(frame.op, payload, len, image) && on_image) on_image(image);
    } else if (frame.op == kStreamGaze) {
        GazeSample gaze;
        if (DecodeGazePayload(payload, len, gaze) && on_gaze) on_gaze(gaze);
    }
    return PollResult::kFrame;
}

}

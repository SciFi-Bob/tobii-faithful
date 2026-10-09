#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tobii::usb {

inline constexpr std::uint16_t kVendorId  = 0x2104;
inline constexpr std::uint16_t kProductId = 0x0313;
inline constexpr int kVendorInterface = 0;
inline constexpr std::uint8_t kEndpointIn  = 0x83;
inline constexpr std::uint8_t kEndpointOut = 0x05;

inline constexpr std::size_t kTtpHeaderSize = 24;
inline constexpr std::size_t kEnvelopeSize  = 8;
inline constexpr std::size_t kMaxFrameBytes = 2 * 1024 * 1024;

inline constexpr std::uint32_t kMagicRequest  = 0x51;
inline constexpr std::uint32_t kMagicResponse = 0x52;
inline constexpr std::uint32_t kMagicNotify   = 0x53;

inline constexpr std::uint32_t kOpHello           = 0x03e8;
inline constexpr std::uint32_t kOpSubscribe       = 0x04c4;
inline constexpr std::uint32_t kOpUnsubscribe     = 0x04ce;
inline constexpr std::uint32_t kOpGetDisplayArea  = 0x0596;
inline constexpr std::uint32_t kOpSetDisplayArea  = 0x05a0;
inline constexpr std::uint32_t kOpGetCalibration         = 0x044c;
inline constexpr std::uint32_t kOpGetCalibrationStimulus = 0x0460;

inline constexpr std::uint32_t kOpCalibrationStart   = 0x03f2;
inline constexpr std::uint32_t kOpCalibrationStop    = 0x03fc;
inline constexpr std::uint32_t kOpCalibrationClear   = 0x0424;
inline constexpr std::uint32_t kOpCalibrationCompute = 0x042e;
inline constexpr std::uint32_t kOpCalibrationCollect = 0x0406;
inline constexpr std::uint32_t kOpCalibrationDiscard = 0x0438;
inline constexpr std::uint32_t kOpApplyCalibration   = 0x0456;

enum class EnabledEye : std::uint32_t { kLeft = 1, kRight = 2, kBoth = 3 };
inline constexpr std::uint32_t kOpSetEnabledEye = 0x0c58;

inline constexpr std::uint32_t kOpGetConfigKey = 0x0848;
inline constexpr std::uint32_t kOpGetDeviceInfo    = 0x058c;
inline constexpr std::uint32_t kOpGetDeviceConfig  = 0x0532;
inline constexpr std::uint32_t kOpGetCapabilities  = 0x0546;
inline constexpr std::uint32_t kOpGetStreamCatalog = 0x04b0;

inline constexpr std::uint32_t kOfficialReadsBeforeDisplay[] = {
    0x0532, 0x0546, 0x0547, 0x05d2, 0x05b4, 0x0686, 0x04b0,
};
inline constexpr std::uint32_t kOfficialReadsAfterDisplay[] = {0x0578, 0x083e};

inline constexpr std::uint32_t kOpQueryRealm      = 0x0640;
inline constexpr std::uint32_t kOpOpenRealm       = 0x076c;
inline constexpr std::uint32_t kOpRealmResponse   = 0x0776;

inline constexpr std::uint32_t kRealmObject2712     = 0x2712;
inline constexpr std::uint32_t kOpRealmResponse2712 = 0x0777;
inline constexpr std::uint32_t kOpRealmFinalize     = 0x077b;

inline constexpr std::uint16_t kStreamGaze         = 0x0500;
inline constexpr std::uint16_t kStreamImageAlt     = 0x0501;
inline constexpr std::uint16_t kStreamPresence     = 0x0504;
inline constexpr std::uint16_t kStreamImagePrimary = 0x050e;
inline constexpr std::uint16_t kStreamCounter      = 0x1770;
inline constexpr std::uint16_t kStreamSync         = 0x1771;
inline constexpr std::uint16_t kStreamDeviceLog    = 0x1772;

struct Frame {
    std::uint32_t magic = 0;
    std::uint32_t seq   = 0;
    std::uint32_t op    = 0;
    std::uint32_t kind = 0;
    std::uint32_t word16 = 0;
    std::vector<std::uint8_t> payload;
};

class FrameParser {
public:
    bool Feed(const std::uint8_t* data, std::size_t len);

    bool Pop(Frame& out);

    void Clear() { acc_.clear(); }

private:
    std::vector<std::uint8_t> acc_;
};

std::uint32_t ReadBe32(const std::uint8_t* p);
std::uint64_t ReadBe64(const std::uint8_t* p);

std::vector<std::uint8_t> BuildHello(std::uint32_t seq);
std::vector<std::uint8_t> BuildQueryRealm(std::uint32_t seq);
std::vector<std::uint8_t> BuildGetDisplayArea(std::uint32_t seq);

std::vector<std::uint8_t> BuildEmptyRequest(std::uint32_t op, std::uint32_t seq = 1);

std::vector<std::uint8_t> BuildRequest(std::uint32_t op, std::uint32_t seq,
                                       const std::uint8_t* payload, std::size_t len);

struct DisplayCorners {
    double top_left[3]{};
    double top_right[3]{};
    double bottom_left[3]{};
};
std::vector<std::uint8_t> BuildSetDisplayArea(std::uint32_t seq, const DisplayCorners& c);

DisplayCorners CollectionPlane();
std::vector<std::uint8_t> BuildOpenRealm(std::uint32_t seq, std::uint32_t realm_type);
std::vector<std::uint8_t> BuildRealmResponse(std::uint32_t seq, std::uint32_t realm_id,
                                             std::uint32_t field_210,
                                             const std::uint8_t digest[16]);
std::vector<std::uint8_t> BuildOpenRealmObject(std::uint32_t seq, std::uint32_t object);

std::vector<std::uint8_t> BuildRealmResponse2712(std::uint32_t seq, std::uint32_t realm_id,
                                                 std::uint32_t field,
                                                 const std::uint8_t digest[16]);

std::vector<std::uint8_t> BuildRealmFinalize(std::uint32_t seq, std::uint32_t object);

struct RealmChallenge {
    std::uint32_t realm_id = 0;
    std::uint32_t field = 0;
    std::vector<std::uint8_t> challenge;
};

bool ParseRealmChallenge(const std::uint8_t* payload, std::uint32_t len, RealmChallenge& out);

bool RealmDigest2712(const std::uint8_t* challenge, std::size_t len, std::uint8_t out[16]);

struct DeviceGeometry {
    bool present = false;
    std::vector<double> scalars;
    std::vector<std::array<double, 3>> points;
    std::vector<std::uint8_t> raw;
};

bool ParseDeviceGeometry(const std::uint8_t* payload, std::uint32_t len, DeviceGeometry& out);

std::vector<std::uint8_t> BuildSubscribe(std::uint32_t seq, std::uint16_t stream_id);
std::vector<std::uint8_t> BuildUnsubscribe(std::uint32_t seq, std::uint16_t stream_id);

std::vector<std::uint8_t> BuildCalibrationPoint(std::uint32_t op, std::uint32_t seq,
                                                double x, double y);

std::vector<std::uint8_t> BuildSetEnabledEye(std::uint32_t seq, EnabledEye eye);

std::vector<std::uint8_t> BuildApplyCalibration(std::uint32_t seq,
                                                const std::vector<std::uint8_t>& blob);

std::vector<std::string> TlvStrings(const std::uint8_t* payload, std::uint32_t len);

std::uint32_t TlvFirstU32(const std::uint8_t* payload, std::uint32_t len);
std::uint32_t TlvU32AtIndex(const std::uint8_t* payload, std::uint32_t len, int index);
bool TlvFindChallenge(const std::uint8_t* payload, std::uint32_t len,
                      const std::uint8_t** challenge, std::size_t* challenge_len);

}

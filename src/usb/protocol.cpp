#include "tobii/usb/protocol.hpp"
#include "tobii/usb/realm_secrets.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cstring>

namespace tobii::usb {
namespace {

void PutBe32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

void PutBe64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (56 - 8 * i));
}

void PutLe32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

std::vector<std::uint8_t> Wrap(std::uint32_t seq, std::uint32_t op,
                               const std::uint8_t* payload, std::size_t plen) {
    std::vector<std::uint8_t> ttp(kTtpHeaderSize + plen, 0);
    PutBe32(ttp.data() + 0,  kMagicRequest);
    PutBe32(ttp.data() + 4,  seq);
    PutBe32(ttp.data() + 8,  0);
    PutBe32(ttp.data() + 12, op);
    PutBe32(ttp.data() + 16, 0);
    PutBe32(ttp.data() + 20, static_cast<std::uint32_t>(plen));
    if (plen > 0) std::memcpy(ttp.data() + kTtpHeaderSize, payload, plen);

    std::vector<std::uint8_t> out(kEnvelopeSize + ttp.size(), 0);
    PutLe32(out.data() + 4, static_cast<std::uint32_t>(ttp.size()));
    std::memcpy(out.data() + kEnvelopeSize, ttp.data(), ttp.size());
    return out;
}

void AppendTlvU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    const std::size_t n = out.size();
    out.resize(n + 9);
    out[n] = 0x02;
    PutBe32(out.data() + n + 1, 4);
    PutBe32(out.data() + n + 5, value);
}

void AppendTlvBlob(std::vector<std::uint8_t>& out, const std::uint8_t* data, std::size_t len) {
    const std::size_t n = out.size();
    out.resize(n + 9 + len);
    out[n] = 0x15;
    PutBe32(out.data() + n + 1, static_cast<std::uint32_t>(4 + len));
    PutBe32(out.data() + n + 5, static_cast<std::uint32_t>(len));
    if (len > 0) std::memcpy(out.data() + n + 9, data, len);
}

std::vector<std::uint8_t> BuildStreamOp(std::uint32_t seq, std::uint16_t stream_id,
                                        std::uint32_t op) {
    std::uint8_t payload[] = {
        0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x00, 0x17, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
    };
    payload[9]  = static_cast<std::uint8_t>(stream_id >> 8);
    payload[10] = static_cast<std::uint8_t>(stream_id);
    return Wrap(seq, op, payload, sizeof(payload));
}

class TlvReader {
public:
    TlvReader(const std::uint8_t* buf, std::uint32_t len)
        : buf_(buf), len_(len), pos_(len >= 2 ? 2 : 0) {}

    bool ReadU32(std::uint32_t& value) {
        if (pos_ + 5 > len_) return false;
        if (buf_[pos_] != 0x02) return false;
        const std::uint32_t size = ReadBe32(buf_ + pos_ + 1);
        if (size != 4 || pos_ + 5 + size > len_) return false;
        value = ReadBe32(buf_ + pos_ + 5);
        pos_ += 9;
        return true;
    }

    std::uint32_t pos() const { return pos_; }
    std::uint32_t len() const { return len_; }
    const std::uint8_t* buf() const { return buf_; }

private:
    const std::uint8_t* buf_;
    std::uint32_t len_;
    std::uint32_t pos_;
};

}

std::uint32_t ReadBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  | static_cast<std::uint32_t>(p[3]);
}

std::uint64_t ReadBe64(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(ReadBe32(p)) << 32) | ReadBe32(p + 4);
}

bool FrameParser::Feed(const std::uint8_t* data, std::size_t len) {
    const std::uint8_t* ptr = data;
    std::size_t remaining = len;

    if (acc_.size() >= kEnvelopeSize + kTtpHeaderSize) {
        const std::uint32_t plen = ReadBe32(acc_.data() + kEnvelopeSize + 20);
        const std::size_t frame_size = kEnvelopeSize + kTtpHeaderSize + plen;
        if (acc_.size() < frame_size && len >= kEnvelopeSize &&
            data[0] == 0x01 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x00) {
            ptr = data + kEnvelopeSize;
            remaining = len - kEnvelopeSize;
        }
    }

    if (acc_.size() + remaining > 4 * kMaxFrameBytes) return false;
    acc_.insert(acc_.end(), ptr, ptr + remaining);
    return true;
}

bool FrameParser::Pop(Frame& out) {
    if (acc_.size() < kEnvelopeSize) return false;
    if (acc_[0] != 0x01) { acc_.clear(); return false; }
    if (acc_.size() < kEnvelopeSize + kTtpHeaderSize) return false;

    const std::uint32_t plen = ReadBe32(acc_.data() + kEnvelopeSize + 20);
    const std::size_t frame_size = kEnvelopeSize + kTtpHeaderSize + plen;
    if (frame_size > kMaxFrameBytes) { acc_.clear(); return false; }
    if (acc_.size() < frame_size) return false;

    const std::uint8_t* ttp = acc_.data() + kEnvelopeSize;
    out.magic  = ReadBe32(ttp + 0);
    out.seq    = ReadBe32(ttp + 4);
    out.kind   = ReadBe32(ttp + 8);
    out.op     = ReadBe32(ttp + 12);
    out.word16 = ReadBe32(ttp + 16);
    out.payload.assign(ttp + kTtpHeaderSize, ttp + kTtpHeaderSize + plen);

    acc_.erase(acc_.begin(), acc_.begin() + static_cast<std::ptrdiff_t>(frame_size));
    return true;
}

std::vector<std::uint8_t> BuildHello(std::uint32_t seq) {
    static const std::uint8_t payload[] = {
        0x00, 0x00, 0x17, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x09,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02,
        0x00, 0x01, 0x00, 0x03, 0x00, 0x01, 0x00, 0x04, 0x00, 0x01, 0x00, 0x05,
        0x00, 0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x07, 0x00, 0x01, 0x00, 0x08,
    };
    return Wrap(seq, kOpHello, payload, sizeof(payload));
}

std::vector<std::uint8_t> BuildQueryRealm(std::uint32_t seq) {
    const std::uint8_t payload[] = {0x00, 0x00};
    return Wrap(seq, kOpQueryRealm, payload, sizeof(payload));
}

std::vector<std::uint8_t> BuildEmptyRequest(std::uint32_t op, std::uint32_t seq) {
    return Wrap(seq, op, nullptr, 0);
}

std::vector<std::uint8_t> BuildRequest(std::uint32_t op, std::uint32_t seq,
                                       const std::uint8_t* payload, std::size_t len) {
    return Wrap(seq, op, payload, len);
}

std::vector<std::uint8_t> BuildGetDisplayArea(std::uint32_t seq) {
    return Wrap(seq, kOpGetDisplayArea, nullptr, 0);
}

namespace {

void AppendTlvTag(std::vector<std::uint8_t>& out, std::uint32_t value) {
    const std::size_t n = out.size();
    out.resize(n + 9);
    out[n] = 0x05;
    PutBe32(out.data() + n + 1, 4);
    PutBe32(out.data() + n + 5, value);
}

void AppendTlvQ42(std::vector<std::uint8_t>& out, double value) {
    const double scaled = value * 4398046511104.0;
    const std::int64_t rounded = scaled >= 0.0 ? static_cast<std::int64_t>(scaled + 0.5)
                                               : static_cast<std::int64_t>(scaled - 0.5);
    const std::size_t n = out.size();
    out.resize(n + 13);
    out[n] = 0x04;
    PutBe32(out.data() + n + 1, 8);
    PutBe64(out.data() + n + 5, static_cast<std::uint64_t>(rounded));
}

void AppendTlvPoint3d(std::vector<std::uint8_t>& out, const double p[3]) {
    AppendTlvTag(out, 0x031f41);
    AppendTlvQ42(out, p[0]);
    AppendTlvQ42(out, p[1]);
    AppendTlvQ42(out, p[2]);
}

}

DisplayCorners CollectionPlane() {
    DisplayCorners c;
    c.top_left[0] = -500.0; c.top_left[1] = 500.0; c.top_left[2] = 0.0;
    c.top_right[0] = 500.0; c.top_right[1] = 500.0; c.top_right[2] = 0.0;
    c.bottom_left[0] = -500.0; c.bottom_left[1] = 0.0; c.bottom_left[2] = 0.0;
    return c;
}

std::vector<std::uint8_t> BuildSetDisplayArea(std::uint32_t seq, const DisplayCorners& c) {
    std::vector<std::uint8_t> payload{0x00, 0x00};
    AppendTlvPoint3d(payload, c.top_left);
    AppendTlvPoint3d(payload, c.top_right);
    AppendTlvPoint3d(payload, c.bottom_left);
    AppendTlvTag(payload, 0x010100);
    AppendTlvU32(payload, 0x3039);
    return Wrap(seq, kOpSetDisplayArea, payload.data(), payload.size());
}

std::vector<std::uint8_t> BuildOpenRealm(std::uint32_t seq, std::uint32_t realm_type) {
    std::vector<std::uint8_t> payload{0x00, 0x00};
    AppendTlvU32(payload, realm_type);
    payload.push_back(0x00);
    return Wrap(seq, kOpOpenRealm, payload.data(), payload.size());
}

std::vector<std::uint8_t> BuildRealmResponse(std::uint32_t seq, std::uint32_t realm_id,
                                             std::uint32_t field_210,
                                             const std::uint8_t digest[16]) {
    std::vector<std::uint8_t> payload{0x00, 0x00};
    AppendTlvU32(payload, realm_id);
    AppendTlvU32(payload, field_210);
    payload.insert(payload.end(), digest, digest + 16);
    return Wrap(seq, kOpRealmResponse, payload.data(), payload.size());
}

std::vector<std::uint8_t> BuildSubscribe(std::uint32_t seq, std::uint16_t stream_id) {
    return BuildStreamOp(seq, stream_id, kOpSubscribe);
}

std::vector<std::uint8_t> BuildUnsubscribe(std::uint32_t seq, std::uint16_t stream_id) {
    return BuildStreamOp(seq, stream_id, kOpUnsubscribe);
}

std::vector<std::string> TlvStrings(const std::uint8_t* payload, std::uint32_t len) {
    std::vector<std::string> out;
    std::uint32_t pos = 0;
    while (pos + 9 <= len) {
        if (payload[pos] != 0x14) { ++pos; continue; }
        const std::uint32_t outer = ReadBe32(payload + pos + 1);
        const std::uint32_t inner = ReadBe32(payload + pos + 5);
        if (outer < 4 || outer > len - (pos + 5) || inner != outer - 4 ||
            pos + 9 + inner > len) {
            ++pos;
            continue;
        }
        bool printable = true;
        for (std::uint32_t i = 0; i < inner; ++i) {
            const std::uint8_t c = payload[pos + 9 + i];
            if (c < 0x20 || c >= 0x7f) { printable = false; break; }
        }
        if (!printable) { ++pos; continue; }
        out.emplace_back(reinterpret_cast<const char*>(payload + pos + 9), inner);
        pos += 5 + outer;
    }
    return out;
}

std::uint32_t TlvFirstU32(const std::uint8_t* payload, std::uint32_t len) {
    return TlvU32AtIndex(payload, len, 0);
}

std::uint32_t TlvU32AtIndex(const std::uint8_t* payload, std::uint32_t len, int index) {
    TlvReader reader(payload, len);
    std::uint32_t value = 0;
    for (int i = 0; i <= index; ++i) {
        if (!reader.ReadU32(value)) return 0;
    }
    return value;
}

bool TlvFindChallenge(const std::uint8_t* payload, std::uint32_t len,
                      const std::uint8_t** challenge, std::size_t* challenge_len) {
    TlvReader reader(payload, len);
    std::uint32_t ignored = 0;
    for (int i = 0; i < 2; ++i) {
        if (!reader.ReadU32(ignored)) return false;
    }
    if (reader.pos() >= reader.len()) return false;
    *challenge = reader.buf() + reader.pos();
    *challenge_len = reader.len() - reader.pos();
    return *challenge_len > 0;
}

std::vector<std::uint8_t> BuildCalibrationPoint(std::uint32_t op, std::uint32_t seq,
                                                double x, double y) {
    std::vector<std::uint8_t> payload = {0x00, 0x00};
    AppendTlvQ42(payload, x);
    AppendTlvQ42(payload, y);
    AppendTlvU32(payload, 3);
    return Wrap(seq, op, payload.data(), payload.size());
}

std::vector<std::uint8_t> BuildSetEnabledEye(std::uint32_t seq, EnabledEye eye) {
    std::vector<std::uint8_t> payload = {0x00, 0x00};
    AppendTlvU32(payload, static_cast<std::uint32_t>(eye));
    return Wrap(seq, kOpSetEnabledEye, payload.data(), payload.size());
}

std::vector<std::uint8_t> BuildApplyCalibration(std::uint32_t seq,
                                                const std::vector<std::uint8_t>& blob) {
    std::vector<std::uint8_t> ttp(kTtpHeaderSize + blob.size(), 0);
    PutBe32(ttp.data() + 0,  kMagicRequest);
    PutBe32(ttp.data() + 4,  seq);
    PutBe32(ttp.data() + 8,  0);
    PutBe32(ttp.data() + 12, kOpApplyCalibration);
    PutBe32(ttp.data() + 16, 0);
    PutBe32(ttp.data() + 20, static_cast<std::uint32_t>(blob.size()));
    if (!blob.empty()) std::memcpy(ttp.data() + kTtpHeaderSize, blob.data(), blob.size());
    return ttp;
}

bool ParseDeviceGeometry(const std::uint8_t* payload, std::uint32_t len, DeviceGeometry& out) {
    out = DeviceGeometry{};
    if (len < 2) return false;
    out.present = true;
    out.raw.assign(payload, payload + len);

    std::uint32_t pos = 2;
    bool in_point = false;
    std::array<double, 3> point{};
    int point_axis = 0;

    while (pos + 5 <= len) {
        const std::uint8_t tag = payload[pos];
        const std::uint32_t size = ReadBe32(payload + pos + 1);
        const std::uint32_t value_at = pos + 5;
        if (value_at + size > len) break;
        const std::uint8_t* v = payload + value_at;

        if (tag == 0x05 && size == 4) {
            const std::uint32_t handle = ReadBe32(v);
            in_point = (handle == 0x031f41u);
            point_axis = 0;
        } else if (tag == 0x04 && size == 8) {
            const double q42 = static_cast<double>(static_cast<std::int64_t>(ReadBe64(v))) /
                               4398046511104.0;
            if (in_point) {
                point[static_cast<std::size_t>(point_axis++)] = q42;
                if (point_axis == 3) {
                    out.points.push_back(point);
                    in_point = false;
                    point_axis = 0;
                }
            }
        } else if (tag == 0x03 && size == 4) {
            out.scalars.push_back(
                static_cast<double>(static_cast<std::int32_t>(ReadBe32(v))) / 65536.0);
        }
        pos = value_at + size;
    }
    return true;
}

std::vector<std::uint8_t> BuildOpenRealmObject(std::uint32_t seq, std::uint32_t object) {
    std::vector<std::uint8_t> payload = {0x00, 0x00};
    AppendTlvU32(payload, object);
    const std::uint8_t field17[] = {0x17, 0x00, 0x00, 0x00, 0x08,
                                    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
    payload.insert(payload.end(), field17, field17 + sizeof(field17));
    return Wrap(seq, kOpOpenRealm, payload.data(), payload.size());
}

std::vector<std::uint8_t> BuildRealmResponse2712(std::uint32_t seq, std::uint32_t realm_id,
                                                 std::uint32_t field,
                                                 const std::uint8_t digest[16]) {
    std::vector<std::uint8_t> payload = {0x00, 0x00};
    AppendTlvU32(payload, realm_id);
    AppendTlvU32(payload, field);
    AppendTlvBlob(payload, digest, 16);
    std::uint8_t trailer[4];
    CopyRealmResponseTrailer(trailer);
    AppendTlvBlob(payload, trailer, sizeof(trailer));
    return Wrap(seq, kOpRealmResponse2712, payload.data(), payload.size());
}

std::vector<std::uint8_t> BuildRealmFinalize(std::uint32_t seq, std::uint32_t object) {
    std::vector<std::uint8_t> payload = {0x00, 0x00};
    AppendTlvU32(payload, object);
    return Wrap(seq, kOpRealmFinalize, payload.data(), payload.size());
}

bool ParseRealmChallenge(const std::uint8_t* payload, std::uint32_t len, RealmChallenge& out) {
    std::uint32_t pos = 2;
    std::uint32_t u32s[2] = {0, 0};
    int seen_u32 = 0;
    while (pos + 5 <= len) {
        const std::uint8_t tag = payload[pos];
        const std::uint32_t vlen = ReadBe32(payload + pos + 1);
        if (vlen > len - (pos + 5)) return false;
        const std::uint8_t* value = payload + pos + 5;
        if (tag == 0x02 && vlen == 4 && seen_u32 < 2) {
            u32s[seen_u32++] = ReadBe32(value);
        } else if (tag == 0x15 && vlen >= 4 && seen_u32 == 2) {
            const std::uint32_t inner = ReadBe32(value);
            if (inner != vlen - 4) return false;
            out.realm_id = u32s[0];
            out.field = u32s[1];
            out.challenge.assign(value + 4, value + 4 + inner);
            return !out.challenge.empty();
        }
        pos += 5 + vlen;
    }
    return false;
}

bool RealmDigest2712(const std::uint8_t* challenge, std::size_t len, std::uint8_t out[16]) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    std::uint8_t key[16];
    CopyRealm2712Key(key);
    if (HMAC(EVP_md5(), key, static_cast<int>(sizeof(key)),
             challenge, len, digest, &digest_len) == nullptr || digest_len != 16) {
        return false;
    }
    std::memcpy(out, digest, 16);
    return true;
}

}

#include "tobii/usb/decode.hpp"

#include "tobii/usb/protocol.hpp"

#include <cstring>

namespace tobii::usb {
namespace {

std::int64_t ReadBeI64(const std::uint8_t* p) {
    return static_cast<std::int64_t>(ReadBe64(p));
}

class TlvReader {
public:
    TlvReader(const std::uint8_t* buf, std::uint32_t len, std::uint32_t pos)
        : buf_(buf), len_(len), pos_(pos) {}

    bool Header(std::uint8_t& type, std::uint32_t& size) {
        if (len_ - pos_ < 5) return false;
        type = buf_[pos_];
        size = ReadBe32(buf_ + pos_ + 1);
        pos_ += 5;
        return len_ - pos_ >= size;
    }

    bool Tag(std::uint32_t& tag) {
        std::uint8_t type = 0; std::uint32_t size = 0;
        if (!Header(type, size) || type != 5 || size != 4) return false;
        tag = ReadBe32(buf_ + pos_);
        pos_ += 4;
        return true;
    }

    bool U32(std::uint32_t& value) {
        std::uint8_t type = 0; std::uint32_t size = 0;
        if (!Header(type, size) || type != 2 || size != 4) return false;
        value = ReadBe32(buf_ + pos_);
        pos_ += 4;
        return true;
    }

    bool S64AsDouble(double& value) {
        std::uint8_t type = 0; std::uint32_t size = 0;
        if (!Header(type, size) || type != 6 || size != 8) return false;
        value = static_cast<double>(ReadBeI64(buf_ + pos_));
        pos_ += 8;
        return true;
    }

    bool Fixed16x16(double& value) {
        std::uint8_t type = 0; std::uint32_t size = 0;
        if (!Header(type, size) || type != 3 || size != 4) return false;
        value = static_cast<double>(static_cast<std::int32_t>(ReadBe32(buf_ + pos_))) / 65536.0;
        pos_ += 4;
        return true;
    }

    bool Q42(double& value) {
        std::uint8_t type = 0; std::uint32_t size = 0;
        if (!Header(type, size) || type != 4 || size != 8) return false;
        value = static_cast<double>(ReadBeI64(buf_ + pos_)) / 4398046511104.0;
        pos_ += 8;
        return true;
    }

    bool Row(std::uint32_t& count) {
        std::uint32_t tag = 0;
        if (!Tag(tag) || (tag & 0xffffu) != 0x0bb8u) return false;
        count = (tag >> 16) & 0xfffu;
        return true;
    }

    bool Column(std::uint32_t& column) {
        std::uint32_t tag = 0;
        if (!Tag(tag) || tag != 0x020bb9u) return false;
        return U32(column);
    }

    bool Point2d(double& x, double& y) {
        std::uint32_t tag = 0;
        if (!Tag(tag) || tag != 0x021f40u) return false;
        return Q42(x) && Q42(y);
    }

    bool Point3d(double& x, double& y, double& z) {
        std::uint32_t tag = 0;
        if (!Tag(tag) || tag != 0x031f41u) return false;
        return Q42(x) && Q42(y) && Q42(z);
    }

    bool SkipKnown(std::uint32_t column) {
        double x = 0.0, y = 0.0, z = 0.0;
        std::uint32_t u = 0;
        switch (column) {
            case 0x01:
                return S64AsDouble(x);
            case 0x02: case 0x03: case 0x04: case 0x08: case 0x09: case 0x0a:
            case 0x17: case 0x18: case 0x22: case 0x24: case 0x25: case 0x27:
                return Point3d(x, y, z);
            case 0x05: case 0x0b: case 0x19: case 0x1a: case 0x1c: case 0x20:
                return Point2d(x, y);
            case 0x06: case 0x0c: case 0x29: case 0x2b:
                return Fixed16x16(x);
            case 0x07: case 0x0d: case 0x0e: case 0x11: case 0x14: case 0x15:
            case 0x16: case 0x1b: case 0x1d: case 0x1e: case 0x1f: case 0x21:
            case 0x23: case 0x26: case 0x28: case 0x2a: case 0x2c:
                return U32(u);
            default:
                return false;
        }
    }

    std::uint32_t pos() const { return pos_; }
    std::uint32_t len() const { return len_; }

private:
    const std::uint8_t* buf_;
    std::uint32_t len_;
    std::uint32_t pos_;
};

struct FieldView {
    std::uint8_t type = 0;
    std::uint32_t len = 0;
    const std::uint8_t* value = nullptr;
};

bool NextField(const std::uint8_t* payload, std::uint32_t payload_len,
               std::uint32_t& offset, FieldView& field) {
    if (offset + 5 > payload_len) return false;
    field.type = payload[offset];
    field.len = ReadBe32(payload + offset + 1);
    if (field.len > payload_len - offset - 5) return false;
    field.value = payload + offset + 5;
    offset += 5 + field.len;
    return true;
}

}

bool DecodeImagePayload(std::uint32_t object_id, const std::uint8_t* payload,
                        std::uint32_t len, ImageFrame& out) {
    out = ImageFrame{};
    out.object_id = object_id;
    if (len < 2) return false;

    const bool primary = object_id == kStreamImagePrimary;
    std::uint32_t offset = 2;
    const std::uint8_t* image_value = nullptr;
    std::uint32_t image_value_len = 0;

    while (offset + 5 <= len) {
        FieldView marker, id_field, value_field;
        if (!NextField(payload, len, offset, marker) || marker.type != 0x05) return false;
        if (!NextField(payload, len, offset, id_field)) return false;
        if (id_field.type == 0x05) {
            if (!NextField(payload, len, offset, id_field)) return false;
        }
        if (id_field.type != 0x02 || id_field.len != 4) return false;
        if (!NextField(payload, len, offset, value_field)) return false;

        const std::uint32_t column = ReadBe32(id_field.value);
        if (column == 0x0001 && value_field.len == 8) {
            out.timestamp = ReadBe64(value_field.value);
        } else if (column == 0x0002 && value_field.len == 4) {
            out.bpp = ReadBe32(value_field.value);
        } else if (column == 0x0003 && value_field.len == 4) {
            out.width = ReadBe32(value_field.value);
        } else if (primary && column == 0x0004 && value_field.len == 4) {
            out.stride = ReadBe32(value_field.value);
        } else if (primary && column == 0x0005 && value_field.len == 4) {
            out.height = ReadBe32(value_field.value);
        } else if (!primary && column == 0x0004 && value_field.len == 4) {
            out.height = ReadBe32(value_field.value);
        } else if (column == (primary ? 0x0006u : 0x0005u)) {
            image_value = value_field.value;
            image_value_len = value_field.len;
        }
    }

    if (out.stride == 0) out.stride = out.width;
    if (image_value == nullptr || image_value_len < 4) return false;

    const std::uint32_t image_len = ReadBe32(image_value);
    const std::uint64_t required = static_cast<std::uint64_t>(out.stride) * out.height;
    if (image_len > image_value_len - 4 || out.width == 0 || out.height == 0 ||
        out.stride == 0 || out.width > out.stride || image_len < required) {
        return false;
    }

    out.pixels.assign(image_value + 4, image_value + 4 + required);
    return true;
}

bool DecodeGazePayload(const std::uint8_t* payload, std::uint32_t len, GazeSample& out) {
    out = GazeSample{};
    if (len < 2) return false;

    TlvReader reader(payload, len, 2);
    std::uint32_t columns = 0;
    if (!reader.Row(columns)) return false;

    for (std::uint32_t i = 0; i < columns && reader.pos() < reader.len(); ++i) {
        std::uint32_t column = 0;
        if (!reader.Column(column)) return true;
        switch (column) {
            case 0x01: {
                double v = 0.0;
                if (!reader.S64AsDouble(v)) return true;
                out.timestamp_us = static_cast<std::int64_t>(v);
                out.has_timestamp = true;
                break;
            }
            case 0x02:
                if (!reader.Point3d(out.eye_left_x, out.eye_left_y, out.eye_left_z)) return true;
                out.has_left_origin = true;
                break;
            case 0x07:
                if (!reader.U32(out.validity_left)) return true;
                break;
            case 0x08:
                if (!reader.Point3d(out.eye_right_x, out.eye_right_y, out.eye_right_z)) return true;
                out.has_right_origin = true;
                break;
            case 0x0d:
                if (!reader.U32(out.validity_right)) return true;
                break;
            case 0x14:
                if (!reader.U32(out.frame_counter)) return true;
                out.has_frame_counter = true;
                break;
            case 0x15:
                if (!reader.U32(out.eye_present_right)) return true;
                break;
            case 0x16:
                if (!reader.U32(out.eye_present_left)) return true;
                break;
            case 0x1b:
                if (!reader.U32(out.combined_valid)) return true;
                break;
            case 0x1c:
                if (!reader.Point2d(out.gaze_x, out.gaze_y)) return true;
                out.has_gaze = true;
                break;
            case 0x1d:
                if (!reader.U32(out.gaze_valid)) return true;
                break;
            case 0x20:
                if (!reader.Point2d(out.gaze_unfiltered_x, out.gaze_unfiltered_y)) return true;
                out.has_gaze_unfiltered = true;
                break;
            default:
                if (!reader.SkipKnown(column)) return true;
                break;
        }
    }
    return true;
}

}

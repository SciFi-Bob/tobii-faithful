#include "sesp_codec.h"

#include <stdlib.h>
#include <string.h>

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void write_le_float(unsigned char *p, float v)
{
    uint32_t raw = 0;
    memcpy(&raw, &v, sizeof(raw));
    write_le32(p, raw);
}

static void write_le16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

int sesp_read_root_type(const unsigned char *payload, uint32_t payload_len,
                        uint32_t *seq, uint8_t *type)
{
    if (payload_len < 24)
        return 0;
    uint32_t root_off = read_le32(payload);
    if (root_off > payload_len - 16)
        return 0;
    uint32_t obj = root_off;
    int32_t soff = (int32_t)read_le32(payload + obj);
    int64_t vt64 = (int64_t)obj - (int64_t)soff;
    if (vt64 < 0 || vt64 + 10 > payload_len)
        return 0;

    const unsigned char *vt = payload + vt64;
    uint16_t vt_len = (uint16_t)(vt[0] | (vt[1] << 8));
    if (vt_len < 10 || vt64 + vt_len > payload_len)
        return 0;

    uint16_t seq_off = (uint16_t)(vt[4] | (vt[5] << 8));
    uint16_t type_off = (uint16_t)(vt[6] | (vt[7] << 8));
    if (!seq_off || !type_off || obj + seq_off + 4 > payload_len || obj + type_off >= payload_len)
        return 0;

    *seq = read_le32(payload + obj + seq_off);
    *type = payload[obj + type_off];
    return 1;
}

unsigned char *sesp_make_frame(const unsigned char *payload, uint32_t payload_len,
                               uint32_t *out_len)
{
    uint32_t frame_len = 12 + payload_len;
    unsigned char *out = (unsigned char *)calloc(frame_len, 1);
    if (!out)
        return NULL;

    memcpy(out, "sesp", 4);
    write_le32(out + 4, payload_len);
    write_le32(out + 8, payload_len ^ 0x70736573U);
    memcpy(out + 12, payload, payload_len);

    *out_len = frame_len;
    return out;
}

unsigned char *sesp_make_response_initialize(uint32_t seq, uint32_t *out_len)
{
    const uint32_t payload_len = 76;
    const uint32_t wrapper_obj = 4;
    const uint32_t inner_obj = 20;
    const uint32_t inner_vt = 40;
    const uint32_t wrapper_vt = 66;
    unsigned char payload[76];
    memset(payload, 0, sizeof(payload));

    write_le32(payload + 0, wrapper_obj);

    write_le32(payload + wrapper_obj + 0, (uint32_t)(int32_t)((int32_t)wrapper_obj - (int32_t)wrapper_vt));
    write_le32(payload + wrapper_obj + 4, seq);
    write_le32(payload + wrapper_obj + 8, inner_obj - (wrapper_obj + 8));
    payload[wrapper_obj + 12] = 3;

    write_le32(payload + inner_obj + 0, (uint32_t)(int32_t)((int32_t)inner_obj - (int32_t)inner_vt));
    for (uint32_t i = 1; i <= 15; ++i)
        payload[inner_obj + 3 + i] = 1;

    write_le16(payload + inner_vt + 0, 36);
    write_le16(payload + inner_vt + 2, 20);
    write_le16(payload + inner_vt + 4, 0);
    for (uint16_t i = 1; i <= 15; ++i)
        write_le16(payload + inner_vt + 4 + i * 2, (uint16_t)(3 + i));

    write_le16(payload + wrapper_vt + 0, 10);
    write_le16(payload + wrapper_vt + 2, 13);
    write_le16(payload + wrapper_vt + 4, 4);
    write_le16(payload + wrapper_vt + 6, 12);
    write_le16(payload + wrapper_vt + 8, 8);

    return sesp_make_frame(payload, payload_len, out_len);
}

unsigned char *sesp_make_status_response(uint32_t seq, uint8_t reply_type, uint32_t *out_len)
{
    const uint32_t payload_len = 42;
    const uint32_t wrapper_obj = 4;
    const uint32_t inner_obj = 20;
    const uint32_t inner_vt = 24;
    const uint32_t wrapper_vt = 32;
    unsigned char payload[42];
    memset(payload, 0, sizeof(payload));

    write_le32(payload + 0, wrapper_obj);

    write_le32(payload + wrapper_obj + 0, (uint32_t)(int32_t)((int32_t)wrapper_obj - (int32_t)wrapper_vt));
    write_le32(payload + wrapper_obj + 4, seq);
    write_le32(payload + wrapper_obj + 8, inner_obj - (wrapper_obj + 8));
    payload[wrapper_obj + 12] = reply_type;

    write_le32(payload + inner_obj + 0, (uint32_t)(int32_t)((int32_t)inner_obj - (int32_t)inner_vt));

    write_le16(payload + inner_vt + 0, 6);
    write_le16(payload + inner_vt + 2, 4);
    write_le16(payload + inner_vt + 4, 0);

    write_le16(payload + wrapper_vt + 0, 10);
    write_le16(payload + wrapper_vt + 2, 13);
    write_le16(payload + wrapper_vt + 4, 4);
    write_le16(payload + wrapper_vt + 6, 12);
    write_le16(payload + wrapper_vt + 8, 8);

    return sesp_make_frame(payload, payload_len, out_len);
}

unsigned char *sesp_make_headpose(uint32_t seq, const struct SespHeadpose *pose,
                                  uint32_t *out_len)
{
    const uint32_t wrapper_obj = 4;
    const uint32_t inner_obj = 20;
    const uint32_t inner_obj_size = 52;
    const uint32_t inner_vt = inner_obj + inner_obj_size;
    const uint32_t wrapper_vt = inner_vt + 28;
    const uint32_t payload_len = wrapper_vt + 10;
    unsigned char payload[110];
    memset(payload, 0, sizeof(payload));

    write_le32(payload + 0, wrapper_obj);

    write_le32(payload + wrapper_obj + 0, (uint32_t)(int32_t)((int32_t)wrapper_obj - (int32_t)wrapper_vt));
    write_le32(payload + wrapper_obj + 4, seq);
    write_le32(payload + wrapper_obj + 8, inner_obj - (wrapper_obj + 8));
    payload[wrapper_obj + 12] = 8;

    write_le32(payload + inner_obj + 0, (uint32_t)(int32_t)((int32_t)inner_obj - (int32_t)inner_vt));
    write_le32(payload + inner_obj + 4, (uint32_t)(pose->timestamp_us >> 32));
    write_le32(payload + inner_obj + 8, (uint32_t)pose->timestamp_us);
    write_le_float(payload + inner_obj + 12, pose->pos[0]);
    write_le_float(payload + inner_obj + 16, pose->pos[1]);
    write_le_float(payload + inner_obj + 20, pose->pos[2]);
    write_le_float(payload + inner_obj + 24, pose->rot[0]);
    write_le_float(payload + inner_obj + 28, pose->rot[1]);
    write_le_float(payload + inner_obj + 32, pose->rot[2]);
    write_le32(payload + inner_obj + 36, (uint32_t)pose->pos_valid);
    write_le32(payload + inner_obj + 40, (uint32_t)pose->rot_valid[0]);
    write_le32(payload + inner_obj + 44, (uint32_t)pose->rot_valid[1]);
    write_le32(payload + inner_obj + 48, (uint32_t)pose->rot_valid[2]);

    write_le16(payload + inner_vt + 0, 28);
    write_le16(payload + inner_vt + 2, (uint16_t)inner_obj_size);
    for (uint16_t i = 0; i < 12; ++i)
        write_le16(payload + inner_vt + 4 + i * 2, (uint16_t)(4 + i * 4));

    write_le16(payload + wrapper_vt + 0, 10);
    write_le16(payload + wrapper_vt + 2, 13);
    write_le16(payload + wrapper_vt + 4, 4);
    write_le16(payload + wrapper_vt + 6, 12);
    write_le16(payload + wrapper_vt + 8, 8);

    return sesp_make_frame(payload, payload_len, out_len);
}

int sesp_contains_licence(const unsigned char *data, uint32_t len)
{
    static const char marker[] = "--SE-SIGNATURE--";
    const uint32_t m = (uint32_t)(sizeof(marker) - 1);

    for (uint32_t i = 0; i + m <= len; ++i)
        if (memcmp(data + i, marker, m) == 0)
            return 1;
    for (uint32_t i = 0; i + 2 * m <= len; ++i) {
        uint32_t k = 0;
        while (k < m && data[i + 2 * k] == (unsigned char)marker[k] && data[i + 2 * k + 1] == 0)
            ++k;
        if (k == m)
            return 1;
    }
    return 0;
}

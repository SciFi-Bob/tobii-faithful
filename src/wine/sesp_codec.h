#ifndef TOBII_SESP_CODEC_H
#define TOBII_SESP_CODEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SespHeadpose {
    uint64_t timestamp_us;
    float pos[3];
    float rot[3];
    int32_t pos_valid;
    int32_t rot_valid[3];
};

int sesp_read_root_type(const unsigned char *payload, uint32_t payload_len,
                        uint32_t *seq, uint8_t *type);

unsigned char *sesp_make_frame(const unsigned char *payload, uint32_t payload_len,
                               uint32_t *out_len);

unsigned char *sesp_make_response_initialize(uint32_t seq, uint32_t *out_len);

unsigned char *sesp_make_status_response(uint32_t seq, uint8_t reply_type, uint32_t *out_len);

unsigned char *sesp_make_headpose(uint32_t seq, const struct SespHeadpose *pose,
                                  uint32_t *out_len);

int sesp_contains_licence(const unsigned char *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif

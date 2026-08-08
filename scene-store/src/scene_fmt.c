/* scene_fmt.c — byte-level wire helpers (little-endian, FNV-1a 32). */
#include "scene_fmt.h"

uint32_t scene_fnv1a32(const uint8_t *data, size_t len)
{
    uint32_t h = UINT32_C(2166136261);
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= data[i];
        h *= UINT32_C(16777619);
    }
    return h;
}

int scene_frame_check(const scene_frame_header *h, const uint8_t *frame,
                      uint32_t frame_len)
{
    if (!h || !frame || frame_len < SCENE_HEADER_SIZE) return -SCENE_ERR_PROTOCOL;
    if (h->magic != SCENE_MAGIC) return -SCENE_ERR_PROTOCOL;
    if (h->version != SCENE_PROTOCOL_V0) return -SCENE_ERR_PROTOCOL;
    if (h->length != frame_len - SCENE_HEADER_SIZE) return -SCENE_ERR_PROTOCOL;
    /* Checksum covers the whole frame, bytes [0, 16+length), with the
     * 4-byte checksum field (offset 12..16) treated as zero at compute
     * time, exactly as emit computes it.                                  */
    uint32_t total = SCENE_HEADER_SIZE + h->length;
    uint32_t ck = UINT32_C(2166136261);
    uint32_t i;
    for (i = 0; i < total; i++) {
        uint8_t b = (i >= 12 && i < 16) ? 0 : frame[i];
        ck ^= b;
        ck *= UINT32_C(16777619);
    }
    if (ck != h->checksum) return -SCENE_ERR_CKSUM;
    return 0;
}

void scene_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

void scene_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

void scene_put_u64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}

void scene_put_i32(uint8_t *p, int32_t v)
{
    scene_put_u32(p, (uint32_t)v);
}

uint16_t scene_get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t scene_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t scene_get_u64(const uint8_t *p)
{
    int i;
    uint64_t v = 0;
    for (i = 0; i < 8; i++)
        v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

int32_t scene_get_i32(const uint8_t *p)
{
    return (int32_t)scene_get_u32(p);
}

size_t scene_utf8_field_len(uint32_t len)
{
    return (size_t)4 + (size_t)len;
}

size_t scene_put_utf8(uint8_t *p, const char *s, uint32_t len)
{
    scene_put_u32(p, len);
    if (len > 0 && s != NULL) {
        size_t i;
        for (i = 0; i < len; i++)
            p[4 + i] = (uint8_t)s[i];
    }
    return scene_utf8_field_len(len);
}
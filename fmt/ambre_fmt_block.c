/*
 * ambre_fmt_block.c - Block codec, validation, packing, and disk I/O.
 *
 * Portable C11, no dynamic allocation. All serialization uses memcpy
 * to avoid alignment/strict-aliasing issues.
 */

#include "ambre_fmt.h"
#include <string.h>

static uint32_t compute_block_crc(const ambre_block_t *b)
{
    uint8_t raw[AMBRE_BLOCK_SIZE];
    ambre_block_pack(b, raw);
    return ambre_crc32c(0u, raw, AMBRE_BLOCK_SIZE - AMBRE_BLOCK_CRC_SIZE);
}

int ambre_block_build_chunk(ambre_block_t *out, ambre_rec_type_t type,
                            uint64_t seq, uint64_t session_id,
                            uint64_t first_ts_us, uint16_t chunk_index,
                            uint16_t chunk_count, const void *payload,
                            uint32_t chunk_len, uint32_t total_len)
{
    if (out == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (chunk_count == 0u) {
        return AMBRE_ERR_PARAM;      /* a record always spans >= 1 block */
    }
    if (chunk_count > AMBRE_REC_MAX_BLOCKS) {
        return AMBRE_ERR_TOO_BIG;
    }
    if (chunk_index >= chunk_count || chunk_len > AMBRE_BLOCK_PAYLOAD_MAX) {
        return AMBRE_ERR_PARAM;
    }
    if (total_len > (uint32_t)chunk_count * AMBRE_BLOCK_PAYLOAD_MAX) {
        return AMBRE_ERR_TOO_BIG;
    }
    if (chunk_len > 0u && payload == NULL) {
        return AMBRE_ERR_PARAM;
    }

    memset(out, 0, sizeof(*out));
    out->hdr.magic       = AMBRE_MAGIC_BLOCK;
    out->hdr.version     = (uint16_t)AMBRE_FORMAT_VERSION;
    out->hdr.rec_type    = (uint8_t)type;
    out->hdr.flags       = (chunk_index > 0u) ? (uint8_t)AMBRE_FL_CONTINUATION : 0u;
    out->hdr.seq         = seq;
    out->hdr.session_id  = session_id;
    out->hdr.first_ts_us = first_ts_us;
    out->hdr.chunk_index = chunk_index;
    out->hdr.chunk_count = chunk_count;
    out->hdr.payload_len = chunk_len;
    out->hdr.total_len   = total_len;
    if (chunk_len > 0u) {
        memcpy(out->payload, payload, chunk_len);
    }
    out->hdr.payload_crc = ambre_crc32c(0u, out->payload, chunk_len);
    out->block_crc       = compute_block_crc(out);
    return AMBRE_OK;
}

int ambre_block_build(ambre_block_t *out, ambre_rec_type_t type, uint64_t seq,
                      uint64_t session_id, uint64_t first_ts_us,
                      const void *payload, uint32_t payload_len)
{
    if (payload_len > AMBRE_BLOCK_PAYLOAD_MAX) {
        return AMBRE_ERR_TOO_BIG;
    }
    return ambre_block_build_chunk(out, type, seq, session_id, first_ts_us,
                                   0u, 1u, payload, payload_len, payload_len);
}

int ambre_block_validate(const ambre_block_t *b)
{
    uint32_t expected_block_crc;
    uint32_t expected_payload_crc;

    if (b == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (b->hdr.magic != AMBRE_MAGIC_BLOCK) {
        return AMBRE_ERR_BAD_MAGIC;
    }
    if (b->hdr.version != AMBRE_FORMAT_VERSION) {
        return AMBRE_ERR_BAD_VER;
    }
    if (b->hdr.chunk_count == 0u) {
        return AMBRE_ERR_PARAM;      /* a record always spans >= 1 block */
    }
    if (b->hdr.chunk_count > AMBRE_REC_MAX_BLOCKS ||
        b->hdr.payload_len > AMBRE_BLOCK_PAYLOAD_MAX ||
        b->hdr.total_len > (uint32_t)AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX) {
        return AMBRE_ERR_TOO_BIG;
    }
    if (b->hdr.chunk_index >= b->hdr.chunk_count) {
        return AMBRE_ERR_PARAM;
    }

    expected_block_crc = compute_block_crc(b);
    if (b->block_crc != expected_block_crc) {
        return AMBRE_ERR_BAD_CRC;
    }

    expected_payload_crc = ambre_crc32c(0u, b->payload, b->hdr.payload_len);
    if (b->hdr.payload_crc != expected_payload_crc) {
        return AMBRE_ERR_BAD_CRC;
    }
    return AMBRE_OK;
}

void ambre_block_pack(const ambre_block_t *b, uint8_t buf[AMBRE_BLOCK_SIZE])
{
    if (b == NULL || buf == NULL) {
        return;
    }
    memcpy(buf, b, AMBRE_BLOCK_SIZE);
}

void ambre_block_unpack(const uint8_t buf[AMBRE_BLOCK_SIZE], ambre_block_t *b)
{
    if (buf == NULL || b == NULL) {
        return;
    }
    memcpy(b, buf, AMBRE_BLOCK_SIZE);
}

int ambre_block_write(const ambre_disk_t *d, uint64_t off, const ambre_block_t *b)
{
    uint8_t buf[AMBRE_BLOCK_SIZE];

    if (d == NULL || d->write == NULL || b == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (d->size < AMBRE_BLOCK_SIZE || off > d->size - AMBRE_BLOCK_SIZE) {
        return AMBRE_ERR_IO;
    }
    ambre_block_pack(b, buf);
    return (d->write(d->ctx, off, buf, AMBRE_BLOCK_SIZE) == 0)
        ? AMBRE_OK : AMBRE_ERR_IO;
}

int ambre_block_read(const ambre_disk_t *d, uint64_t off, ambre_block_t *b)
{
    uint8_t buf[AMBRE_BLOCK_SIZE];

    if (d == NULL || d->read == NULL || b == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (d->size < AMBRE_BLOCK_SIZE || off > d->size - AMBRE_BLOCK_SIZE) {
        return AMBRE_ERR_IO;
    }
    if (d->read(d->ctx, off, buf, AMBRE_BLOCK_SIZE) != 0) {
        return AMBRE_ERR_IO;
    }
    ambre_block_unpack(buf, b);
    return AMBRE_OK;
}

uint64_t ambre_block_offset(uint32_t seg, uint32_t blk)
{
    return (uint64_t)AMBRE_FIRST_DATA_OFFSET +
           (uint64_t)seg * (uint64_t)AMBRE_SEGMENT_SIZE +
           (uint64_t)blk * (uint64_t)AMBRE_BLOCK_SIZE;
}

bool ambre_offset_to_segblk(uint64_t off, uint32_t *seg, uint32_t *blk)
{
    uint64_t rel;
    uint32_t s;
    uint32_t b;

    if (off < AMBRE_FIRST_DATA_OFFSET || off >= AMBRE_JOURNAL_BYTES) {
        return false;
    }
    rel = off - AMBRE_FIRST_DATA_OFFSET;
    if ((rel % AMBRE_BLOCK_SIZE) != 0u) {
        return false;
    }
    s = (uint32_t)(rel / AMBRE_SEGMENT_SIZE);
    b = (uint32_t)((rel % AMBRE_SEGMENT_SIZE) / AMBRE_BLOCK_SIZE);
    if (s >= AMBRE_SEGMENT_COUNT || b >= AMBRE_BLOCKS_PER_SEGMENT) {
        return false;
    }
    if (seg != NULL) {
        *seg = s;
    }
    if (blk != NULL) {
        *blk = b;
    }
    return true;
}

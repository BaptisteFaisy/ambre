/*
 * ambre_fmt_record.c — Record level access: a record is one or more
 * consecutive blocks sharing the same seq, session and chunk count.
 *
 * Split out of ambre_fmt_scan.c so that the recovery path and the scanner
 * share a single implementation (house rule: no duplicated logic).
 */

#include "ambre_fmt.h"
#include <string.h>

static int read_valid_block(const ambre_disk_t *d, uint32_t seg, uint32_t blk,
                             ambre_block_t *out)
{
    int rc = ambre_block_read(d, ambre_block_offset(seg, blk), out);

    if (rc != AMBRE_OK) {
        return rc;
    }
    return ambre_block_validate(out);
}

/* Validate a record head: session, sequence and chunk geometry. */
static int check_head(const ambre_block_t *b, uint32_t seg, uint32_t blk,
                      uint64_t session_id, int64_t expect_seq)
{
    if (expect_seq >= 0) {
        if (b->hdr.session_id != session_id) {
            return AMBRE_ERR_STALE;
        }
        if (b->hdr.seq != (uint64_t)expect_seq) {
            return AMBRE_ERR_STALE;
        }
    }
    if (b->hdr.chunk_count == 0u || b->hdr.chunk_count > AMBRE_REC_MAX_BLOCKS) {
        return AMBRE_ERR_TORN;
    }
    if (blk + (uint32_t)b->hdr.chunk_count > AMBRE_BLOCKS_PER_SEGMENT) {
        return AMBRE_ERR_TORN;
    }
    (void)seg;
    return AMBRE_OK;
}

/* Read and validate one continuation chunk, appending its payload. */
static int read_continuation(const ambre_disk_t *d, uint32_t seg, uint32_t blk,
                             uint16_t ci, const ambre_block_t *head,
                             uint8_t *payload_out, uint32_t *acc_len)
{
    ambre_block_t c;

    if (read_valid_block(d, seg, blk, &c) != AMBRE_OK) {
        return AMBRE_ERR_TORN;
    }
    if (c.hdr.session_id != head->hdr.session_id || c.hdr.seq != head->hdr.seq ||
        c.hdr.rec_type != head->hdr.rec_type ||
        c.hdr.chunk_count != head->hdr.chunk_count ||
        c.hdr.chunk_index != ci || c.hdr.total_len != head->hdr.total_len) {
        return AMBRE_ERR_TORN;
    }
    if (c.hdr.payload_len > AMBRE_BLOCK_PAYLOAD_MAX) {
        return AMBRE_ERR_TORN;
    }
    if (payload_out != NULL && c.hdr.payload_len > 0u) {
        memcpy(payload_out + *acc_len, c.payload, c.hdr.payload_len);
    }
    *acc_len += c.hdr.payload_len;
    return AMBRE_OK;
}

int ambre_read_record(const ambre_disk_t *d, uint32_t seg, uint32_t blk,
                      uint64_t session_id, int64_t expect_seq,
                      ambre_block_t *head_out, uint8_t *payload_out,
                      uint32_t *payload_len_out)
{
    ambre_block_t b;
    uint16_t nchunks;
    uint32_t acc_len;
    int rc;

    if (d == NULL || d->read == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (read_valid_block(d, seg, blk, &b) != AMBRE_OK) {
        return AMBRE_ERR_TORN;
    }
    rc = check_head(&b, seg, blk, session_id, expect_seq);
    if (rc != AMBRE_OK) {
        return rc;
    }

    nchunks = b.hdr.chunk_count;
    acc_len = b.hdr.payload_len;
    if (acc_len > AMBRE_BLOCK_PAYLOAD_MAX) {
        return AMBRE_ERR_TORN;
    }
    if (payload_out != NULL && acc_len > 0u) {
        memcpy(payload_out, b.payload, acc_len);
    }
    for (uint16_t ci = 1u; ci < nchunks; ci++) {
        int crc = read_continuation(d, seg, blk + ci, ci, &b, payload_out,
                                    &acc_len);

        if (crc != AMBRE_OK) {
            return crc;
        }
    }
    if (acc_len != b.hdr.total_len) {
        return AMBRE_ERR_TORN;
    }
    if (head_out != NULL) {
        *head_out = b;
    }
    if (payload_len_out != NULL) {
        *payload_len_out = acc_len;
    }
    return AMBRE_OK;
}

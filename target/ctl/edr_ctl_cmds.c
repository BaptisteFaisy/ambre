/*
 * edr_ctl_cmds.c — The four edrctl commands and the export container I/O.
 *
 * status / list read the frozen STATUS_REPLY and event lines; export pulls the
 * raw blocks of one event page by page and writes the "AMEX" container; verify
 * checks an event's integrity both locally (the container's own fingerprints)
 * and against the journal's answer.
 *
 * SEC-01: a refusal (NAK) is always printed as such, with its code; edrctl
 * only ever talks to the journal's service socket and never opens a producer
 * input. SEC-05: a malformed reply is rejected, never partially interpreted.
 *
 * Rules: files < 500 lines, functions < 50 lines, no allocation on the
 * exchange path (every buffer here is static or on the stack).
 */

#include "edr_ctl.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An event spans at most one 2 MiB segment: 4096 blocks of 512 bytes. With
 * 7 blocks per reply that is 586 pages; 4096 pages is a generous bound that
 * still guarantees termination against a misbehaving service. */
#define CTL_EXPORT_MAX_PAGES 4096u

/* ------------------------------------------------------------- container -- */

int ctl_container_open(const char *path, FILE **out)
{
    uint8_t placeholder[CTL_EXPORT_HDR_SIZE];
    FILE *f;

    if (path == NULL || out == NULL) {
        return AMBRE_ERR_PARAM;
    }
    f = fopen(path, "w+b");
    if (f == NULL) {
        return AMBRE_ERR_IO;
    }
    memset(placeholder, 0, sizeof(placeholder));
    if (fwrite(placeholder, 1, sizeof(placeholder), f) != sizeof(placeholder)) {
        fclose(f);
        return AMBRE_ERR_IO;
    }
    *out = f;
    return AMBRE_OK;
}

int ctl_container_append(FILE *f, const uint8_t *block)
{
    if (f == NULL || block == NULL) {
        return AMBRE_ERR_PARAM;
    }
    return (fwrite(block, 1, AMBRE_BLOCK_SIZE, f) == AMBRE_BLOCK_SIZE)
               ? AMBRE_OK : AMBRE_ERR_IO;
}

int ctl_container_finish(FILE *f, const ctl_container_info_t *info)
{
    ctl_container_hdr_t hdr;
    uint8_t raw[CTL_EXPORT_HDR_SIZE];

    if (f == NULL || info == NULL) {
        return AMBRE_ERR_PARAM;
    }
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = CTL_EXPORT_MAGIC;
    hdr.version = (uint16_t)CTL_EXPORT_VERSION;
    hdr.flags = (uint16_t)info->flags;
    hdr.event_id = info->event_id;
    hdr.block_count = info->block_count;
    hdr.fingerprint = info->fingerprint;
    memcpy(raw, &hdr, sizeof(raw));       /* byte copy, never a cast */
    if (fseek(f, 0, SEEK_SET) != 0 ||
        fwrite(raw, 1, sizeof(raw), f) != sizeof(raw)) {
        fclose(f);
        return AMBRE_ERR_IO;
    }
    return (fclose(f) == 0) ? AMBRE_OK : AMBRE_ERR_IO;
}

static void report_fill_info(const ctl_container_hdr_t *hdr,
                             ctl_container_report_t *rep)
{
    rep->header_ok = true;
    rep->info.event_id = hdr->event_id;
    rep->info.block_count = hdr->block_count;
    rep->info.fingerprint = hdr->fingerprint;
    rep->info.flags = hdr->flags;
}

int ctl_container_verify(const char *path, ctl_container_report_t *rep)
{
    uint8_t raw[CTL_EXPORT_HDR_SIZE];
    uint8_t blk[AMBRE_BLOCK_SIZE];
    ctl_container_hdr_t hdr;
    ambre_block_t parsed;
    uint32_t fp = 0u;
    FILE *f;

    if (path == NULL || rep == NULL) {
        return AMBRE_ERR_PARAM;
    }
    memset(rep, 0, sizeof(*rep));
    rep->first_bad_index = UINT32_MAX;
    f = fopen(path, "rb");
    if (f == NULL) {
        return AMBRE_ERR_IO;
    }
    if (fread(raw, 1, sizeof(raw), f) != sizeof(raw)) {
        fclose(f);
        return AMBRE_ERR_TORN;
    }
    memcpy(&hdr, raw, sizeof(hdr));
    if (hdr.magic != CTL_EXPORT_MAGIC || hdr.version != CTL_EXPORT_VERSION) {
        fclose(f);
        return AMBRE_ERR_BAD_MAGIC;
    }
    report_fill_info(&hdr, rep);
    for (uint32_t i = 0u; i < hdr.block_count; i++) {
        if (fread(blk, 1, sizeof(blk), f) != sizeof(blk)) {
            fclose(f);
            return AMBRE_ERR_TORN;
        }
        ambre_block_unpack(blk, &parsed);
        rep->blocks_checked++;
        if (ambre_block_validate(&parsed) != AMBRE_OK && rep->blocks_bad++ == 0u) {
            rep->first_bad_index = i;
        }
        fp = ambre_crc32c(fp, blk, sizeof(blk));
    }
    fclose(f);
    rep->fingerprint_recomputed = fp;
    rep->fingerprint_match = (fp == hdr.fingerprint);
    return (rep->blocks_bad == 0u && rep->fingerprint_match) ? AMBRE_OK
                                                             : AMBRE_ERR_BAD_CRC;
}

/* ---------------------------------------------------------------- status -- */

int ctl_run_status(const ctl_link_t *link, const ctl_args_t *args, FILE *out)
{
    uint8_t reply[AMBRE_IPC_FRAME_MAX];
    ambre_ipc_status_body_t body;
    size_t got = 0u;
    int32_t nak = 0;
    int rc = ctl_transact(link, (uint8_t)AMBRE_MSG_STATUS, NULL, 0u,
                          args->timeout_us, reply, sizeof(reply), &got, &nak);

    if (rc == AMBRE_PORT_ERR_UNSUPPORTED) {
        fprintf(out, "status unavailable: the portability layer has no "
                     "client-side reply (see target/ctl/edr_ctl.c)\n");
        return 1;
    }
    if (rc != AMBRE_PORT_OK) {
        fprintf(out, "NAK: the journal service refused the request (rc=%d, code %d)\n",
                (int)rc, (int)nak);
        return 1;
    }
    if (ctl_ipc_type(reply, got) != (uint8_t)AMBRE_MSG_STATUS_REPLY ||
        got < AMBRE_IPC_HDR_SIZE + sizeof(body)) {
        fprintf(out, "malformed status reply (%u bytes)\n", (unsigned)got);
        return 1;
    }
    memcpy(&body, ctl_ipc_body(reply), sizeof(body));
    if (args->json) {
        ctl_format_status_json(&body, out);
    } else {
        ctl_format_status_human(&body, out);
    }
    return 0;
}

/* ------------------------------------------------------------------ list -- */

static bool list_read_line(const void *body, size_t body_len, uint32_t index,
                           ambre_ipc_event_line_t *line)
{
    size_t off = sizeof(uint32_t) + (size_t)index * sizeof(*line);

    if (off + sizeof(*line) > body_len) {
        return false;
    }
    memcpy(line, (const uint8_t *)body + off, sizeof(*line));
    return true;
}

int ctl_run_list(const ctl_link_t *link, const ctl_args_t *args, FILE *out)
{
    uint8_t reply[AMBRE_IPC_FRAME_MAX];
    ambre_ipc_event_line_t line;
    uint64_t now_us = ambre_port_now_us();
    size_t got = 0u, body_len;
    int32_t nak = 0;
    uint32_t count = 0u;
    int rc = ctl_transact(link, (uint8_t)AMBRE_MSG_LIST, NULL, 0u,
                          args->timeout_us, reply, sizeof(reply), &got, &nak);

    if (rc == AMBRE_PORT_ERR_UNSUPPORTED) {
        fprintf(out, "list unavailable: the portability layer has no "
                     "client-side reply (see target/ctl/edr_ctl.c)\n");
        return 1;
    }
    if (rc != AMBRE_PORT_OK) {
        fprintf(out, "NAK: the journal service refused the request (rc=%d, code %d)\n",
                (int)rc, (int)nak);
        return 1;
    }
    if (ctl_ipc_type(reply, got) != (uint8_t)AMBRE_MSG_LIST_REPLY) {
        fprintf(out, "malformed list reply\n");
        return 1;
    }
    body_len = got - AMBRE_IPC_HDR_SIZE;
    if (body_len >= sizeof(uint32_t)) {
        memcpy(&count, ctl_ipc_body(reply), sizeof(count));
    }
    for (uint32_t i = 0u; i < count; i++) {
        if (!list_read_line(ctl_ipc_body(reply), body_len, i, &line)) {
            fprintf(out, "truncated event list at index %u\n", i);
            return 1;
        }
        if (args->json) {
            ctl_format_event_json(&line, now_us, out);
        } else {
            ctl_format_event_human(&line, now_us, out);
        }
    }
    if (count == 0u && !args->json) {
        fprintf(out, "no event recorded\n");
    }
    return 0;
}

/* ---------------------------------------------------------------- export -- */

typedef struct {
    FILE    *file;
    uint32_t blocks;          /* blocks written so far = pagination cursor   */
    uint32_t total_blocks;    /* as announced by the service                 */
    uint32_t fingerprint;     /* CRC-32C chained over every raw block        */
    uint32_t bad_blocks;      /* blocks whose own CRC or magic failed        */
} export_ctx_t;

/* Append one reply's worth of raw blocks; returns AMBRE_OK or a negative
 * error. The blocks are written exactly as received: the container is a
 * faithful raw dump, and a block that does not validate is counted, not
 * silently dropped. */
static int export_handle_chunk(export_ctx_t *ctx, const uint8_t *reply,
                               size_t got)
{
    ctl_ipc_export_hdr_t hdr;
    size_t body_len = got - AMBRE_IPC_HDR_SIZE;
    const uint8_t *blocks;
    ambre_block_t parsed;

    if (body_len < sizeof(hdr)) {
        return AMBRE_ERR_TORN;
    }
    memcpy(&hdr, ctl_ipc_body(reply), sizeof(hdr));
    if (body_len < sizeof(hdr) + (size_t)hdr.block_count * AMBRE_BLOCK_SIZE) {
        return AMBRE_ERR_TORN;
    }
    blocks = (const uint8_t *)ctl_ipc_body(reply) + sizeof(hdr);
    for (uint32_t i = 0u; i < hdr.block_count; i++) {
        const uint8_t *blk = blocks + (size_t)i * AMBRE_BLOCK_SIZE;

        ambre_block_unpack(blk, &parsed);
        if (ambre_block_validate(&parsed) != AMBRE_OK) {
            ctx->bad_blocks++;
        }
        if (ctl_container_append(ctx->file, blk) != AMBRE_OK) {
            return AMBRE_ERR_IO;
        }
        ctx->fingerprint = ambre_crc32c(ctx->fingerprint, blk, AMBRE_BLOCK_SIZE);
        ctx->blocks++;
    }
    ctx->total_blocks = hdr.total_blocks;
    return AMBRE_OK;
}

/* Pull every page of the event. Returns 0 when the whole event arrived. */
static int export_pull_pages(const ctl_link_t *link, const ctl_args_t *args,
                             export_ctx_t *ctx, FILE *out)
{
    uint8_t reply[AMBRE_IPC_FRAME_MAX];

    for (uint32_t page = 0u; page < CTL_EXPORT_MAX_PAGES; page++) {
        ctl_ipc_export_req_t req;
        size_t got = 0u;
        int32_t nak = 0;
        int rc;

        req.event_id = args->event_id;
        req.first_block = ctx->blocks;
        rc = ctl_transact(link, (uint8_t)AMBRE_MSG_EXPORT, &req,
                          (uint32_t)sizeof(req), args->timeout_us,
                          reply, sizeof(reply), &got, &nak);
        if (rc == AMBRE_PORT_ERR_UNSUPPORTED) {
            fprintf(out, "export unavailable: the portability layer has no "
                         "client-side reply (see target/ctl/edr_ctl.c)\n");
            return 1;
        }
        if (rc != AMBRE_PORT_OK) {
            fprintf(out, "NAK: the journal service refused the export "
                         "(code %d)\n", (int)nak);
            return 1;
        }
        if (ctl_ipc_type(reply, got) != CTL_MSG_EXPORT_REPLY) {
            fprintf(out, "malformed export reply\n");
            return 1;
        }
        if (export_handle_chunk(ctx, reply, got) != AMBRE_OK) {
            fprintf(out, "export reply is truncated or unwritable\n");
            return 1;
        }
        if (ctx->total_blocks == 0u || ctx->blocks >= ctx->total_blocks) {
            return 0;
        }
    }
    fprintf(out, "export aborted: the service never completed the event\n");
    return 1;
}

int ctl_run_export(const ctl_link_t *link, const ctl_args_t *args, FILE *out)
{
    export_ctx_t ctx;
    ctl_container_info_t info;
    int rc;

    if (args->out_path == NULL) {
        fprintf(out, "export: --out <file> is required\n");
        return 1;
    }
    memset(&ctx, 0, sizeof(ctx));
    rc = ctl_container_open(args->out_path, &ctx.file);
    if (rc != AMBRE_OK) {
        fprintf(out, "cannot create %s\n", args->out_path);
        return 1;
    }
    rc = export_pull_pages(link, args, &ctx, out);
    memset(&info, 0, sizeof(info));
    info.event_id = args->event_id;
    info.block_count = ctx.blocks;
    info.fingerprint = ctx.fingerprint;
    if (ctl_container_finish(ctx.file, &info) != AMBRE_OK) {
        fprintf(out, "cannot finalise %s\n", args->out_path);
        return 1;
    }
    if (rc != 0) {
        (void)remove(args->out_path);    /* never leave a partial export behind */
        return rc;
    }
    fprintf(out, "event %u: %u block(s) -> %s, fingerprint %08" PRIx32 "%s\n",
            args->event_id, ctx.blocks, args->out_path, ctx.fingerprint,
            ctx.bad_blocks ? " (WARNING: some blocks failed validation)" : "");
    /* EXP-06: the request carried CTL_REQ_FLAG_LOG_READ, so the service has
     * incremented the "ignition cycle to download" counter and written the
     * access log. The frozen protocol has no field to return the new counter
     * value, so it cannot be displayed here (reported, not faked). */
    fprintf(out, "read logged (EXP-06): the journal was asked to count this "
                 "read and to record it in the access log\n");
    return 0;
}

/* ---------------------------------------------------------------- verify -- */

/* Verify a container file once and report it; `rep` is filled for the caller
 * so the journal cross-check does not have to read the file again. */
static int verify_print_container(const char *path, bool json, FILE *out,
                                  ctl_container_report_t *rep)
{
    int rc = ctl_container_verify(path, rep);

    if (rc == AMBRE_ERR_IO) {
        fprintf(out, "cannot read container %s\n", path);
        return 1;
    }
    if (rc == AMBRE_ERR_TORN || rc == AMBRE_ERR_BAD_MAGIC) {
        fprintf(out, "%s is not an Ambre export container (%s)\n", path,
                rc == AMBRE_ERR_TORN ? "truncated" : "bad magic");
        return 1;
    }
    if (json) {
        ctl_format_container_json(rep, out);
        return (rc == AMBRE_OK) ? 0 : 1;
    }
    if (rep->blocks_checked == 0u) {
        fprintf(out, "container %s holds no block: not a usable export\n", path);
        return 1;
    }
    fprintf(out, "container %s: event %u, %u block(s) checked, %u bad\n",
            path, rep->info.event_id, rep->blocks_checked, rep->blocks_bad);
    fprintf(out, "  fingerprint %08" PRIx32 " (recomputed %08" PRIx32 ") -> %s\n",
            rep->info.fingerprint, rep->fingerprint_recomputed,
            rep->fingerprint_match ? "match" : "MISMATCH");
    if (rep->blocks_bad > 0u) {
        fprintf(out, "  first invalid block: %u\n", rep->first_bad_index);
    }
    return (rc == AMBRE_OK) ? 0 : 1;
}

/* Ask the journal for the event's own integrity verdict. */
static int verify_ask_journal(const ctl_link_t *link, const ctl_args_t *args,
                              uint32_t local_fingerprint, bool have_local,
                              FILE *out)
{
    uint8_t reply[AMBRE_IPC_FRAME_MAX];
    ctl_ipc_verify_req_t req;
    ctl_ipc_verify_body_t body;
    size_t got = 0u;
    int32_t nak = 0;
    int rc;

    req.event_id = args->event_id;
    rc = ctl_transact(link, (uint8_t)AMBRE_MSG_VERIFY, &req,
                      (uint32_t)sizeof(req), args->timeout_us,
                      reply, sizeof(reply), &got, &nak);
    if (rc == AMBRE_PORT_ERR_UNSUPPORTED) {
        fprintf(out, "journal verify unavailable: the portability layer has no "
                     "client-side reply (see target/ctl/edr_ctl.c)\n");
        return have_local ? 0 : 1;
    }
    if (rc != AMBRE_PORT_OK) {
        fprintf(out, "NAK: the journal service refused the verify "
                     "(code %d)\n", (int)nak);
        return 1;
    }
    if (ctl_ipc_type(reply, got) != CTL_MSG_VERIFY_REPLY ||
        got < AMBRE_IPC_HDR_SIZE + sizeof(body)) {
        fprintf(out, "malformed verify reply\n");
        return 1;
    }
    memcpy(&body, ctl_ipc_body(reply), sizeof(body));
    if (args->json) {
        ctl_format_verify_json(&body, out);
    } else {
        fprintf(out, "journal verify: event %u, %u block(s), fingerprint %08"
                     PRIx32 ", complete_file=%s, locked=%s\n",
                body.event_id, body.block_count, body.fingerprint,
                body.file_complete ? "yes" : "no", body.locked ? "yes" : "no");
    }
    if (have_local && local_fingerprint != body.fingerprint) {
        fprintf(out, "MISMATCH: the container fingerprint differs from the "
                     "journal's (%08" PRIx32 " vs %08" PRIx32 ")\n",
                local_fingerprint, body.fingerprint);
        return 1;
    }
    return 0;
}

int ctl_run_verify(const ctl_link_t *link, const ctl_args_t *args, FILE *out)
{
    ctl_container_report_t rep;
    uint32_t local_fp = 0u;
    bool have_local = false;
    int rc = 0;

    memset(&rep, 0, sizeof(rep));
    if (args->out_path != NULL) {
        int vrc = verify_print_container(args->out_path, args->json, out, &rep);

        rc = vrc;
        have_local = rep.header_ok;
        local_fp = rep.fingerprint_recomputed;
    }
    if (args->have_event_id) {
        rc |= verify_ask_journal(link, args, local_fp, have_local, out);
    } else if (!have_local) {
        fprintf(out, "verify: nothing to check (give an event id and/or "
                     "--out <container>)\n");
        return 1;
    }
    return rc;
}

/* -------------------------------------------------------------- dispatch -- */

int ctl_dispatch(const ctl_link_t *link, const ctl_args_t *args, FILE *out)
{
    switch (args->cmd) {
    case CTL_CMD_STATUS: return ctl_run_status(link, args, out);
    case CTL_CMD_LIST:   return ctl_run_list(link, args, out);
    case CTL_CMD_EXPORT: return ctl_run_export(link, args, out);
    case CTL_CMD_VERIFY: return ctl_run_verify(link, args, out);
    default:             return 2;
    }
}

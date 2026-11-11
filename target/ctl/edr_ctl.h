/*
 * edr_ctl.h — Pure logic of the edrctl service tool (EXP-01, EXP-06, SEC-01).
 *
 * Commands (frozen CLI, docs/W2-PLAN.md):
 *   status                 state of the journal service, text or --json
 *   list                   one event per line: state, lock, "complete file",
 *                          trigger reason, relative instant
 *   export <id> --out <f>  raw records of one event into a container file
 *   verify [id] [--out f]  integrity of an event: fingerprints, record chain
 *
 * Transport: every exchange goes through the injectable ctl_link_t seam, so
 * the whole tool is testable against a fake journal service and so that the
 * one missing piece of the frozen portability layer (a client-side
 * request/reply) is isolated in a single function. See the note above
 * ctl_link_open() in edr_ctl.c.
 *
 * Rules: files < 500 lines, functions < 50 lines, C11, no allocation on the
 * exchange path, no warning.
 */

#ifndef EDR_CTL_H
#define EDR_CTL_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "edr_ipc_wire.h"

/* ------------------------------------------------------------- transport -- */

/* One request/reply exchange. Returns AMBRE_PORT_OK and the number of bytes
 * received in *reply_len, or a negative ambre_port_err_t. */
typedef int (*ctl_xfer_fn)(void *ctx, const void *req, size_t req_len,
                           void *reply, size_t reply_cap, uint32_t timeout_us,
                           size_t *reply_len);

typedef struct {
    void       *ctx;
    ctl_xfer_fn xfer;
} ctl_link_t;

/* Open the production link to the journal service over the portability layer.
 * Returns AMBRE_PORT_OK, AMBRE_PORT_ERR_NOENT when the socket is absent, or
 * AMBRE_PORT_ERR_UNSUPPORTED while the frozen port layer has no client-side
 * request/reply (the connection is still opened, so the diagnostic is honest
 * and the tool exits 1 instead of inventing a state). */
int  ctl_link_open(const char *journal_sock, ctl_link_t *out);
void ctl_link_close(ctl_link_t *link);

/* Build one request, exchange it, validate the reply header. On a NAK the
 * refusal is decoded and reported through *nak_code (SEC-01: a refusal is
 * always shown, never swallowed). Returns AMBRE_PORT_OK on a good reply. */
int ctl_transact(const ctl_link_t *link, uint8_t req_type, const void *body,
                 uint32_t body_len, uint32_t timeout_us,
                 void *reply, size_t reply_cap, size_t *reply_len,
                 int32_t *nak_code);

/* -------------------------------------------------------------- container - */

/* Export container "AMEX" v1 (documented here, written by `edrctl export`):
 *
 *   offset  size  field
 *        0      4  magic "AMEX" (0x414D4558, little endian)
 *        4      2  version = 1
 *        6      2  flags (bit0 = the journal reported "file complete")
 *        8      4  event_id
 *       12      4  block_count (512-byte blocks that follow)
 *       16      4  fingerprint: CRC-32C chained over every raw block in
 *                   order (ambre_crc32c chaining, starting from 0), valid
 *                   blocks or not — the same value `edrctl verify` asks the
 *                   journal for (SEC-04)
 *       20     12  reserved, zero
 *       32  512*n  the raw journal blocks, exactly as they sit on the
 *                   partition (block headers, payloads and CRCs included)
 *
 * The container holds the event's raw records, not MCAP: building MCAP is
 * edr-archive's job (target/archive/) and host/mcap/export_mcap.py's job, and
 * neither writer is duplicated here. */
#define CTL_EXPORT_MAGIC    0x414D4558u
#define CTL_EXPORT_VERSION  1u
#define CTL_EXPORT_FLAG_COMPLETE 0x0001u
#define CTL_EXPORT_HDR_SIZE 32u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t event_id;
    uint32_t block_count;
    uint32_t fingerprint;
    uint8_t  reserved[12];
} ctl_container_hdr_t;
#pragma pack(pop)

_Static_assert(sizeof(ctl_container_hdr_t) == CTL_EXPORT_HDR_SIZE,
               "container header must be 32 bytes");

typedef struct {
    uint32_t event_id;
    uint32_t block_count;
    uint32_t fingerprint;
    uint32_t flags;
} ctl_container_info_t;

/* Result of verifying a container: every block is checked on its own (magic,
 * version, both CRCs) and the chained fingerprint is recomputed. */
typedef struct {
    ctl_container_info_t info;
    uint32_t fingerprint_recomputed;
    uint32_t blocks_checked;
    uint32_t blocks_bad;
    uint32_t first_bad_index;
    bool     fingerprint_match;
    bool     header_ok;
} ctl_container_report_t;

int ctl_container_verify(const char *path, ctl_container_report_t *report);

/* Append one raw block to an open container file (the header is patched once
 * the last block is known). */
int ctl_container_open(const char *path, FILE **out);
int ctl_container_append(FILE *f, const uint8_t *block);
int ctl_container_finish(FILE *f, const ctl_container_info_t *info);

/* ------------------------------------------------------------ formatting -- */

void ctl_format_status_human(const ambre_ipc_status_body_t *sb, FILE *out);
void ctl_format_status_json(const ambre_ipc_status_body_t *sb, FILE *out);
void ctl_format_event_human(const ambre_ipc_event_line_t *line, uint64_t now_us,
                            FILE *out);
void ctl_format_event_json(const ambre_ipc_event_line_t *line, uint64_t now_us,
                           FILE *out);
void ctl_format_verify_json(const ctl_ipc_verify_body_t *vb, FILE *out);
void ctl_format_container_json(const ctl_container_report_t *rep, FILE *out);

/* Escape a string into a JSON literal (quotes, backslashes, control chars).
 * Returns the number of bytes written, excluding the terminator. */
size_t ctl_json_escape(const char *in, char *out, size_t cap);

const char *ctl_trig_reason_str(uint8_t reason);
const char *ctl_state_str(uint8_t state);
const char *ctl_backend_str(uint8_t backend);
const char *ctl_health_str(uint8_t health);

/* --------------------------------------------------------------- commands - */

typedef enum {
    CTL_CMD_NONE = 0,
    CTL_CMD_STATUS,
    CTL_CMD_LIST,
    CTL_CMD_EXPORT,
    CTL_CMD_VERIFY
} ctl_cmd_t;

typedef struct {
    ctl_cmd_t   cmd;
    const char *journal_sock;
    const char *out_path;      /* export container, or container to verify   */
    uint32_t    event_id;
    bool        have_event_id;
    bool        json;
    uint32_t    timeout_us;
} ctl_args_t;

void ctl_args_default(ctl_args_t *args);

/* Each command returns the process exit code: 0 ok, 1 functional failure. */
int ctl_run_status(const ctl_link_t *link, const ctl_args_t *args, FILE *out);
int ctl_run_list(const ctl_link_t *link, const ctl_args_t *args, FILE *out);
int ctl_run_export(const ctl_link_t *link, const ctl_args_t *args, FILE *out);
int ctl_run_verify(const ctl_link_t *link, const ctl_args_t *args, FILE *out);

/* Dispatch one parsed command line. */
int ctl_dispatch(const ctl_link_t *link, const ctl_args_t *args, FILE *out);

/* Parse the frozen command line:
 *   edrctl --journal <sock> {status|list|export <id> --out <f>|verify [id]}
 *          [--json]
 * Returns 0 when a command is ready to run, 1 when --help was printed (the
 * caller exits 0), -1 on a usage error (the caller exits 2). */
int ctl_parse_args(int argc, char **argv, ctl_args_t *args);

void ctl_print_usage(const char *prog, FILE *out);

/* Process entry point: 0 ok, 1 functional failure, 2 usage error. */
int edrctl_main(int argc, char **argv);

#endif /* EDR_CTL_H */

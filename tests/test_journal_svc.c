/*
 * test_journal_svc.c — Tests for the portable service layer of edr-journal.
 *
 * Calls ambre_edr_service_handle() directly over a temporary POSIX disk image
 * in /tmp (no sockets): virgin formatting, recovery before first capture
 * (REP-01), trigger/continue/seal/export/verify, access control (SEC-01),
 * no stop order (SEC-02), locked event preserved (SEC-03), no archive while
 * open (GEL-06), bounded freeze latency (PERF-02/03), and start-up self-test
 * (OBS-04).
 */

#include "ambre_test.h"
#include "journal/edr_journal_svc.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_SEGS 2u
#define TEST_DISK_BYTES (AMBRE_FIRST_DATA_OFFSET + \
                         (uint64_t)TEST_SEGS * AMBRE_SEGMENT_SIZE)

typedef struct {
    uint32_t count;
    uint32_t last_event_id;
    uint8_t  last_locked;
    uint8_t  last_complete;
} notify_spy_t;

static int on_notify(void *ctx, const void *msg, size_t len)
{
    notify_spy_t *spy = (notify_spy_t *)ctx;
    ambre_ipc_hdr_t h;
    ambre_edr_event_ready_body_t r;

    if (len < AMBRE_IPC_HDR_SIZE + sizeof(r)) {
        return AMBRE_PORT_ERR_PARAM;
    }
    memcpy(&h, msg, sizeof(h));
    if (h.msg_type != (uint8_t)AMBRE_MSG_EVENT_READY) {
        return AMBRE_PORT_ERR_PARAM;
    }
    memcpy(&r, (const uint8_t *)msg + AMBRE_IPC_HDR_SIZE, sizeof(r));
    spy->count++;
    spy->last_event_id = r.event_id;
    spy->last_locked   = r.locked;
    spy->last_complete = r.file_complete;
    return AMBRE_PORT_OK;
}

static void make_disk_path(char *buf, size_t cap, const char *tag)
{
    (void)snprintf(buf, cap, "/tmp/ambre_jsvc_%s_%ld.img", tag, (long)getpid());
}

static void make_cfg(ambre_edr_service_cfg_t *scfg, const char *path)
{
    memset(scfg, 0, sizeof(*scfg));
    scfg->disk_spec = path;
    scfg->disk_size = TEST_DISK_BYTES;
    ambre_journal_cfg_default(&scfg->cfg);
    scfg->cfg.session_id = 0xCAFEu;
    scfg->cfg.segment_limit = TEST_SEGS;
}

static uint8_t reply_type(const uint8_t *reply, size_t reply_len)
{
    ambre_ipc_hdr_t h;

    if (reply_len < AMBRE_IPC_HDR_SIZE) {
        return 0u;
    }
    memcpy(&h, reply, sizeof(h));
    return (h.magic == AMBRE_IPC_MAGIC) ? h.msg_type : 0u;
}

static int send_msg(ambre_edr_service_t *svc, uint32_t peer_type, uint8_t type,
                    const void *body, uint32_t body_len,
                    uint8_t *reply, size_t reply_cap, size_t *reply_len)
{
    uint8_t req[AMBRE_IPC_FRAME_MAX];
    ambre_ipc_hdr_t h;

    if (AMBRE_IPC_HDR_SIZE + body_len > sizeof(req)) {
        return AMBRE_PORT_ERR_PARAM;
    }
    memset(&h, 0, sizeof(h));
    h.magic = AMBRE_IPC_MAGIC;
    h.version = (uint8_t)AMBRE_IPC_VERSION;
    h.msg_type = type;
    h.body_len = body_len;
    memcpy(req, &h, sizeof(h));
    if (body_len > 0u && body != NULL) {
        memcpy(req + AMBRE_IPC_HDR_SIZE, body, body_len);
    }
    return ambre_edr_service_handle(svc, peer_type, req,
                                    (size_t)AMBRE_IPC_HDR_SIZE + body_len,
                                    reply, reply_cap, reply_len);
}

static void fill_emerg(ambre_emerg_t *em, uint32_t event_id, uint8_t locked)
{
    memset(em, 0, sizeof(*em));
    em->event_id       = event_id;
    em->trig_reason    = (uint8_t)(locked ? AMBRE_TRIG_RESTRAINT
                                          : AMBRE_TRIG_DV_LONG);
    em->locked         = locked;
    em->event_number   = 1u;
    em->ignition_cycle = 2u;
    em->t0_ts_us       = 100000u;
    em->trig_ts_us     = 100100u;
    em->crash_order    = event_id;
    em->point_count    = 4u;
    em->points[0].speed_kmh_x4 = 320u;
}

static void fill_seal(ambre_seal_t *sl, uint32_t event_id, uint8_t locked)
{
    memset(sl, 0, sizeof(*sl));
    sl->event_id            = event_id;
    sl->file_complete       = 1u;
    sl->seal_cause          = (uint8_t)AMBRE_SEAL_NORMAL;
    sl->locked              = locked;
    sl->event_number        = 1u;
    sl->time_since_first_ms = 250u;
    sl->max_dv_long_mm_s    = 4200;
    sl->max_dv_long_at_ms   = 60u;
}

static void trigger_cont_seal(ambre_edr_service_t *svc, uint32_t event_id,
                              uint8_t locked, uint64_t *freeze_us)
{
    uint8_t rep[AMBRE_IPC_FRAME_MAX];
    size_t rlen = 0u;
    ambre_emerg_t em;
    ambre_cont_t cont;
    ambre_seal_t sl;
    ambre_edr_ack_body_t ack;

    fill_emerg(&em, event_id, locked);
    AMBRE_CHECK_OK(send_msg(svc, AMBRE_PEER_CAPTURE, (uint8_t)AMBRE_MSG_TRIGGER,
                            &em, (uint32_t)sizeof(em), rep, sizeof(rep), &rlen));
    AMBRE_CHECK_EQ_U64(reply_type(rep, rlen), AMBRE_MSG_ACK);
    memcpy(&ack, rep + AMBRE_IPC_HDR_SIZE, sizeof(ack));
    if (freeze_us != NULL) {
        *freeze_us = ack.freeze_us;
    }

    /* GEL-06: export/verify refused while the event is open. */
    {
        ambre_edr_export_req_t ereq = { event_id, 0u };
        AMBRE_CHECK(send_msg(svc, AMBRE_PEER_ARCHIVE, (uint8_t)AMBRE_MSG_EXPORT,
                             &ereq, (uint32_t)sizeof(ereq),
                             rep, sizeof(rep), &rlen) < 0);
        AMBRE_CHECK_EQ_U64(reply_type(rep, rlen), AMBRE_MSG_NAK);
    }

    memset(&cont, 0, sizeof(cont));
    cont.event_id = event_id;
    cont.sample_count = 1u;
    AMBRE_CHECK_OK(send_msg(svc, AMBRE_PEER_CAPTURE, (uint8_t)AMBRE_MSG_CONTINUE,
                            &cont, (uint32_t)sizeof(cont),
                            rep, sizeof(rep), &rlen));
    AMBRE_CHECK_EQ_U64(reply_type(rep, rlen), AMBRE_MSG_ACK);

    fill_seal(&sl, event_id, locked);
    AMBRE_CHECK_OK(send_msg(svc, AMBRE_PEER_CAPTURE, (uint8_t)AMBRE_MSG_SEAL,
                            &sl, (uint32_t)sizeof(sl), rep, sizeof(rep), &rlen));
    AMBRE_CHECK_EQ_U64(reply_type(rep, rlen), AMBRE_MSG_ACK);
}

/* --------------------------------------------------------------- tests --- */

static void test_virgin_open_and_selftest(void)
{
    char path[128];
    ambre_edr_service_cfg_t scfg;
    ambre_edr_service_t svc;
    char text[AMBRE_EDR_SVC_TEXT_MAX];

    make_disk_path(path, sizeof(path), "virgin");
    (void)unlink(path);
    make_cfg(&scfg, path);

    AMBRE_CHECK_OK(ambre_edr_service_open(&svc, &scfg));
    AMBRE_CHECK_OK(ambre_edr_service_selftest(&svc)); /* OBS-04 */
    AMBRE_CHECK_OK(ambre_edr_service_status_text(&svc, text, sizeof(text)));
    AMBRE_CHECK(strstr(text, "mode=continuous") != NULL);
    AMBRE_CHECK(strstr(text, "health=ok") != NULL);
    ambre_edr_service_close(&svc);
    (void)unlink(path);
}

static void test_recovery_before_capture(void)
{
    char path[128];
    ambre_edr_service_cfg_t scfg;
    ambre_edr_service_t svc;
    uint8_t rep[AMBRE_IPC_FRAME_MAX];
    size_t rlen = 0u;
    ambre_emerg_t em;
    ambre_edr_list_body_t list;

    make_disk_path(path, sizeof(path), "recov");
    (void)unlink(path);
    make_cfg(&scfg, path);

    /* Boot 1: trigger an event, then stop without SEAL. */
    AMBRE_CHECK_OK(ambre_edr_service_open(&svc, &scfg));
    fill_emerg(&em, 7u, 1u);
    AMBRE_CHECK_OK(send_msg(&svc, AMBRE_PEER_CAPTURE, (uint8_t)AMBRE_MSG_TRIGGER,
                            &em, (uint32_t)sizeof(em), rep, sizeof(rep), &rlen));
    AMBRE_CHECK_EQ_U64(reply_type(rep, rlen), AMBRE_MSG_ACK);
    ambre_edr_service_close(&svc);

    /* Boot 2: recovery runs before any capture (REP-01, REP-03). */
    AMBRE_CHECK_OK(ambre_edr_service_open(&svc, &scfg));
    AMBRE_CHECK(svc.journal.recovered);
    AMBRE_CHECK_EQ_U64(svc.journal.last_recovery.sealed_incomplete, 1u);

    AMBRE_CHECK_OK(send_msg(&svc, AMBRE_PEER_SERVICE, (uint8_t)AMBRE_MSG_LIST,
                            NULL, 0u, rep, sizeof(rep), &rlen));
    AMBRE_CHECK_EQ_U64(reply_type(rep, rlen), AMBRE_MSG_LIST_REPLY);
    memcpy(&list, rep + AMBRE_IPC_HDR_SIZE, rlen - AMBRE_IPC_HDR_SIZE);
    AMBRE_CHECK_EQ_U64(list.count, 1u);
    AMBRE_CHECK_EQ_U64(list.lines[0].event_id, 7u);
    AMBRE_CHECK_EQ_U64(list.lines[0].state, AMBRE_EV_SEALED);
    AMBRE_CHECK_EQ_U64(list.lines[0].locked, 1u);
    AMBRE_CHECK_EQ_U64(list.lines[0].file_complete, 0u);

    ambre_edr_service_close(&svc);
    (void)unlink(path);
}

static void check_status_and_list(ambre_edr_service_t *svc)
{
    uint8_t rep[AMBRE_IPC_FRAME_MAX];
    size_t rlen = 0u;
    ambre_ipc_status_body_t st;
    ambre_edr_list_body_t list;

    AMBRE_CHECK_OK(send_msg(svc, AMBRE_PEER_SERVICE, (uint8_t)AMBRE_MSG_STATUS,
                            NULL, 0u, rep, sizeof(rep), &rlen));
    AMBRE_CHECK_EQ_U64(reply_type(rep, rlen), AMBRE_MSG_STATUS_REPLY);
    memcpy(&st, rep + AMBRE_IPC_HDR_SIZE, sizeof(st));
    AMBRE_CHECK_EQ_U64(st.open_events, 0u);
    AMBRE_CHECK_EQ_U64(st.acquired_events, 1u);
    AMBRE_CHECK_EQ_U64(st.sealed_events, 1u);
    AMBRE_CHECK(st.bytes_written > 0u);
    AMBRE_CHECK(st.flush_count >= 3u);
    AMBRE_CHECK(st.worst_freeze_us >= st.last_freeze_us);
    AMBRE_CHECK(st.worst_freeze_us < 500000u);

    AMBRE_CHECK_OK(send_msg(svc, AMBRE_PEER_SERVICE, (uint8_t)AMBRE_MSG_LIST,
                            NULL, 0u, rep, sizeof(rep), &rlen));
    memcpy(&list, rep + AMBRE_IPC_HDR_SIZE, rlen - AMBRE_IPC_HDR_SIZE);
    AMBRE_CHECK_EQ_U64(list.count, 1u);
    AMBRE_CHECK_EQ_U64(list.lines[0].locked, 1u);
    AMBRE_CHECK_EQ_U64(list.lines[0].file_complete, 1u);
}

static void check_export_and_verify(ambre_edr_service_t *svc, uint32_t event_id)
{
    uint8_t rep[AMBRE_IPC_FRAME_MAX];
    size_t rlen = 0u;
    ambre_edr_export_req_t ereq = { event_id, 0u };
    ambre_edr_export_body_t ebody;
    ambre_edr_verify_req_t vreq = { event_id };
    ambre_edr_verify_body_t vbody;

    AMBRE_CHECK_OK(send_msg(svc, AMBRE_PEER_ARCHIVE, (uint8_t)AMBRE_MSG_EXPORT,
                            &ereq, (uint32_t)sizeof(ereq),
                            rep, sizeof(rep), &rlen));
    AMBRE_CHECK_EQ_U64(reply_type(rep, rlen), AMBRE_EDR_REPLY_EXPORT);
    memcpy(&ebody, rep + AMBRE_IPC_HDR_SIZE, sizeof(ebody));
    AMBRE_CHECK_EQ_U64(ebody.event_id, event_id);
    AMBRE_CHECK_EQ_U64(ebody.total_blocks, 3u);
    AMBRE_CHECK_EQ_U64(ebody.block_count, 3u);

    AMBRE_CHECK_OK(send_msg(svc, AMBRE_PEER_SERVICE, (uint8_t)AMBRE_MSG_VERIFY,
                            &vreq, (uint32_t)sizeof(vreq),
                            rep, sizeof(rep), &rlen));
    AMBRE_CHECK_EQ_U64(reply_type(rep, rlen), AMBRE_EDR_REPLY_VERIFY);
    memcpy(&vbody, rep + AMBRE_IPC_HDR_SIZE, sizeof(vbody));
    AMBRE_CHECK_EQ_U64(vbody.block_count, 3u);
    AMBRE_CHECK(vbody.fingerprint != 0u);
    AMBRE_CHECK_EQ_U64(vbody.locked, 1u);
    AMBRE_CHECK_EQ_U64(vbody.file_complete, 1u);
}

static void test_trigger_status_list_export_verify(void)
{
    char path[128];
    ambre_edr_service_cfg_t scfg;
    ambre_edr_service_t svc;
    notify_spy_t spy;
    uint64_t freeze_us = 0u;

    make_disk_path(path, sizeof(path), "flow");
    (void)unlink(path);
    make_cfg(&scfg, path);
    memset(&spy, 0, sizeof(spy));

    AMBRE_CHECK_OK(ambre_edr_service_open(&svc, &scfg));
    ambre_edr_service_set_notify(&svc, on_notify, &spy);

    trigger_cont_seal(&svc, 11u, 1u, &freeze_us);
    AMBRE_CHECK(freeze_us < 500000u);
    AMBRE_CHECK_EQ_U64(spy.count, 1u);
    AMBRE_CHECK_EQ_U64(spy.last_event_id, 11u);
    AMBRE_CHECK_EQ_U64(spy.last_complete, 1u);

    check_status_and_list(&svc);
    check_export_and_verify(&svc, 11u);

    ambre_edr_service_close(&svc);
    (void)unlink(path);
}

static void test_access_control_and_no_stop(void)
{
    char path[128];
    ambre_edr_service_cfg_t scfg;
    ambre_edr_service_t svc;
    uint8_t rep[AMBRE_IPC_FRAME_MAX];
    size_t rlen = 0u;
    ambre_emerg_t em;

    make_disk_path(path, sizeof(path), "sec");
    (void)unlink(path);
    make_cfg(&scfg, path);
    AMBRE_CHECK_OK(ambre_edr_service_open(&svc, &scfg));

    fill_emerg(&em, 1u, 0u);
    AMBRE_CHECK_EQ_I64(send_msg(&svc, AMBRE_PEER_UNKNOWN, (uint8_t)AMBRE_MSG_TRIGGER,
                                &em, (uint32_t)sizeof(em), rep, sizeof(rep), &rlen),
                       AMBRE_PORT_ERR_PERM);
    AMBRE_CHECK_EQ_U64(reply_type(rep, rlen), AMBRE_MSG_NAK);
    AMBRE_CHECK_EQ_I64(send_msg(&svc, AMBRE_PEER_UNKNOWN, (uint8_t)AMBRE_MSG_STATUS,
                                NULL, 0u, rep, sizeof(rep), &rlen),
                       AMBRE_PORT_ERR_PERM);
    AMBRE_CHECK_EQ_I64(send_msg(&svc, AMBRE_PEER_SERVICE, (uint8_t)AMBRE_MSG_TRIGGER,
                                &em, (uint32_t)sizeof(em), rep, sizeof(rep), &rlen),
                       AMBRE_PORT_ERR_PERM);

    /* SEC-02 + SEC-05: unknown opcode / truncated frame refused with NAK. */
    AMBRE_CHECK(send_msg(&svc, AMBRE_PEER_SERVICE, 0x7Fu, NULL, 0u,
                         rep, sizeof(rep), &rlen) < 0);
    AMBRE_CHECK(ambre_edr_service_handle(&svc, AMBRE_PEER_CAPTURE, rep, 4u,
                                         rep, sizeof(rep), &rlen) < 0);
    AMBRE_CHECK_EQ_U64(reply_type(rep, rlen), AMBRE_MSG_NAK);
    AMBRE_CHECK(svc.refused_count >= 5u);
    AMBRE_CHECK(svc.malformed_count >= 2u);

    AMBRE_CHECK_OK(send_msg(&svc, AMBRE_PEER_CAPTURE, (uint8_t)AMBRE_MSG_TRIGGER,
                            &em, (uint32_t)sizeof(em), rep, sizeof(rep), &rlen));
    AMBRE_CHECK_EQ_U64(reply_type(rep, rlen), AMBRE_MSG_ACK);

    ambre_edr_service_close(&svc);
    (void)unlink(path);
}

static void test_locked_event_cannot_be_erased(void)
{
    char path[128];
    ambre_edr_service_cfg_t scfg;
    ambre_edr_service_t svc;
    uint8_t rep[AMBRE_IPC_FRAME_MAX];
    size_t rlen = 0u;
    ambre_sample_t s;

    make_disk_path(path, sizeof(path), "lock");
    (void)unlink(path);
    make_cfg(&scfg, path);
    AMBRE_CHECK_OK(ambre_edr_service_open(&svc, &scfg));

    memset(&s, 0, sizeof(s));
    s.signal_id = 1u;
    for (uint32_t id = 1u; id <= TEST_SEGS; id++) {
        trigger_cont_seal(&svc, id, 1u, NULL);
        while (svc.journal.table.current_block < AMBRE_BLOCKS_PER_SEGMENT) {
            AMBRE_CHECK_OK(ambre_journal_append_data(&svc.journal, 1000u, &s, 1u));
        }
        if (id < TEST_SEGS) {
            AMBRE_CHECK_OK(ambre_edr_service_tick(&svc, 200000u));
        }
    }
    AMBRE_CHECK_EQ_I64(ambre_edr_service_tick(&svc, 400000u), AMBRE_PORT_ERR_FULL);
    AMBRE_CHECK_EQ_U64(svc.health, 2u);
    AMBRE_CHECK(send_msg(&svc, AMBRE_PEER_SERVICE, 0xFEu, NULL, 0u,
                         rep, sizeof(rep), &rlen) < 0);
    AMBRE_CHECK_EQ_U64(reply_type(rep, rlen), AMBRE_MSG_NAK);
    AMBRE_CHECK(ambre_table_find_event_const(&svc.journal.table, 1u) != NULL);
    AMBRE_CHECK(ambre_table_find_event_const(&svc.journal.table, 2u) != NULL);

    ambre_edr_service_close(&svc);
    (void)unlink(path);
}

static const ambre_test_case_t cases[] = {
    { "virgin_open_and_selftest", test_virgin_open_and_selftest },
    { "recovery_before_capture", test_recovery_before_capture },
    { "trigger_status_list_export_verify", test_trigger_status_list_export_verify },
    { "access_control_and_no_stop", test_access_control_and_no_stop },
    { "locked_event_cannot_be_erased", test_locked_event_cannot_be_erased }
};

AMBRE_TEST_MAIN(cases)

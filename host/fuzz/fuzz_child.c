/*
 * fuzz_child.c — What one generated image must survive (spec 12.10).
 *
 * Post-conditions asserted on ANY input, each one grounded in the format
 * spec or the frozen headers:
 *   - ambre_recover() returns a known ambre_err_t code and honours the I1
 *     bounds (same bounds as the explorer's invariant checker);
 *   - after a successful recovery the table validates (REP-02), recovery is
 *     idempotent (REP-04 / I8, checked exactly like check_i8 does), and
 *   - every event the recovery declares can be read back through the public
 *     reader ambre_journal_read_event(), while an absent event id is
 *     reported as AMBRE_ERR_STATE instead of crashing.
 *
 * Runs in a forked child: no allocation, only fixed-size locals, so the
 * parent can fork it from its slot loop without malloc-lock hazards.
 */

#include "fuzz_child.h"
#include "ambre_fmt.h"
#include "ambre_journal.h"
#include <stdio.h>
#include <string.h>

static const char *const FAIL_NAMES[] = {
    "ok", "recover-rc", "table-validate", "table-sanity", "idempotent",
    "journal-open", "read-event", "unknown-id", "open-on-garbage"
};

const char *fuzz_fail_name(int code)
{
    if (code < 0 || code >= (int)(sizeof(FAIL_NAMES) / sizeof(FAIL_NAMES[0]))) {
        return "unknown";
    }
    return FAIL_NAMES[code];
}

/* Every documented error code lies in [AMBRE_ERR_STALE, AMBRE_OK]. */
static bool rc_valid(int rc)
{
    return rc <= (int)AMBRE_OK && rc >= (int)AMBRE_ERR_STALE;
}

/* I1-style bounds plus the range checks the recovered table must satisfy. */
static int check_result_sanity(const ambre_recovery_result_t *res)
{
    if (res->scanned_blocks > AMBRE_SEGMENT_COUNT * AMBRE_BLOCKS_PER_SEGMENT) {
        return FUZZ_F_TABLE_SANITY;
    }
    if (res->table_commits > 64u) {
        return FUZZ_F_TABLE_SANITY;
    }
    if (res->events_after > AMBRE_MAX_EVENTS) {
        return FUZZ_F_TABLE_SANITY;
    }
    if (res->table.event_count > AMBRE_MAX_EVENTS) {
        return FUZZ_F_TABLE_SANITY;
    }
    return FUZZ_OK;
}

/* Re-run recovery on the same disk: must succeed and change nothing
 * (REP-04, invariant I8 — same comparison as the explorer's check_i8). */
static int check_idempotent(ambre_memdisk_t *md,
                            const ambre_recovery_result_t *first)
{
    ambre_recovery_result_t second;

    memset(&second, 0, sizeof(second));
    if (ambre_recover(&md->disk, &second) != AMBRE_OK) {
        return FUZZ_F_IDEMPOTENT;
    }
    if (!ambre_tables_equivalent(&first->table, &second.table)) {
        return FUZZ_F_IDEMPOTENT;
    }
    return FUZZ_OK;
}

/* One event id absent from the table (deterministic pick). */
static uint32_t pick_unknown_id(const ambre_alloc_table_t *t)
{
    for (uint32_t id = 0xFFFFFFFFu; ; id--) {
        if (ambre_table_find_event_const(t, id) == NULL) {
            return id;
        }
    }
}

/* Read back every declared event through the public reader. */
static int check_read_events(const ambre_journal_t *j,
                             const ambre_alloc_table_t *t)
{
    ambre_event_view_t view;

    for (uint32_t i = 0u; i < t->event_count; i++) {
        const ambre_ev_desc_t *desc = &t->events[i];
        int rc = ambre_journal_read_event(j, desc->event_id, &view);
        if (rc != AMBRE_OK) {
            return FUZZ_F_READ_EVENT;
        }
        if (view.have_emerg && view.emerg.event_id != desc->event_id) {
            return FUZZ_F_READ_EVENT;
        }
    }
    memset(&view, 0, sizeof(view));
    if (ambre_journal_read_event(j, pick_unknown_id(t), &view)
            != AMBRE_ERR_STATE) {
        return FUZZ_F_UNKNOWN_ID;
    }
    return FUZZ_OK;
}

/* After a rejected recovery, opening must still cope: with no usable table
 * it formats a fresh journal (documented ambre_journal_open behaviour). */
static int check_open_on_garbage(ambre_memdisk_t *md)
{
    ambre_journal_cfg_t cfg;
    ambre_journal_t j;
    int rc;

    ambre_journal_cfg_default(&cfg);
    rc = ambre_journal_open(&md->disk, &cfg, false, &j);
    return rc_valid(rc) ? FUZZ_OK : FUZZ_F_OPEN_GARBAGE;
}

int fuzz_child_run(ambre_memdisk_t *md)
{
    ambre_recovery_result_t res;
    ambre_journal_cfg_t cfg;
    ambre_journal_t j;
    int rc;
    int fail;

    memset(&res, 0, sizeof(res));
    rc = ambre_recover(&md->disk, &res);
    if (!rc_valid(rc)) {
        return FUZZ_F_RECOVER_RC;
    }
    if (rc != AMBRE_OK) {
        return check_open_on_garbage(md);
    }
    if (ambre_table_validate(&res.table) != AMBRE_OK) {
        return FUZZ_F_TABLE_VALID;
    }
    fail = check_result_sanity(&res);
    if (fail != FUZZ_OK) {
        return fail;
    }
    fail = check_idempotent(md, &res);
    if (fail != FUZZ_OK) {
        return fail;
    }
    ambre_journal_cfg_default(&cfg);
    cfg.session_id = res.table.session_id;
    if (ambre_journal_open(&md->disk, &cfg, false, &j) != AMBRE_OK) {
        return FUZZ_F_OPEN;
    }
    return check_read_events(&j, &res.table);
}

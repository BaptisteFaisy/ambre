/*
 * fuzz_child.h — The analysis run inside the forked child (spec 12.10).
 *
 * The child inherits the generated image through fork() copy-on-write and
 * never allocates: everything it needs is pre-allocated by the parent, so
 * fork() from a multi-slot parent stays safe. Its exit code tells the parent
 * which check failed (0 = the image was analysed cleanly).
 */

#ifndef FUZZ_CHILD_H
#define FUZZ_CHILD_H

#include "ambre_explore_internal.h"

/* Exit codes of fuzz_child_run(), mapped to names by fuzz_fail_name(). */
enum {
    FUZZ_OK             = 0,
    FUZZ_F_RECOVER_RC   = 1,  /* ambre_recover returned an unknown code     */
    FUZZ_F_TABLE_VALID  = 2,  /* recovered table fails ambre_table_validate */
    FUZZ_F_TABLE_SANITY = 3,  /* recovered table holds out-of-range values  */
    FUZZ_F_IDEMPOTENT   = 4,  /* second recovery differs (REP-04 / I8)      */
    FUZZ_F_OPEN         = 5,  /* journal_open failed after a good recovery  */
    FUZZ_F_READ_EVENT   = 6,  /* read_event failed or mismatched a declared
                                 event of the recovered table               */
    FUZZ_F_UNKNOWN_ID   = 7,  /* read_event of an absent id != ERR_STATE    */
    FUZZ_F_OPEN_GARBAGE = 8   /* journal_open on a rejected image misbehaved*/
};

const char *fuzz_fail_name(int code);

/* Analyse one image: ambre_recover(), then ambre_journal_read_event() for
 * every event the recovery declares, plus the idempotency re-run.
 * Returns a FUZZ_* exit code. */
int fuzz_child_run(ambre_memdisk_t *md);

#endif /* FUZZ_CHILD_H */

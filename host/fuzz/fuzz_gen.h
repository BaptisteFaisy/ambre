/*
 * fuzz_gen.h — Random disk-image generator for the journal fuzzer.
 *
 * Builds journal images of every nasty family the spec asks for (spec 12.10,
 * "random-data testing"): pure noise, partially written journals, truncated
 * blocks, corrupted allocation tables, torn records. Generation is a pure
 * function of (seed, index): the same pair always yields the same image and
 * the same strategy, so a violation is reproducible from its seed alone.
 *
 * The valid-journal base is produced by the real journal engine on an
 * ambre_memdisk_t (host/explore, reused — no duplication), with the write
 * log enabled after formatting, so cuts can replay an exact prefix of the
 * write history like the crash explorer does.
 */

#ifndef FUZZ_GEN_H
#define FUZZ_GEN_H

#include "ambre_explore_internal.h"
#include "fuzz_rng.h"

/* Image families. The fuzzer picks one per image, from the seeded PRNG. */
enum {
    FUZZ_S_NOISE = 0,   /* every byte is random                            */
    FUZZ_S_SPARSE,      /* zeros + scattered noisy sectors                 */
    FUZZ_S_PARTIAL,     /* valid journal cut after k writes (power cut)    */
    FUZZ_S_TORN_REC,    /* multi-block record with corrupted continuation  */
    FUZZ_S_BAD_TABLE,   /* table copy A and/or B destroyed                 */
    FUZZ_S_BITFLIP,     /* valid journal + random bit flips (media error)  */
    FUZZ_S_MIX,         /* valid journal + a run of noisy sectors          */
    FUZZ_S_COUNT        /* number of strategies                            */
};

const char *fuzz_strategy_name(int strategy);

/* Parent-side workspace: one scratch journal builder + one formatted-base
 * snapshot, reused for every generated image. Never touched by children. */
typedef struct {
    ambre_memdisk_t scratch;
    uint8_t        *base;         /* image right after ambre_journal_format */
    uint64_t        base_size;
} fuzz_gen_ws_t;

int  fuzz_gen_ws_init(fuzz_gen_ws_t *ws);
void fuzz_gen_ws_free(fuzz_gen_ws_t *ws);

/* Fill md->image with image number `index` of a run seeded with `seed`.
 * Returns the strategy used (FUZZ_S_*), or -1 on internal error. */
int fuzz_gen_image(fuzz_gen_ws_t *ws, ambre_memdisk_t *md,
                   uint64_t seed, uint64_t index);

#endif /* FUZZ_GEN_H */

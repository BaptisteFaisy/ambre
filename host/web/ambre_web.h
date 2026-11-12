/*
 * ambre_web.h — JSON API over the real power-cut explorer, for the browser.
 *
 * The website embeds the very same C sources as the PC tools (fmt/ +
 * host/explore/ + host/simdisk/), compiled to WebAssembly: what the browser
 * shows is not a re-implementation, it is ambre-explore running inside the
 * page. The JSON is the only boundary: the C side owns every number, the
 * page only draws them.
 *
 * Every entry point returns a JSON document in a static buffer that stays
 * valid until the next call.
 */

#ifndef AMBRE_WEB_H
#define AMBRE_WEB_H

#include <stdint.h>

/* Cut variants, same order as ambre_explore_internal.h. */
#define AMBRE_WEB_VAR_PREFIX   0
#define AMBRE_WEB_VAR_TORN     1
#define AMBRE_WEB_VAR_CACHE    2
#define AMBRE_WEB_VAR_BITFLIP  3
#define AMBRE_WEB_VAR_TABLE    4
#define AMBRE_WEB_VAR_NESTED   5

/* Variant bits accepted by ambre_web_sweep_start_json(). */
#define AMBRE_WEB_SWEEP_TORN     0x02u
#define AMBRE_WEB_SWEEP_CACHE    0x04u
#define AMBRE_WEB_SWEEP_BITFLIP  0x08u
#define AMBRE_WEB_SWEEP_TABLE    0x10u
#define AMBRE_WEB_SWEEP_NESTED   0x20u

/* Prepare one run: record the ground truth and the op trace.
 * scenario_path may be NULL (synthetic scenario from seed/duration/crash).
 * Returns 0 on success, negative on error. */
int ambre_web_setup(const char *scenario_path, uint64_t seed,
                    uint32_t duration_ms, uint32_t crash_at_ms,
                    uint32_t segment_limit);

/* Release everything the setup allocated. */
void ambre_web_teardown(void);

/* Run one crash state: build the image at the cut, recover it, check I1..I8.
 * torn_bytes, keep_mask, corrupt_off, corrupt_bit and wipe_both_tables are
 * the parameters of the chosen variant; they are ignored by the variants
 * that do not use them. */
const char *ambre_web_state_json(uint32_t prefix, int variant,
                                  uint32_t torn_bytes, uint64_t keep_mask,
                                  uint64_t corrupt_off, uint32_t corrupt_bit,
                                  int wipe_both_tables);

/* Layout of the run: op count, disk size, ground truth (events, their op
 * indices), and the constants the page needs to label things. */
const char *ambre_web_info_json(void);

/* A slice of the op trace: what the journal did, in order. Each write carries
 * the header of the block it wrote (record type, timestamp, sequence), so a
 * page can animate the journal with the real clock and the real records. */
const char *ambre_web_ops_json(uint32_t from, uint32_t count);

/* A slice of the injected class B stream: [ts_us, a_long, a_lat] per ms. */
const char *ambre_web_samples_json(uint32_t from, uint32_t count);

/* What the trace costs: op count, longest single write, total bytes written.
 * The page shows it, and tests/test_web_api.c uses it to prove that every
 * scenario shipped with the website fits the browser build's op-log bound. */
const char *ambre_web_trace_json(void);

/* Start an exploration: build the state space (every reachable cut point and
 * its variants) and reset the counters. */
const char *ambre_web_sweep_start_json(uint32_t max_states, uint32_t variants);

/* Run the next `states` states of the exploration and report the counters. */
const char *ambre_web_sweep_step_json(uint32_t states);

#endif /* AMBRE_WEB_H */

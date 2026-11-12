/*
 * ambre_web_ctx.h — State shared by the browser API modules.
 *
 * One run is alive at a time: the recorded truth, the op trace, the scratch
 * space of the explorer and the exploration counters.
 */

#ifndef AMBRE_WEB_CTX_H
#define AMBRE_WEB_CTX_H

#include "ambre_explore_internal.h"
#include "ambre_web.h"
#include "ambre_web_json.h"
#include <stdio.h>

typedef struct {
    bool                  ready;
    ambre_explore_cfg_t   cfg;
    ambre_simdisk_t       sd;
    ambre_truth_t         truth;
    ambre_explore_ws_t    ws;
    uint64_t              size;
    uint8_t              *crash_img;   /* image at the cut, for the page */
    uint8_t              *after_img;   /* image after recovery, for the page */
    ambre_state_queue_t   q;           /* state space of the exploration */
    ambre_explore_stats_t stats;       /* exploration counters */
    size_t                sweep_done;
    double                sweep_start;
    char                  scenario[160];
} ambre_web_ctx_t;

ambre_web_ctx_t *ambre_web_ctx(void);

/* Human name of a variant, and one line on what it does to the disk. */
const char *ambre_web_variant_name(int variant);
const char *ambre_web_variant_note(int variant);

/* Class A points of an EMERG payload as a JSON array, scaled for a reader:
 * [rel_time_ms, speed_kmh, throttle, brake, rpm, steer_deg, a_long, a_lat,
 *  belt, airbag_lamp, abs, esc]. Used by the state answer and by the run
 * summary, so both show the injected snapshot the same way. */
void ambre_web_points_json(ambre_json_t *j, const ambre_emerg_t *e);

#endif /* AMBRE_WEB_CTX_H */

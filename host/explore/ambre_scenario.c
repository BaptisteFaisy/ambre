/*
 * ambre_scenario.c — Scenario file loader (docs/FORMAT.md §11) + synthetic
 * fallback driven by --seed / --duration-ms / --crash-at-ms.
 *
 * Format: '#' comments, key=value header, then one sample per line:
 *   t_us a_long_mm_s2 a_lat_mm_s2 speed_kmh_x4 throttle brake rpm steer_x2 ext
 */

#include "ambre_explore_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AMBRE_SCEN_MAX_SAMPLES 200000u

static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Deterministic synthetic drive: gentle noise, then a crash pulse. */
static void synth_sample(ambre_scen_sample_t *s, uint32_t i, uint32_t crash_idx)
{
    memset(s, 0, sizeof(*s));
    s->ts_us = (uint64_t)i * 1000u;
    s->a_long_mm_s2 = 12;
    s->a_lat_mm_s2 = -3;
    s->speed_kmh_x4 = (uint16_t)(400u - (i / 100u));
    s->engine_rpm = 800;
    if (crash_idx > 0 && i >= crash_idx && i < crash_idx + 150u) {
        /* 150 ms deceleration ramp: well above the 8 km/h delta-v trigger. */
        s->a_long_mm_s2 = -120000;
        s->brake_active = 1;
        s->ext_flags = 0x01u; /* restraint deployed */
    } else if (crash_idx > 0 && i >= crash_idx + 150u) {
        s->a_long_mm_s2 = -800;
        s->brake_active = 1;
    }
}

static int scenario_synth(const ambre_explore_cfg_t *cfg, ambre_scenario_t *sc)
{
    uint32_t duration_ms = cfg->duration_ms ? cfg->duration_ms : 1000u;
    if (duration_ms > AMBRE_SCEN_MAX_SAMPLES) duration_ms = AMBRE_SCEN_MAX_SAMPLES;
    sc->count = duration_ms; /* 1 kHz */
    sc->s = (ambre_scen_sample_t *)calloc(sc->count, sizeof(*sc->s));
    if (!sc->s) return AMBRE_ERR_IO;
    uint32_t crash_idx = cfg->crash_at_ms;
    if (crash_idx >= sc->count) crash_idx = 0;
    uint32_t seed = (uint32_t)cfg->seed ^ 0x9E3779B9u;
    for (uint32_t i = 0; i < sc->count; i++) {
        synth_sample(&sc->s[i], i, crash_idx);
        if ((i % 97u) == 0u) {
            int32_t jitter = (int32_t)(xorshift(&seed) % 21u) - 10;
            sc->s[i].a_long_mm_s2 += jitter;
        }
    }
    sc->crash_at_ms = crash_idx;
    sc->duration_ms = duration_ms;
    return AMBRE_OK;
}

static bool parse_sample_line(const char *line, ambre_scen_sample_t *s)
{
    unsigned long long ts = 0;
    long al = 0, alat = 0, spd = 0, thr = 0, brk = 0, rpm = 0, steer = 0, ext = 0;
    int n = sscanf(line, "%llu %ld %ld %ld %ld %ld %ld %ld %ld",
                   &ts, &al, &alat, &spd, &thr, &brk, &rpm, &steer, &ext);
    if (n < 2) return false;
    memset(s, 0, sizeof(*s));
    s->ts_us = (uint64_t)ts;
    s->a_long_mm_s2 = (int32_t)al;
    s->a_lat_mm_s2 = (int32_t)alat;
    s->speed_kmh_x4 = (uint16_t)spd;
    s->throttle_pct = (uint8_t)thr;
    s->brake_active = (uint8_t)(brk != 0);
    s->engine_rpm = (uint16_t)rpm;
    s->steer_deg_x2 = (int16_t)steer;
    s->ext_flags = (uint8_t)ext;
    return true;
}

static void parse_header(const char *line, ambre_scenario_t *sc)
{
    if (strncmp(line, "crash_at_ms=", 12) == 0) {
        sc->crash_at_ms = (uint32_t)strtoul(line + 12, NULL, 10);
    } else if (strncmp(line, "duration_ms=", 12) == 0) {
        sc->duration_ms = (uint32_t)strtoul(line + 12, NULL, 10);
    }
}

static int scenario_parse(FILE *f, ambre_scenario_t *sc)
{
    char line[256];
    sc->s = (ambre_scen_sample_t *)calloc(AMBRE_SCEN_MAX_SAMPLES, sizeof(*sc->s));
    if (!sc->s) return AMBRE_ERR_IO;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (strchr(line, '=') != NULL && sc->count == 0) {
            parse_header(line, sc);
            continue;
        }
        if (sc->count >= AMBRE_SCEN_MAX_SAMPLES) break;
        if (parse_sample_line(line, &sc->s[sc->count])) sc->count++;
    }
    if (sc->count == 0) {
        free(sc->s);
        sc->s = NULL;
        return AMBRE_ERR_PARAM;
    }
    return AMBRE_OK;
}

int ambre_scenario_load(const ambre_explore_cfg_t *cfg, ambre_scenario_t *sc)
{
    memset(sc, 0, sizeof(*sc));
    if (cfg->scenario_path) {
        FILE *f = fopen(cfg->scenario_path, "r");
        if (f) {
            int rc = scenario_parse(f, sc);
            fclose(f);
            if (rc == AMBRE_OK) return AMBRE_OK;
        }
    }
    return scenario_synth(cfg, sc);
}

void ambre_scenario_free(ambre_scenario_t *sc)
{
    if (!sc) return;
    free(sc->s);
    sc->s = NULL;
    sc->count = 0;
}

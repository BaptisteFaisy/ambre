/*
 * scenario_file.c — Loader and class B conversion for scenario files
 * (docs/FORMAT.md §11).
 *
 * The loader is deliberately independent from the host explorer's own copy:
 * that one is tied to ambre_explore_cfg_t and lives behind a non-public
 * internal header, so a target process cannot reuse it without dragging the
 * whole crash explorer in. This file is the shareable version: same grammar,
 * same column order, no dependency outside fmt/ and libc.
 *
 * Privacy (VIE-01, VIE-02): the file carries only relative timestamps and
 * whitelisted signals; nothing here can produce an absolute time or a
 * forbidden signal.
 */

#include "scenario_file.h"

#include <stdlib.h>
#include <string.h>

#define LINE_MAX_LEN 256u
#define DEFAULT_RATE_HZ 1000u
/* ext_flags bit 5 marks a sample invalid (DEC-13); it is not an external
 * signal, so it never reaches the EXTERNAL mask. */
#define EXT_SIGNAL_MASK 0x1Fu

/* --------------------------------------------------------------- helpers -- */

static bool is_comment_or_blank(const char *line)
{
    return line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0';
}

static void parse_header_value(const char *key, const char *value,
                               scenario_file_t *sc)
{
    if (strcmp(key, "name") == 0) {
        strncpy(sc->name, value, sizeof(sc->name) - 1u);
        sc->name[sizeof(sc->name) - 1u] = '\0';
    } else if (strcmp(key, "seed") == 0) {
        sc->seed = strtoull(value, NULL, 10);
    } else if (strcmp(key, "rate_hz") == 0) {
        sc->rate_hz = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcmp(key, "duration_ms") == 0) {
        sc->duration_ms = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcmp(key, "crash_at_ms") == 0) {
        sc->crash_at_ms = (uint32_t)strtoul(value, NULL, 10);
    }
}

/* One "key=value" header line; anything else is ignored. */
static void parse_header_line(const char *line, scenario_file_t *sc)
{
    char key[32];
    const char *eq = strchr(line, '=');
    size_t key_len;

    if (eq == NULL) {
        return;
    }
    key_len = (size_t)(eq - line);
    if (key_len == 0u || key_len >= sizeof(key)) {
        return;
    }
    memcpy(key, line, key_len);
    key[key_len] = '\0';
    parse_header_value(key, eq + 1, sc);
}

/* One sample line: t_us a_long a_lat speed_kmh_x4 throttle brake rpm steer ext */
static bool parse_row_line(const char *line, scenario_row_t *row)
{
    unsigned long long ts = 0;
    long a_long = 0, a_lat = 0, speed = 0, throttle = 0, brake = 0;
    long rpm = 0, steer = 0, ext = 0;
    int n = sscanf(line, "%llu %ld %ld %ld %ld %ld %ld %ld %ld",
                   &ts, &a_long, &a_lat, &speed, &throttle, &brake,
                   &rpm, &steer, &ext);

    if (n < 2) {
        return false;
    }
    memset(row, 0, sizeof(*row));
    row->ts_us = (uint64_t)ts;
    row->a_long_mm_s2 = (int32_t)a_long;
    row->a_lat_mm_s2 = (int32_t)a_lat;
    row->speed_kmh_x4 = (uint16_t)speed;
    row->throttle_pct = (uint8_t)throttle;
    row->brake_active = (uint8_t)(brake != 0);
    row->engine_rpm = (uint16_t)rpm;
    row->steer_deg_x2 = (int16_t)steer;
    row->ext_flags = (uint8_t)ext;
    return true;
}

static bool is_header_line(const char *line)
{
    return strchr(line, '=') != NULL;
}

/* Drop the trailing end-of-line so a header value never carries it. */
static void strip_eol(char *line)
{
    size_t n = strlen(line);

    while (n > 0u && (line[n - 1u] == '\n' || line[n - 1u] == '\r')) {
        line[--n] = '\0';
    }
}

/* ---------------------------------------------------------------- loading -- */

/* First pass: how many sample lines does the file hold? */
static int count_rows(FILE *f, uint32_t *out)
{
    char line[LINE_MAX_LEN];
    uint32_t count = 0u;

    while (fgets(line, sizeof(line), f) != NULL) {
        if (is_comment_or_blank(line) || is_header_line(line)) {
            continue;
        }
        count++;
    }
    if (count == 0u) {
        return -1;
    }
    if (count > SCENARIO_MAX_SAMPLES) {
        return -2;
    }
    *out = count;
    return 0;
}

/* Second pass: parse the header and every sample line. */
static int fill_rows(FILE *f, scenario_file_t *sc)
{
    char line[LINE_MAX_LEN];

    while (fgets(line, sizeof(line), f) != NULL) {
        if (is_comment_or_blank(line)) {
            continue;
        }
        strip_eol(line);
        if (is_header_line(line)) {
            parse_header_line(line, sc);
            continue;
        }
        if (sc->count >= sc->capacity) {
            return -2;
        }
        if (parse_row_line(line, &sc->rows[sc->count])) {
            sc->count++;
        }
    }
    return (sc->count > 0u) ? 0 : -1;
}

int scenario_file_load(const char *path, scenario_file_t *out)
{
    FILE *f;
    uint32_t capacity = 0u;
    int rc;

    if (path == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->rate_hz = DEFAULT_RATE_HZ;
    f = fopen(path, "r");
    if (f == NULL) {
        return -3;
    }
    rc = count_rows(f, &capacity);
    if (rc == 0) {
        out->capacity = capacity;
        out->rows = (scenario_row_t *)calloc(capacity, sizeof(*out->rows));
        if (out->rows == NULL) {
            rc = -4;
        } else {
            rewind(f);
            rc = fill_rows(f, out);
        }
    }
    fclose(f);
    if (rc != 0) {
        scenario_file_free(out);
        return rc;
    }
    if (out->duration_ms == 0u && out->count > 0u) {
        out->duration_ms = (uint32_t)(out->rows[out->count - 1u].ts_us / 1000u);
    }
    if (out->rate_hz == 0u) {
        out->rate_hz = DEFAULT_RATE_HZ;
    }
    return 0;
}

void scenario_file_free(scenario_file_t *sc)
{
    if (sc == NULL) {
        return;
    }
    free(sc->rows);
    sc->rows = NULL;
    sc->count = 0u;
    sc->capacity = 0u;
}

/* ------------------------------------------------------------- whitelist -- */

bool scenario_signal_whitelisted(uint16_t signal_id)
{
    if (signal_id >= 1u && signal_id <= 3u) {
        return true;                    /* accelerations, 1 kHz              */
    }
    if (signal_id >= 10u && signal_id <= 13u) {
        return true;                    /* wheel speeds, 100 Hz              */
    }
    if (signal_id >= 20u && signal_id <= 25u) {
        return true;                    /* dynamics, 100 Hz                  */
    }
    if (signal_id >= 40u && signal_id <= 43u) {
        return true;                    /* states, 10 Hz                     */
    }
    return false;                       /* 90..99 and everything else        */
}

/* ------------------------------------------------------ class B conversion - */

static uint32_t put_sample(ambre_sample_t *out, uint32_t n, uint32_t max,
                           uint16_t signal_id, int32_t value, uint8_t quality)
{
    if (n >= max) {
        return n;
    }
    out[n].signal_id = signal_id;
    out[n].quality = quality;
    out[n].pad = 0u;
    out[n].value = value;
    return n + 1u;
}

/* ADAS states bitmask (signal 40, docs/FORMAT.md §5): bit0 AEB, bit3 emergency
 * brake, both taken from the scenario's ext_flags. Bits the scenario does not
 * carry (ACC, LKA) are left at zero rather than invented. */
static int32_t adas_states_from_ext(uint8_t ext_flags)
{
    int32_t mask = 0;

    if ((ext_flags & EXT_FLAG_AEB) != 0u) {
        mask |= 1;
    }
    if ((ext_flags & EXT_FLAG_EMERG_BRAKE) != 0u) {
        mask |= 8;
    }
    return mask;
}

/* The 100 Hz group: refreshed once every 10 rows of a 1 kHz scenario. */
static uint32_t put_slow_100hz(const scenario_row_t *row, uint32_t index,
                               ambre_sample_t *out, uint32_t n, uint32_t max)
{
    if (index % 10u != 0u) {
        return n;
    }
    n = put_sample(out, n, max, SIG_STEER_ANGLE, (int32_t)row->steer_deg_x2, 0u);
    n = put_sample(out, n, max, SIG_THROTTLE, (int32_t)row->throttle_pct * 2, 0u);
    n = put_sample(out, n, max, SIG_BRAKE, (int32_t)row->brake_active, 0u);
    n = put_sample(out, n, max, SIG_ENGINE_RPM, (int32_t)row->engine_rpm, 0u);
    return n;
}

/* The 10 Hz group: refreshed once every 100 rows. */
static uint32_t put_slow_10hz(const scenario_row_t *row, uint32_t index,
                              ambre_sample_t *out, uint32_t n, uint32_t max)
{
    if (index % 100u != 0u) {
        return n;
    }
    return put_sample(out, n, max, SIG_ADAS_STATES,
                      adas_states_from_ext(row->ext_flags), 0u);
}

uint32_t scenario_row_to_samples(const scenario_row_t *row, uint32_t row_index,
                                 ambre_sample_t *out, uint32_t max_samples)
{
    uint32_t n = 0u;
    uint8_t quality = ((row->ext_flags & EXT_FLAG_INVALID) != 0u) ? 2u : 0u;

    if (row == NULL || out == NULL) {
        return 0u;
    }
    /* 1 kHz accelerations. Signal 3 (vertical) is not carried by the scenario
     * format, so it is never emitted: a replay invents no data. */
    n = put_sample(out, n, max_samples, SIG_ACCEL_X, row->a_long_mm_s2, quality);
    n = put_sample(out, n, max_samples, SIG_ACCEL_Y, row->a_lat_mm_s2, quality);
    n = put_slow_100hz(row, row_index, out, n, max_samples);
    n = put_slow_10hz(row, row_index, out, n, max_samples);
    return n;
}

uint32_t scenario_external_mask(const scenario_row_t *row)
{
    return (row == NULL) ? 0u : (uint32_t)(row->ext_flags & EXT_SIGNAL_MASK);
}

uint32_t scenario_row_period_us(const scenario_file_t *sc)
{
    if (sc == NULL || sc->rate_hz == 0u) {
        return 1000u;
    }
    return 1000000u / sc->rate_hz;
}

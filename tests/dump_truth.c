/*
 * dump_truth.c — CLI to convert .vec trigger vectors to .dec decision lines.
 *
 * Usage: ambre-dump-truth --vectors <in.vec>
 *        ambre-dump-truth --help
 *
 * Reads .vec format (docs/AGENT-BRIEF.md §"Format des vecteurs"):
 *   S <id> <mode> <dv_trigger> <dv_lock> <dv_t0> <win_trigger_ms>
 *     <win_t0_long> <win_t0_lat> <has_front_restraint> <same_crash_window_ms>
 *   A <t_us> <a_long_mm_s2> <a_lat_mm_s2> <ext_flags>
 *   E
 *
 * Writes .dec lines (one per sample) to stdout, byte-for-byte identical to
 * Python twin (DEC-12). Booleans 0/1, single space, no trailing space, \n.
 * Exit codes: 0 = success, 2 = parse/IO error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "ambre_trigger.h"

#define LINE_BUF_SIZE 4096

static void print_help(void)
{
    printf("Usage: ambre-dump-truth --vectors <in.vec>\n");
    printf("       ambre-dump-truth --help\n\n");
    printf("Convert .vec trigger vectors to .dec decision lines (stdout).\n");
}

static int parse_args(int argc, char **argv, const char **vectors_path)
{
    *vectors_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_help();
            return 1;
        } else if (strcmp(argv[i], "--vectors") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --vectors requires a file path\n");
                return -1;
            }
            *vectors_path = argv[i + 1];
            i++;
        } else {
            fprintf(stderr, "error: unknown argument '%s'\n", argv[i]);
            return -1;
        }
    }
    if (*vectors_path == NULL) {
        fprintf(stderr, "error: --vectors is required\n");
        return -1;
    }
    return 0;
}

static int parse_scenario_line(const char *line, ambre_trig_cfg_t *cfg, uint32_t *scen_id)
{
    int id, mode, dv_trig, dv_lock, dv_t0, win_trig, win_t0_long, win_t0_lat, has_fr, same_win;
    int n = sscanf(line, "S %d %d %d %d %d %d %d %d %d %d",
                   &id, &mode, &dv_trig, &dv_lock, &dv_t0,
                   &win_trig, &win_t0_long, &win_t0_lat, &has_fr, &same_win);
    if (n != 10) {
        fprintf(stderr, "error: malformed S line (expected 10 fields, got %d)\n", n);
        return -1;
    }
    *scen_id = (uint32_t)id;
    cfg->mode = (uint8_t)mode;
    cfg->dv_trigger_mm_s = (int32_t)dv_trig;
    cfg->dv_lock_mm_s = (int32_t)dv_lock;
    cfg->dv_t0_mm_s = (int32_t)dv_t0;
    cfg->win_trigger_ms = (uint16_t)win_trig;
    cfg->win_t0_long_ms = (uint16_t)win_t0_long;
    cfg->win_t0_lat_ms = (uint16_t)win_t0_lat;
    cfg->has_front_restraint = (uint8_t)has_fr;
    cfg->same_crash_window_ms = (uint32_t)same_win;
    return 0;
}

static int parse_sample_line(const char *line, uint64_t *ts_us, int32_t *a_long,
                             int32_t *a_lat, uint8_t *ext_flags)
{
    unsigned long long ts;
    int al, aa, flags;
    int n = sscanf(line, "A %llu %d %d %d", &ts, &al, &aa, &flags);
    if (n != 4) {
        fprintf(stderr, "error: malformed A line (expected 4 fields, got %d)\n", n);
        return -1;
    }
    *ts_us = (uint64_t)ts;
    *a_long = (int32_t)al;
    *a_lat = (int32_t)aa;
    *ext_flags = (uint8_t)flags;
    return 0;
}

static void print_dec_line(uint32_t scen_id, uint32_t idx, const ambre_trig_out_t *out)
{
    printf("D %" PRIu32 " %" PRIu32 " %d %d %d %" PRIu8 " %" PRIu64 " %" PRIu32 " %" PRIu32
           " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64
           " %" PRId64 " %" PRId64 " %" PRIu16 " %" PRIu16
           " %" PRId64 " %" PRId64 "\n",
           scen_id, idx,
           out->fired ? 1 : 0, out->event_end ? 1 : 0, out->locked ? 1 : 0,
           out->reason, out->t0_ts_us, out->event_number, out->time_since_first_ms,
           out->dv_long_mm_s, out->dv_lat_mm_s,
           out->dv_since_t0_long_mm_s, out->dv_since_t0_lat_mm_s,
           out->max_dv_long_mm_s, out->max_dv_lat_mm_s,
           out->max_dv_long_at_ms, out->max_dv_lat_at_ms,
           out->win_long_mm_s, out->win_lat_mm_s);
}

static void decode_ext(uint8_t flags, ambre_trig_ext_t *ext)
{
    memset(ext, 0, sizeof(*ext));
    ext->restraint_deployed = (flags & 0x01) != 0;
    ext->vru_activated = (flags & 0x02) != 0;
    ext->emergency_brake = (flags & 0x04) != 0;
    ext->aeb_intervention = (flags & 0x08) != 0;
    ext->service_command = (flags & 0x10) != 0;
    ext->sample_invalid = (flags & 0x20) != 0;
}

static int handle_scenario_line(const char *line, ambre_trig_cfg_t *cfg,
                                ambre_trig_state_t *st, uint32_t *scen_id,
                                uint32_t *sample_idx, bool *in_scenario)
{
    if (*in_scenario) {
        fprintf(stderr, "error: new scenario started before 'E' (line: %s)\n", line);
        return 2;
    }
    if (parse_scenario_line(line, cfg, scen_id) != 0) return 2;
    ambre_trig_init(st, cfg);
    *in_scenario = true;
    *sample_idx = 0;
    return 0;
}

static int handle_sample_line(const char *line, ambre_trig_state_t *st,
                              uint32_t scen_id, uint32_t *sample_idx, bool in_scenario)
{
    if (!in_scenario) {
        fprintf(stderr, "error: A line outside scenario (line: %s)\n", line);
        return 2;
    }
    uint64_t ts_us;
    int32_t a_long, a_lat;
    uint8_t ext_flags;
    if (parse_sample_line(line, &ts_us, &a_long, &a_lat, &ext_flags) != 0) return 2;
    ambre_trig_ext_t ext;
    decode_ext(ext_flags, &ext);
    ambre_trig_out_t out;
    ambre_trig_push(st, ts_us, a_long, a_lat, &ext, &out);
    print_dec_line(scen_id, *sample_idx, &out);
    (*sample_idx)++;
    return 0;
}

/* Strip trailing CR/LF in place; returns the stripped length. */
static size_t chomp(char *line)
{
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
    if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
    return len;
}

/* Process one line. Returns 0 on success, 2 on parse error. */
static int process_line(char *line, ambre_trig_cfg_t *cfg, ambre_trig_state_t *st,
                        uint32_t *scen_id, uint32_t *sample_idx, bool *in_scenario)
{
    if (line[0] == '\0' || line[0] == '#') return 0;

    if (line[0] == 'S' && line[1] == ' ') {
        return handle_scenario_line(line, cfg, st, scen_id, sample_idx, in_scenario);
    }
    if (line[0] == 'A' && line[1] == ' ') {
        return handle_sample_line(line, st, *scen_id, sample_idx, *in_scenario);
    }
    if (line[0] == 'E' && line[1] == '\0') {
        if (!*in_scenario) {
            fprintf(stderr, "error: E line without scenario\n");
            return 2;
        }
        *in_scenario = false;
        return 0;
    }
    fprintf(stderr, "error: unknown line format (line: %s)\n", line);
    return 2;
}

int main(int argc, char **argv)
{
    const char *vectors_path = NULL;
    int pr = parse_args(argc, argv, &vectors_path);
    if (pr != 0) return (pr > 0) ? 0 : 2;

    FILE *fp = fopen(vectors_path, "r");
    if (!fp) {
        fprintf(stderr, "error: cannot open file '%s'\n", vectors_path);
        return 2;
    }

    char line[LINE_BUF_SIZE];
    ambre_trig_cfg_t cfg;
    ambre_trig_state_t st;
    uint32_t scen_id = 0, sample_idx = 0;
    bool in_scenario = false;
    int rc = 0;

    while (fgets(line, sizeof(line), fp)) {
        chomp(line);
        rc = process_line(line, &cfg, &st, &scen_id, &sample_idx, &in_scenario);
        if (rc != 0) break;
    }

    if (rc == 0 && in_scenario) {
        fprintf(stderr, "error: file ended while scenario still open\n");
        rc = 2;
    }

    fclose(fp);
    return rc;
}

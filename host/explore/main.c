/*
 * main.c — ambre-explore CLI entry point
 *
 * Rules: files < 500 lines, functions < 50 lines, C11, no warnings.
 */

#include "ambre_explore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --scenario <file.txt> [--seed S] [--duration-ms N]\n"
        "       [--crash-at-ms N] [--segment-limit N] [--states N]\n"
        "       [--jobs N] [--variants all|prefix,torn,cache,bitflip,table]\n"
        "       [--nested] [--report out.json] [--violation-dir DIR]\n"
        "       [--export-image out.img]\n", prog);
}

static int parse_variants(ambre_explore_cfg_t *cfg, const char *arg)
{
    if (strcmp(arg, "all") == 0) {
        cfg->variant_torn = true;
        cfg->variant_volatile_cache = true;
        cfg->variant_bitflip = true;
        cfg->variant_table_wipe = true;
        return 0;
    }
    cfg->variant_torn = (strstr(arg, "torn") != NULL);
    cfg->variant_volatile_cache = (strstr(arg, "cache") != NULL);
    cfg->variant_bitflip = (strstr(arg, "bitflip") != NULL);
    cfg->variant_table_wipe = (strstr(arg, "table") != NULL);
    return 0;
}

static int parse_arg(int argc, char **argv, int *i, ambre_explore_cfg_t *cfg,
                     const char **export_image)
{
    if (strcmp(argv[*i], "--scenario") == 0 && *i + 1 < argc) {
        cfg->scenario_path = argv[++(*i)];
    } else if (strcmp(argv[*i], "--seed") == 0 && *i + 1 < argc) {
        cfg->seed = strtoull(argv[++(*i)], NULL, 10);
    } else if (strcmp(argv[*i], "--duration-ms") == 0 && *i + 1 < argc) {
        cfg->duration_ms = (uint32_t)strtoul(argv[++(*i)], NULL, 10);
    } else if (strcmp(argv[*i], "--crash-at-ms") == 0 && *i + 1 < argc) {
        cfg->crash_at_ms = (uint32_t)strtoul(argv[++(*i)], NULL, 10);
    } else if (strcmp(argv[*i], "--segment-limit") == 0 && *i + 1 < argc) {
        cfg->segment_limit = (uint32_t)strtoul(argv[++(*i)], NULL, 10);
    } else if (strcmp(argv[*i], "--states") == 0 && *i + 1 < argc) {
        cfg->max_states = (uint32_t)strtoul(argv[++(*i)], NULL, 10);
    } else if (strcmp(argv[*i], "--jobs") == 0 && *i + 1 < argc) {
        cfg->jobs = (uint32_t)strtoul(argv[++(*i)], NULL, 10);
    } else if (strcmp(argv[*i], "--variants") == 0 && *i + 1 < argc) {
        parse_variants(cfg, argv[++(*i)]);
    } else if (strcmp(argv[*i], "--nested") == 0) {
        cfg->nested_recovery = true;
    } else if (strcmp(argv[*i], "--report") == 0 && *i + 1 < argc) {
        cfg->report_json = argv[++(*i)];
    } else if (strcmp(argv[*i], "--violation-dir") == 0 && *i + 1 < argc) {
        cfg->violation_dir = argv[++(*i)];
    } else if (strcmp(argv[*i], "--export-image") == 0 && *i + 1 < argc) {
        *export_image = argv[++(*i)];
    } else {
        return -1;
    }
    return 0;
}

static int export_disk_image(const ambre_explore_cfg_t *cfg, const char *out_path)
{
    ambre_simdisk_t sd;
    uint64_t disk_size = AMBRE_FIRST_DATA_OFFSET +
                         (uint64_t)cfg->segment_limit * AMBRE_SEGMENT_SIZE;
    if (ambre_simdisk_create(&sd, disk_size) != AMBRE_OK) return -1;
    ambre_truth_t truth;
    ambre_explore_record_truth(cfg, &sd, &truth);
    ambre_truth_free(&truth);
    FILE *f = fopen(out_path, "wb");
    if (f) {
        (void)fwrite(sd.image, 1, (size_t)sd.size, f);
        fclose(f);
    }
    ambre_simdisk_destroy(&sd);
    return f ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc <= 1) { print_usage(argv[0]); return 2; }
    ambre_explore_cfg_t cfg;
    ambre_explore_cfg_default(&cfg);
    const char *export_image = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]); return 0;
        }
        if (parse_arg(argc, argv, &i, &cfg, &export_image) != 0) {
            print_usage(argv[0]); return 2;
        }
    }
    if (export_image) {
        return export_disk_image(&cfg, export_image) == 0 ? 0 : 1;
    }
    ambre_explore_stats_t stats;
    int rc = ambre_explore_run(&cfg, &stats);
    double rate = stats.seconds > 0 ? (double)stats.states_explored / stats.seconds : 0.0;
    printf("ambre-explore: %lu states in %.3fs (%.0f states/s), %lu violations\n",
           (unsigned long)stats.states_explored, stats.seconds, rate,
           (unsigned long)stats.violations);
    if (stats.violations > 0 && stats.first_violation[0] != '\0') {
        printf("First violation: %s\n", stats.first_violation);
    }
    return (rc == 0 && stats.violations == 0) ? 0 : 1;
}

/*
 * scenario_file.h — Loader for Ambre scenario files (docs/FORMAT.md §11).
 *
 * Scenarios drive both the crash explorer and the edr-replay process:
 * a plain text format, '#' comments, 'key=value' header, then one sample per
 * line. Samples are 1 kHz acceleration + the slow signals repeated at their
 * own rate (100 Hz / 10 Hz).
 *
 * Whitelist: any signal_id outside the list of docs/FORMAT.md §5 is rejected
 * and counted (ACQ-04).
 */

#ifndef SCENARIO_FILE_H
#define SCENARIO_FILE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "../../fmt/ambre_types.h"

#define SCENARIO_MAX_SAMPLES 120000u  /* up to 120 s at 1 kHz                */

/* Signal IDs from docs/FORMAT.md §5 (VIE-01, ACQ-03, ACQ-04). */
#define SIG_ACCEL_X         1u
#define SIG_ACCEL_Y         2u
#define SIG_ACCEL_Z         3u
#define SIG_WHEEL_FL       10u
#define SIG_WHEEL_FR       11u
#define SIG_WHEEL_RL       12u
#define SIG_WHEEL_RR       13u
#define SIG_STEER_ANGLE    20u
#define SIG_THROTTLE       21u
#define SIG_BRAKE          22u
#define SIG_YAW_RATE       23u
#define SIG_ROLL_RATE      24u
#define SIG_ENGINE_RPM     25u
#define SIG_ADAS_STATES    40u
#define SIG_BELT_STATES    41u
#define SIG_LAMP_STATES    42u
#define SIG_GEAR_SELECTOR  43u

/* ext_flags bitmask (docs/FORMAT.md §11) */
#define EXT_FLAG_RESTRAINT      (1u << 0)
#define EXT_FLAG_VRU            (1u << 1)
#define EXT_FLAG_EMERG_BRAKE    (1u << 2)
#define EXT_FLAG_AEB            (1u << 3)
#define EXT_FLAG_SERVICE        (1u << 4)
#define EXT_FLAG_INVALID        (1u << 5)

typedef struct {
    uint64_t ts_us;
    int32_t  a_long_mm_s2;
    int32_t  a_lat_mm_s2;
    uint16_t speed_kmh_x4;
    uint8_t  throttle_pct;
    uint8_t  brake_active;
    uint16_t engine_rpm;
    int16_t  steer_deg_x2;
    uint8_t  ext_flags;
} scenario_row_t;

typedef struct {
    char     name[64];
    uint64_t seed;
    uint32_t rate_hz;
    uint32_t duration_ms;
    uint32_t crash_at_ms;
    uint32_t count;
    uint32_t capacity;      /* rows allocated; count <= capacity           */
    scenario_row_t *rows;
} scenario_file_t;

/* True when signal_id is whitelisted (FORMAT.md §5). Returns false for any
 * forbidden signal (90..99) or unassigned id. */
bool scenario_signal_whitelisted(uint16_t signal_id);

/* Load a scenario file from disk. Allocates rows with malloc(); free with
 * scenario_file_free(). Returns 0 on success, negative error code on failure. */
int scenario_file_load(const char *path, scenario_file_t *out);

/* Release memory allocated for rows. */
void scenario_file_free(scenario_file_t *sc);

/* Convert a single scenario row at 1 kHz into its class B samples.
 * Emits the 1 kHz accelerations on every row, the 100 Hz group once every 10
 * rows and the 10 Hz group once every 100 rows, so that a frame covering 10 ms
 * holds exactly one sample per slow signal at its frame start (the cadence the
 * reader of docs/FORMAT.md §12 reconstructs from first_ts_us).
 * Returns the number of samples written into `out` (at most max_samples).
 * Signals the scenario format does not carry (vertical acceleration, wheel
 * speeds, yaw/roll rates, belt and lamp states, gear) are never emitted: a
 * replay invents no data. */
uint32_t scenario_row_to_samples(const scenario_row_t *row,
                                 uint32_t row_index,
                                 ambre_sample_t *out,
                                 uint32_t max_samples);

/* The external-signal bits of a row (ext_flags & 0x1F): bit0 restraint,
 * bit1 VRU, bit2 emergency brake, bit3 AEB, bit4 service command. Bit5
 * (sample invalid, DEC-13) is not an external signal and is carried instead
 * by the samples' quality field. */
uint32_t scenario_external_mask(const scenario_row_t *row);

/* Microseconds between two consecutive rows (1000 us for a 1 kHz scenario). */
uint32_t scenario_row_period_us(const scenario_file_t *sc);

#endif /* SCENARIO_FILE_H */

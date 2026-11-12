/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <sys/slog2.h> - building on the PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real QNX header from the SDP is used on target; this file exists so the
 * QNX glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirement: QNX-08 (write to the system log; the driver's counters
 * are published through the same backend).
 *
 * Provenance: QNX SDP 8.0 C Library Reference, slog2_register(), slog2f():
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/slog2_register.html
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/slog2f.html
 * Severities and buffer configuration fields follow that page; their numeric
 * values are symbolic on the PC (the drivers only pass them on).
 */
#ifndef PGW_STUB_SYS_SLOG2_H
#define PGW_STUB_SYS_SLOG2_H

#include <stdarg.h>
#include <stdint.h>

/* A buffer handle. QNX documents it as an opaque object passed by address; it
 * is complete here so a driver can keep an array of handles in static storage
 * (the drivers never look inside it). */
/* Real SDP 8.0 (sys/slog2.h line 102): an opaque pointer, not a struct. */
typedef struct slog2_buffer_meta *slog2_buffer_t;

/* Severity levels, from the slog2_register()/slog2f() pages. */
#define SLOG2_SHUTDOWN   0
#define SLOG2_CRITICAL   1
#define SLOG2_ERROR      2
#define SLOG2_WARNING    3
#define SLOG2_NOTICE     4
#define SLOG2_INFO       5
#define SLOG2_DEBUG1     6
#define SLOG2_DEBUG2     7

/* Corrected 2026-11-12 against the real $QNX_TARGET/usr/include/sys/slog2.h
 * (lines 35, 107-133): one buffer is sized in 4K PAGES, the array is called
 * buffer_config[], and there is no SLOG2_NO_FLAGS. */
#define SLOG2_MAX_BUFFERS 4

typedef struct _slog2_buffer_config {
    const char *buffer_name;
    int         num_pages;
} slog2_buffer_config_t;

typedef struct _slog2_buffer_set_config {
    int                   num_buffers;
    const char           *buffer_set_name;
    uint8_t               verbosity_level;
    slog2_buffer_config_t buffer_config[SLOG2_MAX_BUFFERS];
    uint32_t              max_retries;
} slog2_buffer_set_config_t;

/* slog2_info_t, the metadata carried by one record. */
typedef struct _slog2_info {
    uint32_t milliseconds;
    uint32_t sequence_number;
    uint16_t flags;
    uint16_t severity;
    const char *filename;
    const char *function;
    uint32_t line;
} slog2_info_t;

int slog2_register(const slog2_buffer_set_config_t *config,
                   slog2_buffer_t *handles, uint32_t flags);
int slog2_reset(void);
slog2_buffer_t *slog2_set_default_buffer(slog2_buffer_t *buffer);
int slog2f(slog2_buffer_t buffer, uint16_t code, uint8_t severity,
           const char *format, ...);
int slog2sdk_register(const slog2_buffer_set_config_t *config,
                      slog2_buffer_t *handles, uint32_t flags);

#endif /* PGW_STUB_SYS_SLOG2_H */

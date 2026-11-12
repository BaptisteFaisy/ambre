/*
 * main_wasm.c — WebAssembly entry points of the browser emulator.
 *
 * Thin layer only: every function forwards to ambre_web_*, the same API the
 * native test (tests/test_web_api.c) drives. Numbers cross the boundary as
 * doubles because JavaScript has no 64-bit integer type; the 64-bit keep_mask
 * of the volatile-cache variant is passed as two 32-bit halves.
 *
 * Build: scripts/build-web.sh (emcc). This file is the only one that includes
 * an Emscripten header, so the rest of host/web/ compiles unchanged on PC.
 */

#include <emscripten/emscripten.h>
#include "ambre_crc32c.h"
#include "ambre_web.h"

/* Self-test the page runs at load time: CRC-32C of "123456789" must be
 * 0xE3069283 (docs/FORMAT.md section 2). Anything else means the module was
 * mis-built and every number it would show is worthless. */
EMSCRIPTEN_KEEPALIVE unsigned web_crc_selftest(void)
{
    static const uint8_t probe[] = "123456789";

    return ambre_crc32c(0u, probe, 9u);
}

EMSCRIPTEN_KEEPALIVE int web_setup(const char *scenario_path, double seed,
                                   double duration_ms, double crash_at_ms,
                                   double segment_limit)
{
    return ambre_web_setup(scenario_path, (uint64_t)seed,
                            (uint32_t)duration_ms, (uint32_t)crash_at_ms,
                            (uint32_t)segment_limit);
}

EMSCRIPTEN_KEEPALIVE void web_teardown(void)
{
    ambre_web_teardown();
}

EMSCRIPTEN_KEEPALIVE const char *web_info(void)
{
    return ambre_web_info_json();
}

EMSCRIPTEN_KEEPALIVE const char *web_ops(double from, double count)
{
    return ambre_web_ops_json((uint32_t)from, (uint32_t)count);
}

EMSCRIPTEN_KEEPALIVE const char *web_samples(double from, double count)
{
    return ambre_web_samples_json((uint32_t)from, (uint32_t)count);
}

EMSCRIPTEN_KEEPALIVE const char *web_trace(void)
{
    return ambre_web_trace_json();
}

EMSCRIPTEN_KEEPALIVE const char *web_state(double prefix, double variant,
                                           double torn_bytes, double keep_lo,
                                           double keep_hi, double corrupt_off,
                                           double corrupt_bit, double wipe_both)
{
    uint64_t mask = ((uint64_t)(uint32_t)keep_hi << 32) | (uint32_t)keep_lo;

    return ambre_web_state_json((uint32_t)prefix, (int)variant,
                                (uint32_t)torn_bytes, mask,
                                (uint64_t)corrupt_off, (uint32_t)corrupt_bit,
                                (int)wipe_both);
}

EMSCRIPTEN_KEEPALIVE const char *web_sweep_start(double max_states,
                                                 double variants)
{
    return ambre_web_sweep_start_json((uint32_t)max_states, (uint32_t)variants);
}

EMSCRIPTEN_KEEPALIVE const char *web_sweep_step(double states)
{
    return ambre_web_sweep_step_json((uint32_t)states);
}

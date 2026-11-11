/*
 * fuzz_rng.h — Deterministic PRNG for the journal fuzzer.
 *
 * splitmix64 (public-domain algorithm, Vigna 2014): small, fast, and its
 * output depends only on the seed, so a fuzzing run is reproducible from
 * --seed alone, on any machine and any compiler.
 */

#ifndef FUZZ_RNG_H
#define FUZZ_RNG_H

#include <stdint.h>

typedef struct {
    uint64_t state;
} fuzz_rng;

/* Seed the generator. Same seed => same stream, everywhere. */
static inline void fuzz_rng_seed(fuzz_rng *r, uint64_t seed)
{
    r->state = seed;
}

static inline uint64_t fuzz_rng_next(fuzz_rng *r)
{
    uint64_t z = (r->state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Integer in [0, n) with n > 0. Plain modulo: the tiny bias is irrelevant
 * for fuzzing, and it keeps the header strict ISO C11 (-Wpedantic clean). */
static inline uint64_t fuzz_rng_below(fuzz_rng *r, uint64_t n)
{
    return fuzz_rng_next(r) % n;
}

/* Combine two 64-bit values into one well-mixed seed (staff variant hash). */
static inline uint64_t fuzz_mix64(uint64_t a, uint64_t b)
{
    uint64_t h = a * 0x9E3779B97F4A7C15ull;
    h ^= b + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
    return h ^ (h >> 27);
}

#endif /* FUZZ_RNG_H */

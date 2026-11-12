/*
 * ambre_web_limits.h — Bounds of the browser build, in one place.
 *
 * scripts/build-web.sh reads AMBRE_WEB_SIMDISK_MAX_OPS from this file and
 * passes it to emcc as -DAMBRE_SIMDISK_MAX_OPS, and tests/test_web_api.c
 * asserts that every scenario shipped with the website stays inside it. So the
 * bound is never a guess: it is either enough for the published scenarios, or
 * the test fails.
 *
 * Why shrink it at all: the PC simdisk pre-allocates a payload slot per logged
 * write (400 000 x 8 KiB). A web page has no business reserving gigabytes.
 * Exceeding the bound is never silent — ambre_simdisk_write() refuses the
 * write with AMBRE_ERR_TOO_BIG and the run reports the error.
 */

#ifndef AMBRE_WEB_LIMITS_H
#define AMBRE_WEB_LIMITS_H

#define AMBRE_WEB_SIMDISK_MAX_OPS 12000u

#endif /* AMBRE_WEB_LIMITS_H */

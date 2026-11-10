/*
 * ambre_test_util.c — harness support + small shared fixtures.
 * Shared by every test binary: no duplication of the failure printer.
 */

#include "ambre_test.h"
#include <stdarg.h>

void ambre_test_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    ambre_test_failures++;
    printf("\n    FAIL %s:%d in %s: ", file, line, ambre_test_current);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n    %-42s", "");
    fflush(stdout);
}

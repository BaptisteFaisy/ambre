#!/bin/sh
# Measure the line coverage of the critical portable library (fmt/).
#
# Spec 12.10 asks for "coverage of the format library above 95% of the lines".
# The CI image has gcov but neither lcov nor gcovr, so this script:
#
#   1. compiles fmt/*.c with --coverage (gcov instrumentation);
#   2. compiles the rest of the library (simdisk, crash explorer) without it;
#   3. builds and runs every C test suite (tests/test_*.c) that compiles;
#   4. runs gcov on the instrumented objects;
#   5. calls scripts/coverage_report.py to print the report and write JSON.
#
# The build directory is isolated (never the shared build/): BUILD=build-cov by
# default. Usage:
#
#   sh scripts/coverage.sh                 # report + JSON, no gate
#   COV_MIN=95 sh scripts/coverage.sh      # fail when below 95%
#   BUILD=build-x sh scripts/coverage.sh   # another isolated build directory
#
# A test suite that does not compile (for example a file another agent is still
# writing) is reported and skipped: the measurement stays honest about which
# suites it used. The script never touches the sources: it only reads fmt/.

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-build-cov}
COV_MIN=${COV_MIN:-0}
CC=${CC:-gcc}
PYTHON=${PYTHON:-python3}

WARN="-Wall -Wextra -Wshadow -Wpedantic -Werror"
CFLAGS="-std=c11 -O0 -g $WARN"
INCLUDES="-Ifmt -I. -Itests -Ihost/simdisk -Ihost/explore -Itarget -Itarget/port -Itarget/ipc"

OBJDIR="$BUILD/obj"
GCOVDIR="$BUILD/gcov"
JSON="$BUILD/coverage.json"

cd "$ROOT"
rm -rf "$OBJDIR" "$GCOVDIR"
mkdir -p "$OBJDIR" "$GCOVDIR"

echo "coverage: building the instrumented library in $BUILD"

# 1. the instrumented library: fmt/ only.
for src in fmt/*.c; do
    base=$(basename "$src" .c)
    $CC $CFLAGS --coverage $INCLUDES -c "$src" -o "$OBJDIR/$base.c.o"
done

# 2. the support code the tests need, without instrumentation.
for src in host/simdisk/*.c host/explore/*.c; do
    case "$src" in
        */main.c) continue ;;
    esac
    base=$(basename "$src" .c)
    $CC $CFLAGS $INCLUDES -c "$src" -o "$OBJDIR/$base.c.o"
done

# 3. every C test suite that compiles in the current tree.
SUITES=""
for src in tests/test_*.c; do
    base=$(basename "$src" .c)
    if $CC $CFLAGS --coverage $INCLUDES "$src" tests/ambre_test_util.c \
           "$OBJDIR"/*.c.o -o "$BUILD/$base" 2> "$BUILD/$base.build.log"; then
        SUITES="$SUITES $base"
    else
        echo "coverage: SKIPPED $src (does not compile)" >&2
        grep -m3 "error:" "$BUILD/$base.build.log" >&2 || true
    fi
done

# 4. run every built suite: the counters are what gcov reads.
echo "coverage: running the test suites:$SUITES"
fails=0
for base in $SUITES; do
    if ! "$BUILD/$base" > "$BUILD/$base.log" 2>&1; then
        echo "coverage: FAILED $base (see $BUILD/$base.log)" >&2
        fails=$((fails + 1))
    fi
done
if [ "$fails" -ne 0 ]; then
    echo "coverage: $fails suite(s) failed, the coverage would be meaningless" >&2
    exit 1
fi

# 5. annotate: one .gcov file per instrumented source. The list comes from the
#    sources themselves, so no file can be missed.
echo "coverage: running gcov"
for src in fmt/*.c; do
    base=$(basename "$src" .c)
    obj="$OBJDIR/$base.c.o"
    [ -f "$obj" ] || continue
    gcov -o "$obj" "$src" > "$BUILD/gcov-$base.log" 2>&1 || true
    if [ -f "$base.c.gcov" ]; then
        mv "$base.c.gcov" "$GCOVDIR/"
    fi
done

# 6. report.
exec $PYTHON scripts/coverage_report.py --gcov-dir "$GCOVDIR" --source-dir fmt \
     --out-json "$JSON" --min "$COV_MIN" "$@"

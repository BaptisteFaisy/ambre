/*
 * main.c — edr-replay CLI entry point (ACQ-05).
 *
 * Frozen command line (docs/W2-PLAN.md):
 *   edr-replay --capture <sock> --scenario <file.txt> [--rate 1.0 | --fast]
 *              [--kill-after-ms N]
 *
 * Exit codes: 0 ok, 1 functional failure, 2 usage error.
 * Everything else lives in edr_replay.c so that the frame builder, the
 * histogram and the argument parser are testable without a process.
 */

#include "edr_replay.h"

int main(int argc, char **argv)
{
    return edr_replay_main(argc, argv);
}

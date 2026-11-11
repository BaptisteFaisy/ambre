/*
 * main.cpp — edr-archive process entry point.
 *
 * Command-line interface (docs/W2-PLAN.md, frozen):
 *   edr-archive --journal <sock> --out <dir> [--once]
 *
 * Standalone mode, used by the tests and by a recovery bench (it reads a raw
 * journal image instead of asking edr-journal for it):
 *   edr-archive --image <file.img> --out <dir> [--event-id N] [--list]
 *
 * Exit codes: 0 ok, 1 functional failure, 2 usage error.
 * Priority 8 (docs/ARCHITECTURE.md section 1): the archive is the low-priority
 * consumer of the journal and never competes with the freeze path (GEL-03).
 * Logs go through the portability layer, so AMBRE_LOG=<0..3> applies (OBS-01).
 */

#include "ambre_log.hpp"
#include "edr_archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

const char* kTag = "edr-archive";

void usage(const char* program)
{
    std::fprintf(stderr,
                 "Usage: %s --journal <sock> --out <dir> [--once]\n"
                 "       %s --image <file.img> --out <dir> [--event-id N] "
                 "[--list]\n",
                 program, program);
}

/* Read the value of an option; false (and a message) when it is missing. */
bool take_value(int* index, int argc, char** argv, const char* option,
                std::string* out)
{
    if (*index + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", option);
        return false;
    }
    (*index)++;
    *out = argv[*index];
    return true;
}

bool parse_args(int argc, char** argv, ambre::archive::Options* options,
                bool* list_only)
{
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--journal") {
            if (!take_value(&i, argc, argv, "--journal", &options->journal_sock)) {
                return false;
            }
        } else if (arg == "--out") {
            if (!take_value(&i, argc, argv, "--out", &options->out_dir)) {
                return false;
            }
        } else if (arg == "--image") {
            if (!take_value(&i, argc, argv, "--image", &options->image_path)) {
                return false;
            }
        } else if (arg == "--event-id") {
            if (!take_value(&i, argc, argv, "--event-id", &value)) {
                return false;
            }
            options->event_id = static_cast<uint32_t>(std::strtoul(
                value.c_str(), nullptr, 10));
        } else if (arg == "--list") {
            *list_only = true;
        } else if (arg == "--once") {
            options->once = true;
        } else {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            return false;
        }
    }
    if (options->journal_sock.empty() == options->image_path.empty()) {
        std::fprintf(stderr, "exactly one of --journal or --image is required\n");
        return false;
    }
    if (options->out_dir.empty() && !*list_only) {
        std::fprintf(stderr, "--out is required\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    ambre::archive::Options options;
    bool list_only = false;

    if (!parse_args(argc, argv, &options, &list_only)) {
        usage(argv[0]);
        return 2;
    }

    /* Priority is set before any work: the archive must never outrank the
     * journal on the freeze path. A refusal is reported, never faked. */
    if (ambre_port_set_priority(ambre::archive::kPriorityArchive) !=
        AMBRE_PORT_OK) {
        log_warn(kTag, "priority %d not granted (reported, not faked)",
                 ambre::archive::kPriorityArchive);
    }

    if (list_only) {
        return ambre::archive::run_standalone(options.image_path,
                                              options.out_dir,
                                              options.event_id, true);
    }
    return ambre::archive::run(options);
}

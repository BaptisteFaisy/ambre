/*
 * main.c — edrctl CLI entry point (EXP-01).
 *
 * Frozen command line (docs/W2-PLAN.md):
 *   edrctl --journal <sock> {status|list|export <id> --out <f.mcap>|verify}
 *          [--json]
 *
 * Exit codes: 0 ok, 1 functional failure, 2 usage error.
 * All the logic lives in edr_ctl.c / edr_ctl_cmds.c so that it is testable
 * without spawning a process.
 */

#include "edr_ctl.h"

#include <stdio.h>
#include <string.h>

int edrctl_main(int argc, char **argv)
{
    ctl_args_t args;
    ctl_link_t link;
    int rc;

    rc = ctl_parse_args(argc, argv, &args);
    if (rc > 0) {
        return 0;                        /* --help printed, success */
    }
    if (rc < 0) {
        ctl_print_usage(argv[0], stderr);
        return 2;
    }
    memset(&link, 0, sizeof(link));
    rc = ctl_link_open(args.journal_sock, &link);
    if (rc != AMBRE_PORT_OK) {
        fprintf(stderr, "edrctl: cannot reach the journal service on %s (%d)\n",
                args.journal_sock, rc);
        return 1;
    }
    rc = ctl_dispatch(&link, &args, stdout);
    ctl_link_close(&link);
    return rc;
}

int main(int argc, char **argv)
{
    return edrctl_main(argc, argv);
}

/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <sys/procmgr.h> - building on the PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real QNX header from the SDP is used on target; this file exists so the
 * QNX glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirements: QNX-02 (start from the boot image and detach), QNX-06
 * and SEC-01 (drop root, keep only the abilities the driver needs).
 *
 * Provenance: QNX SDP 8.0 C Library Reference, procmgr_daemon() and
 * procmgr_ability(); ability names and PROCMGR_AID_* constants from the
 * "Abilities Reference" of the System Security Guide:
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/procmgr_ability.html
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.security.system/topic/manual/abilities_reference.html
 * The PROCMGR_AID_* values are symbolic here: the drivers only OR them into a
 * mask, and the QNX build uses the SDP's own numbers.
 */
#ifndef PGW_STUB_SYS_PROCMGR_H
#define PGW_STUB_SYS_PROCMGR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* --- daemonisation -------------------------------------------------------- */

#define PROCMGR_DAEMON_NOCHDIR      0x00000001u
#define PROCMGR_DAEMON_NOCLOSE      0x00000002u
#define PROCMGR_DAEMON_NODEVNULL    0x00000004u
#define PROCMGR_DAEMON_KEEPUMASK    0x00000008u

int procmgr_daemon(int status, unsigned flags);

/* --- abilities (QNX-06, SEC-01) ------------------------------------------- */

/* Operation codes of a procmgr_ability() request (real header, SDP 8.0:
 * the operation and the domain are bits of the SAME word as the ability id). */
#define PROCMGR_AOP_ALLOW    0x00020000u
#define PROCMGR_AOP_DENY     0x00010000u
#define PROCMGR_AOP_SUBRANGE 0x00040000u

/* Which identity an ability request applies to. QNX names them
 * PROCMGR_ADN_ROOT / PROCMGR_ADN_NONROOT (ADN = "ability domain number"). */
#define PROCMGR_ADN_ROOT    0x10000000u
#define PROCMGR_ADN_NONROOT 0x20000000u

/* Ability class / inheritance / locking flags. */
#define PROCMGR_AID_INHERIT  0x00000001u
#define PROCMGR_AID_LOCK     0x00000002u
#define PROCMGR_AID_UNLOCK   0x00000004u
#define PROCMGR_AID_DROP     0x00000008u

/* The static abilities the drivers use, by name. Values are symbolic on the PC;
 * see the Abilities Reference for the meaning of each one. */
#define PROCMGR_AID_MEM_PHYS       1u   /* map device / DMA memory */
#define PROCMGR_AID_INTERRUPT      2u   /* InterruptAttachThread/Event (VIO-04) */
#define PROCMGR_AID_PATHSPACE      3u   /* resmgr_attach() (QNX-01) */
#define PROCMGR_AID_PUBLIC_CHANNEL 44u  /* name_attach() public channel */
/* End-of-list marker of a procmgr_ability() request (real header: the ability
 * mask, 0xffff: the identifier lives in the low 16 bits, the operation in the
 * next byte, the domains in the two bytes above). */
#define PROCMGR_AID_MASK       0xffffu
#define PROCMGR_AID_EOL        PROCMGR_AID_MASK
#define PROCMGR_AID_IO             5u   /* ThreadCtl(_NTO_TCTL_IO) */
#define PROCMGR_AID_RLIMIT         6u   /* setrlimit() above the soft limit */
#define PROCMGR_AID_TRACE          7u   /* kernel trace control (QNX-09) */

int procmgr_ability(pid_t pid, unsigned flags, ...);

#endif /* PGW_STUB_SYS_PROCMGR_H */

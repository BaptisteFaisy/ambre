/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <sys/dcmd_blk.h> - building on the PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real QNX header from the SDP is used on target; this file exists so the
 * QNX glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirements: FS-10 (`df` reports the shared folder's free space),
 * SEC-01 (mount flags reported to the process manager, which ignores setuid
 * bits when the filesystem is mounted nosuid).
 *
 * Provenance: QNX SDP 8.0 C Library Reference, devctl()/statvfs() and the
 * "Devctl and Ioctl Commands" reference. The command numbers are symbolic on
 * the PC: the drivers pass them to a switch, they never depend on the value.
 */
#ifndef PGW_STUB_SYS_DCMD_BLK_H
#define PGW_STUB_SYS_DCMD_BLK_H

#include <sys/iomsg.h>

/* devctl() commands answered by a filesystem resource manager. */
#define DCMD_FSYS_STATVFS    0x0000d001
#define DCMD_FSYS_MOUNTED_ON 0x0000d002
#define DCMD_FSYS_MOUNTED_AT 0x0000d003
#define DCMD_FSYS_DEVICE     0x0000d004
#define DCMD_FSYS_READY      0x0000d005

/* Asked by the process manager before it executes a file from a filesystem. */
#define DCMD_ALL_GETMOUNTFLAGS 0x0000d100

/* Added for the Ambre EDR (essay E1, STO-01): cache flush of a block device.
 * io-blk answers devctl(fd, DCMD_FLUSH_CACHE, ...) on /dev/hd0tNNN.
 * Provenance: QNX SDP 8.0, io-blk chapter of the Utilities Reference,
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.utilities/topic/i/io-blk.html
 *   and the "Devctl and Ioctl Commands" reference of the C Library.
 * The value below is SYMBOLIC on the PC (the glue only passes it to devctl(),
 * it never decodes it); the real number comes from the SDP's own
 * <sys/dcmd_blk.h> and must be recorded in docs/QNX-ABI.md at the first
 * target build. */
#define DCMD_FLUSH_CACHE     0x0000d200
#define DCMD_BLK_FLUSHCACHE  DCMD_FLUSH_CACHE

#endif /* PGW_STUB_SYS_DCMD_BLK_H */

/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <devctl.h> - building on the PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real QNX header from the SDP is used on target; this file exists so the
 * QNX glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirement: STO-01 / essay E1 (the journal sends DCMD_FLUSH_CACHE
 * to the block device with devctl()).
 *
 * Provenance: QNX SDP 8.0 C Library Reference, devctl():
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/devctl.html
 * The prototype below is the one documented on that page; it returns EOK (0)
 * on success or a positive errno-style status.
 */
#ifndef PGW_STUB_DEVCTL_H
#define PGW_STUB_DEVCTL_H

#include <stddef.h>
#include <sys/uio.h>   /* struct iovec, pour devctlv() */

int devctl(int fd, int dcmd, void *dev_data_ptr, size_t n_bytes,
           int *dev_info_ptr);
int devctlv(int fd, int dcmd, int sparts, int rparts, const struct iovec *sv,
            const struct iovec *rv, int *dev_info_ptr);

/* --------------------------------------------------------------------------
 * Encoding of a devctl command — mirrored from the SDP 8.0 header
 * ($QNX_TARGET/usr/include/devctl.h, lines 45-135), verbatim in shape and
 * meaning. The stub exists so the QNX glue compiles on the PC; a command the
 * glue builds with these macros must be built the SAME way on both sides, or
 * the PC build would not show what the target does.
 *
 * A devctl code carries a DIRECTION in its two top bits and a SIZE in bits
 * 16-31, and the kernel reads both out of the code — a command built by hand
 * as a bare number encodes a size of 0, so nothing is copied back to the
 * client. That is not a theory here: it is what the target measured on
 * 2026-11-14 (docs/QNX-ABI.md §3, `devctl ... rc=25 errno=0 code=1281 size=68`).
 * -------------------------------------------------------------------------- */

#define _POSIX_DEVDIR_NONE      0
#define _POSIX_DEVDIR_TO        0x80000000
#define _POSIX_DEVDIR_FROM      0x40000000
#define _POSIX_DEVDIR_TOFROM    (_POSIX_DEVDIR_TO | _POSIX_DEVDIR_FROM)
#define _POSIX_DEVDIR_CMD_MASK  0x0000FFFF

#define DEVDIR_NONE             _POSIX_DEVDIR_NONE
#define DEVDIR_TO               _POSIX_DEVDIR_TO
#define DEVDIR_FROM             _POSIX_DEVDIR_FROM
#define DEVDIR_TOFROM           _POSIX_DEVDIR_TOFROM
#define DEVDIR_CMD_MASK         _POSIX_DEVDIR_CMD_MASK

/* Classes (devctl.h:101-127). _DCMD_MISC is the class QNX recommends for a
 * devctl that is not a QNX driver — which is exactly Ambre's case. */
#define _DCMD_ALL       0x01
#define _DCMD_FSYS      0x02
#define _DCMD_BLK       _DCMD_FSYS
#define _DCMD_CHR       0x03
#define _DCMD_NET       0x04
#define _DCMD_MISC      0x05
#define _DCMD_IP        0x06
#define _DCMD_MIXER     0x07
#define _DCMD_PROC      0x08
#define _DCMD_MEM       0x09
#define _DCMD_INPUT     0x0A
#define _DCMD_PHOTON    0x0B
#define _DCMD_CAM       0x0C
#define _DCMD_USB       0x0D
#define _DCMD_MEDIA     0x0E
#define _DCMD_CAM_SIM   0x0F
#define _DCMD_MEMCLASS  0x10
#define _DCMD_PARTITION 0x11
#define _DCMD_FSEVMGR   0x12

#define __DIO_RAW(__class, __cmd, __size, __dir) \
    (int)(((unsigned)(__size) << 16) + ((unsigned)(__class) << 8) \
          + (unsigned)(__cmd) + (unsigned)(__dir))
#define __DIOF(class, cmd, data)  (int)((unsigned)(sizeof(data) << 16) \
    + ((unsigned)(class) << 8) + (unsigned)(cmd) + (unsigned)(_POSIX_DEVDIR_FROM))
#define __DIOT(class, cmd, data)  (int)((unsigned)(sizeof(data) << 16) \
    + ((unsigned)(class) << 8) + (unsigned)(cmd) + (unsigned)(_POSIX_DEVDIR_TO))
#define __DIOTF(class, cmd, data) (int)((unsigned)(sizeof(data) << 16) \
    + ((unsigned)(class) << 8) + (unsigned)(cmd) \
    + (unsigned)(_POSIX_DEVDIR_TOFROM))
#define __DION(class, cmd)        (int)(((unsigned)(class) << 8) \
    + (unsigned)(cmd) + (unsigned)(_POSIX_DEVDIR_NONE))

#define get_device_command(_cmd)      ((unsigned)(_cmd) & DEVDIR_CMD_MASK)
#define get_device_size(_cmd)         (((unsigned)(_cmd) & ~DEVDIR_TOFROM) >> 16)
#define get_device_direction(_cmd)    ((unsigned)(_cmd) & DEVDIR_TOFROM)
#define set_device_direction(_cmd, _dir)  ((unsigned)(_cmd) | (_dir))

#endif /* PGW_STUB_DEVCTL_H */

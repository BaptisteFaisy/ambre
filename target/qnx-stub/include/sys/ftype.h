/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <sys/ftype.h> - building on the PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real QNX header from the SDP is used on target; this file exists so the
 * QNX glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirements: QNX-01 (attach for _FTYPE_ANY, which is what a
 * filesystem resource manager needs), QNX-07 (_FTYPE_MOUNT).
 *
 * Provenance: QNX SDP 8.0 C Library Reference, `_io_connect` (member
 * `file_type`) and resmgr_attach(). Values are symbolic on the PC.
 */
#ifndef PGW_STUB_SYS_FTYPE_H
#define PGW_STUB_SYS_FTYPE_H

#define _FTYPE_ANY     0x0000
#define _FTYPE_LINK    0x0100
#define _FTYPE_MOUNT   0x0200
#define _FTYPE_MQ      0x0300
#define _FTYPE_MQUEUE  0x0400
#define _FTYPE_PIPE    0x0500
#define _FTYPE_SEM     0x0600
#define _FTYPE_SHMEM   0x0700
#define _FTYPE_SOCKET  0x0800
#define _FTYPE_SYMLINK 0x0900

#endif /* PGW_STUB_SYS_FTYPE_H */

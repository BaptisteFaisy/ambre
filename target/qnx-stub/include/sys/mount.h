/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <sys/mount.h> - building on the PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real QNX header from the SDP is used on target; this file exists so the
 * QNX glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirements: SEC-01 (mount flags nosuid/ro/noexec reported to the
 * process manager), QNX-07 (mount/umount), FS-10 (statvfs flags).
 *
 * Provenance: QNX SDP 8.0 C Library Reference, mount()/statvfs(); the
 * `_MOUNT_*` names are the ones the iofunc layer copies into
 * iofunc_mount_t.flags and reports for DCMD_ALL_GETMOUNTFLAGS. Values are
 * symbolic on the PC (the drivers OR them into a word, never compare).
 */
#ifndef PGW_STUB_SYS_MOUNT_H
#define PGW_STUB_SYS_MOUNT_H

#define _MOUNT_READONLY 0x00000001u
#define _MOUNT_NOSUID   0x00000002u
#define _MOUNT_NOEXEC   0x00000004u
#define _MOUNT_NODEV    0x00000008u
#define _MOUNT_NOATIME  0x00000010u

#define _MFLAG_READONLY _MOUNT_READONLY
#define _MFLAG_NOSUID   _MOUNT_NOSUID
#define _MFLAG_NOEXEC   _MOUNT_NOEXEC

int mount(const char *special_device, const char *mount_directory,
          int flags, const char *mount_type, const void *mount_data,
          int data_len);
int umount(const char *mount_directory);

#endif /* PGW_STUB_SYS_MOUNT_H */

/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <sys/iofunc.h> - building on the PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real QNX header from the SDP is used on target; this file exists so the
 * QNX glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirements: QNX-01 (POSIX-layer data structures of a filesystem
 * resource manager), FS-02 (attributes), FS-09 / SEC-01 (permission checks
 * against the caller's identity), FS-10 (sync and statvfs), QNX-06 (mount flags
 * that carry nosuid/ro/noexec).
 *
 * Provenance: QNX SDP 8.0 Writing a Resource Manager, "POSIX-Layer Data
 * Structures" and the iofunc_*() entries of the C Library Reference:
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.resmgr/topic/structures.html
 * The `IOFUNC_ATTR_*` and `IOFUNC_PC_*` bit values are symbolic on the PC (they
 * are only OR-ed into a flags word); the mount flags are asserted on QNX by
 * drivers/tests/qnx_abi_assert.c.
 */
#ifndef PGW_STUB_SYS_IOFUNC_H
#define PGW_STUB_SYS_IOFUNC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* --- attribute flags ------------------------------------------------------ */

#define IOFUNC_ATTR_MTIME       0x00000001u
#define IOFUNC_ATTR_ATIME       0x00000002u
#define IOFUNC_ATTR_CTIME       0x00000004u
#define IOFUNC_ATTR_DIRTY_TIME  0x00000008u
#define IOFUNC_ATTR_DIRTY_MASK  0x0000000fu
#define IOFUNC_ATTR_INUSE       0x00000010u
#define IOFUNC_ATTR_KNOWN_INIT  0x00000020u
#define IOFUNC_ATTR_DIRTY_NLINK 0x00000100u
#define IOFUNC_ATTR_DIRTY_SIZE  0x00000200u
#define IOFUNC_ATTR_DIRTY_MODE  0x00000400u
#define IOFUNC_ATTR_DIRTY_OWNER 0x00000800u
#define IOFUNC_ATTR_DIRTY_RDEV  0x00001000u
#define IOFUNC_ATTR_DIRTY_MOUNT 0x00002000u
#define IOFUNC_ATTR_ATIME_SET   0x00010000u
#define IOFUNC_ATTR_MTIME_SET   0x00020000u
#define IOFUNC_ATTR_OWNED       0x00100000u
#define IOFUNC_ATTR_READONLY    0x00200000u

/* --- mount configuration (iofunc_mount_t.conf) --------------------------- */

#define IOFUNC_PC_SYMLINK          0x0001u
#define IOFUNC_PC_CHOWN_RESTRICTED 0x0002u
#define IOFUNC_PC_NO_TRUNC         0x0004u
#define IOFUNC_PC_SYNC_IO          0x0008u
#define IOFUNC_PC_LINK_DIR         0x0010u

/* --- structures ----------------------------------------------------------- */

/* On target these two macros are how a resource manager picks the attribute and
 * OCB types used by the whole iofunc layer. */
#ifndef IOFUNC_ATTR_T
#define IOFUNC_ATTR_T struct _iofunc_attr
#endif
#ifndef IOFUNC_OCB_T
#define IOFUNC_OCB_T struct _iofunc_ocb
#endif

typedef struct _iofunc_mount {
    uint32_t conf;      /* IOFUNC_PC_* */
    uint32_t flags;     /* _MOUNT_* of <sys/mount.h>, reported by devctl */
    uint32_t blocksize; /* st_blksize */
    struct _iofunc_funcs *funcs;
    void *dev;
} iofunc_mount_t;

typedef struct _iofunc_attr {
    uint32_t flags; /* IOFUNC_ATTR_* */
    uint32_t reserved1;
    uint32_t count;      /* OCBs bound to this attribute */
    uint32_t rdev;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t inode;
    int64_t nbytes;
    time_t atime;
    unsigned atime_ns;
    time_t mtime;
    unsigned mtime_ns;
    time_t ctime;
    unsigned ctime_ns;
    iofunc_mount_t *mount;
    sync_t lock;
    void *reserved2[2];
} iofunc_attr_t;

typedef struct _iofunc_ocb {
    iofunc_attr_t *attr; /* must be first for the iofunc defaults */
    int32_t ioflag;      /* O_* / _IO_FLAG_* the file was opened with */
    off_t offset;
    uint32_t reserved;
    struct _iofunc_ocb *next;
    uint32_t reserved2[2];
} iofunc_ocb_t;

typedef struct _iofunc_funcs {
    unsigned nfuncs;
    IOFUNC_OCB_T *(*ocb_calloc)(resmgr_context_t *ctp, IOFUNC_ATTR_T *attr);
    void (*ocb_free)(IOFUNC_OCB_T *ocb);
} iofunc_funcs_t;

/* The caller's identity, as the iofunc layer reports it (FS-09, SEC-01). */
typedef struct _client_info {
    uint32_t info_size;
    uint32_t flags;
    struct _cred_info cred;
} _client_info;

/* --- iofunc layer --------------------------------------------------------- */

void iofunc_func_init(unsigned nconnect, resmgr_connect_funcs_t *connect,
                      unsigned nio, resmgr_io_funcs_t *io);
void iofunc_attr_init(iofunc_attr_t *attr, mode_t mode, iofunc_attr_t *dattr,
                      _client_info *info);
int iofunc_attr_lock(iofunc_attr_t *attr);
int iofunc_attr_unlock(iofunc_attr_t *attr);
int iofunc_ocb_attach(resmgr_context_t *ctp, io_open_t *msg,
                      iofunc_ocb_t *ocb, iofunc_attr_t *attr,
                      const resmgr_io_funcs_t *io_funcs);
int iofunc_ocb_detach(resmgr_context_t *ctp, iofunc_ocb_t *ocb);

int iofunc_open(resmgr_context_t *ctp, io_open_t *msg, iofunc_attr_t *attr,
                iofunc_attr_t *dattr, _client_info *info);
int iofunc_open_default(resmgr_context_t *ctp, io_open_t *msg,
                        iofunc_attr_t *attr, void *extra);
/* NOTE (2026-11-12, corrected against the real SDP 8.0 headers): the
 * iofunc_* helpers below take the FIXED iofunc_ocb_t *, not the project's
 * IOFUNC_OCB_T. Only the resmgr handler prototypes (RESMGR_OCB_T) and
 * ocb_calloc/ocb_free are parameterised by the custom OCB type; a resource
 * manager with a custom OCB therefore casts when it calls a helper. */
int iofunc_close_ocb_default(resmgr_context_t *ctp, void *reserved,
                             iofunc_ocb_t *ocb);
int iofunc_read_verify(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb,
                       int *nonblock);
int iofunc_write_verify(resmgr_context_t *ctp, io_write_t *msg, iofunc_ocb_t *ocb,
                        int *nonblock);
int iofunc_stat_default(resmgr_context_t *ctp, io_stat_t *msg, iofunc_ocb_t *ocb);
int iofunc_lseek_default(resmgr_context_t *ctp, io_lseek_t *msg, iofunc_ocb_t *ocb);
int iofunc_devctl_default(resmgr_context_t *ctp, io_devctl_t *msg, iofunc_ocb_t *ocb);
int iofunc_sync_verify(resmgr_context_t *ctp, io_sync_t *msg, iofunc_ocb_t *ocb);
int iofunc_space_verify(resmgr_context_t *ctp, io_space_t *msg, iofunc_ocb_t *ocb,
                        int *nonblock);
int iofunc_chmod(resmgr_context_t *ctp, io_chmod_t *msg, iofunc_ocb_t *ocb,
                 iofunc_attr_t *attr);
int iofunc_chown(resmgr_context_t *ctp, io_chown_t *msg, iofunc_ocb_t *ocb,
                 iofunc_attr_t *attr);
int iofunc_utime(resmgr_context_t *ctp, io_utime_t *msg, iofunc_ocb_t *ocb,
                 iofunc_attr_t *attr);
int iofunc_time_update(iofunc_attr_t *attr);

/* FS-09 / SEC-01: may this client use `attr` with the given access bits? */
int iofunc_check_access(resmgr_context_t *ctp, iofunc_attr_t *attr,
                        uint32_t access, const _client_info *info);
int iofunc_client_info_ext(resmgr_context_t *ctp, int ioflag, _client_info **info,
                           unsigned flags);
int iofunc_client_info_ext_free(_client_info **info);

#define IOFUNC_CLIENTINFO_GETGROUPS 0x0001u

/* FS-09 / SEC-01: the caller's identity, as the resource manager sees it.
 * Used by the Ambre journal glue to type a client before answering a devctl
 * (the kernel's typed policy of image/ambre.sec is the first gate, this is
 * the second).
 * Provenance: QNX SDP 8.0 C Library Reference, iofunc_client_info():
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/iofunc_client_info.html */
int iofunc_client_info(resmgr_context_t *ctp, int ioflag,
                       struct _client_info *info);

#endif /* PGW_STUB_SYS_IOFUNC_H */

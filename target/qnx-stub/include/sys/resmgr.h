/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <sys/resmgr.h> - building on the PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real QNX header from the SDP is used on target; this file exists so the
 * QNX glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirements: QNX-01 (attach a resource manager over a whole subtree),
 * QNX-05 (client unblock requests), QNX-07 (detach on umount).
 *
 * Provenance: QNX SDP 8.0 C Library Reference, resmgr_attach() and the
 * "Writing a Resource Manager" guide:
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/r/resmgr_attach.html
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.resmgr/topic/fleshing_out_Connect_messages.html
 * The handler signatures below follow the `resmgr_connect_funcs_t` and
 * `resmgr_io_funcs_t` layouts documented on that second page. The `_RESMGR_*`
 * status encodings are symbolic on the PC: the drivers return them, they never
 * inspect them, so no numeric dependency exists.
 */
#ifndef PGW_STUB_SYS_RESMGR_H
#define PGW_STUB_SYS_RESMGR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>

struct _iofunc_attr;
struct _iofunc_ocb;

/* The iofunc layer defines the OCB type; the resource manager library uses the
 * same one (on target this is `#define RESMGR_OCB_T IOFUNC_OCB_T` when the
 * project does not override it). */
#ifndef RESMGR_OCB_T
#define RESMGR_OCB_T struct _iofunc_ocb
#endif
#ifndef RESMGR_HANDLE_T
#define RESMGR_HANDLE_T void
#endif

/* Table sizes the iofunc layer fills (names used by iofunc_func_init()). */
#define _RESMGR_CONNECT_NFUNCS 10
#define _RESMGR_IO_NFUNCS      42
#ifndef RESMGR_CONNECT_NFUNCS
#define RESMGR_CONNECT_NFUNCS _RESMGR_CONNECT_NFUNCS
#endif
#ifndef RESMGR_IO_NFUNCS
#define RESMGR_IO_NFUNCS _RESMGR_IO_NFUNCS
#endif

/* --- server-side context -------------------------------------------------- */

typedef struct _resmgr_context {
    rcvid_t rcvid;         /* the client waiting for a reply */
    struct _msg_info info; /* sender priority + credentials (QNX-03, FS-09) */
    int32_t offset;        /* scratch for the iofunc layer */
    uint32_t msg_max_size; /* size of the receive buffer */
    uint32_t flags;
} resmgr_context_t;

/* A resource manager's thread pool carries resmgr_context_t. The real
 * sys/resmgr.h (SDP 8.0, lines 247-249) defines this before sys/dispatch.h
 * declares thread_pool_attr_t; the stub mirrors it so that a TU which includes
 * <sys/resmgr.h> before <sys/dispatch.h> — the order the real headers end up
 * in through sys/dcmd_blk.h — typechecks the same way here. */
#ifndef THREAD_POOL_PARAM_T
# define THREAD_POOL_PARAM_T resmgr_context_t
#endif

typedef struct _resmgr_attr {
    uint32_t flags;
    uint32_t nparts_max;
    uint32_t msg_max_size;
    uint32_t other[4];
} resmgr_attr_t;

/* --- handler tables ------------------------------------------------------- */

typedef int (*resmgr_open_fn)(resmgr_context_t *ctp, io_open_t *msg,
                              RESMGR_HANDLE_T *handle, void *extra);
typedef int (*resmgr_unlink_fn)(resmgr_context_t *ctp, io_unlink_t *msg,
                                RESMGR_HANDLE_T *handle, void *reserved);
typedef int (*resmgr_rename_fn)(resmgr_context_t *ctp, io_rename_t *msg,
                                RESMGR_HANDLE_T *handle, io_rename_extra_t *extra);
typedef int (*resmgr_mknod_fn)(resmgr_context_t *ctp, io_mknod_t *msg,
                               RESMGR_HANDLE_T *handle, void *reserved);
typedef int (*resmgr_readlink_fn)(resmgr_context_t *ctp, io_readlink_t *msg,
                                  RESMGR_HANDLE_T *handle, void *reserved);
typedef int (*resmgr_link_fn)(resmgr_context_t *ctp, io_link_t *msg,
                              RESMGR_HANDLE_T *handle, io_link_extra_t *extra);
typedef int (*resmgr_mount_fn)(resmgr_context_t *ctp, io_mount_t *msg,
                               RESMGR_HANDLE_T *handle, io_mount_extra_t *extra);

typedef struct _resmgr_connect_funcs {
    unsigned nfuncs;
    resmgr_open_fn open;
    resmgr_unlink_fn unlink;
    resmgr_rename_fn rename;
    resmgr_mknod_fn mknod;
    resmgr_readlink_fn readlink;
    resmgr_link_fn link;
    resmgr_mount_fn mount;
    void *reserved[RESMGR_CONNECT_NFUNCS];
} resmgr_connect_funcs_t;

/* Every io handler has the message type it is handed as its second argument;
 * the driver's handler signature must match exactly (the QNX header declares
 * them the same way, which is why a wrong handler type is a compile error). */
typedef int (*resmgr_io_read_fn)(resmgr_context_t *ctp, io_read_t *msg,
                                 RESMGR_OCB_T *ocb);
typedef int (*resmgr_io_write_fn)(resmgr_context_t *ctp, io_write_t *msg,
                                  RESMGR_OCB_T *ocb);
typedef int (*resmgr_io_close_ocb_fn)(resmgr_context_t *ctp, void *reserved,
                                      RESMGR_OCB_T *ocb);
typedef int (*resmgr_io_stat_fn)(resmgr_context_t *ctp, io_stat_t *msg,
                                 RESMGR_OCB_T *ocb);
typedef int (*resmgr_io_devctl_fn)(resmgr_context_t *ctp, io_devctl_t *msg,
                                   RESMGR_OCB_T *ocb);
typedef int (*resmgr_io_lseek_fn)(resmgr_context_t *ctp, io_lseek_t *msg,
                                  RESMGR_OCB_T *ocb);
typedef int (*resmgr_io_sync_fn)(resmgr_context_t *ctp, io_sync_t *msg,
                                 RESMGR_OCB_T *ocb);
typedef int (*resmgr_io_chmod_fn)(resmgr_context_t *ctp, io_chmod_t *msg,
                                  RESMGR_OCB_T *ocb);
typedef int (*resmgr_io_chown_fn)(resmgr_context_t *ctp, io_chown_t *msg,
                                  RESMGR_OCB_T *ocb);
typedef int (*resmgr_io_utime_fn)(resmgr_context_t *ctp, io_utime_t *msg,
                                  RESMGR_OCB_T *ocb);
typedef int (*resmgr_io_space_fn)(resmgr_context_t *ctp, io_space_t *msg,
                                  RESMGR_OCB_T *ocb);
typedef int (*resmgr_io_close_fn)(resmgr_context_t *ctp, io_close_t *msg,
                                  RESMGR_OCB_T *ocb);
typedef int (*resmgr_io_fn)(resmgr_context_t *ctp, void *msg, RESMGR_OCB_T *ocb);

typedef struct _resmgr_io_funcs {
    unsigned nfuncs;
    resmgr_io_read_fn read;
    resmgr_io_write_fn write;
    resmgr_io_close_ocb_fn close_ocb;
    resmgr_io_stat_fn stat;
    resmgr_io_fn notify;
    resmgr_io_devctl_fn devctl;
    resmgr_io_fn unblock;
    resmgr_io_fn fcntl;
    resmgr_io_chmod_fn chmod;
    resmgr_io_chown_fn chown;
    resmgr_io_utime_fn utime;
    resmgr_io_fn open;
    resmgr_io_space_fn space;
    resmgr_io_close_fn close;
    resmgr_io_lseek_fn lseek;
    resmgr_io_fn mmap;
    resmgr_io_fn msg;
    resmgr_io_fn rmdir;
    resmgr_io_sync_fn sync;
    resmgr_io_fn dup;
    resmgr_io_read_fn read64;
    resmgr_io_write_fn write64;
    resmgr_io_utime_fn utime64;
    void *reserved[RESMGR_IO_NFUNCS];
} resmgr_io_funcs_t;

/* --- attach / detach (QNX-01, QNX-07) ------------------------------------- */

/* _RESMGR_FLAG_DIR: the manager serves every path at or below the mount point,
 * which is what a filesystem needs (QNX-01). */
#define _RESMGR_FLAG_DIR          0x00000001u
#define _RESMGR_FLAG_BEFORE       0x00000002u
#define _RESMGR_FLAG_AFTER        0x00000004u
#define _RESMGR_DETACH_PATHNAME   0x00000001u
#define _RESMGR_DETACH_CLOSE      0x00000002u

struct _dispatch;
int resmgr_attach(struct _dispatch *dpp, resmgr_attr_t *attr, const char *path,
                  unsigned flags, unsigned ftype,
                  const resmgr_connect_funcs_t *connect_funcs,
                  const resmgr_io_funcs_t *io_funcs, RESMGR_HANDLE_T *handle);
int resmgr_detach(struct _dispatch *dpp, int id, unsigned flags);

/* --- talking to the client's message -------------------------------------- */

ssize_t resmgr_msgread(resmgr_context_t *ctp, void *msg, size_t bytes, size_t offset);
int resmgr_msgwrite(resmgr_context_t *ctp, const void *msg, size_t bytes, size_t offset);
ssize_t resmgr_msgreadv(resmgr_context_t *ctp, void *iov, int n, size_t offset);
int resmgr_msgwritev(resmgr_context_t *ctp, const void *iov, int n, size_t offset);
int resmgr_open_bind(resmgr_context_t *ctp, void *ocb, void *reserved);

/* --- reply status encodings (symbolic) ------------------------------------ */

#define _RESMGR_NOREPLY (-1)
/* Added for the Ambre journal glue: iofunc_*_default() returns
 * _RESMGR_DEFAULT when the message is not theirs, which is how a
 * devctl handler chains to its own switch (Writing a Resource Manager,
 * "Handling devctl messages"). Value symbolic on the PC: the glue
 * only compares against it.
 * Provenance: QNX SDP 8.0 C Library Reference, <sys/resmgr.h>:
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/r/resmgr_attach.html */
#define _RESMGR_DEFAULT (-2)
#define _RESMGR_NPARTS(n) ((int)((unsigned)(n) << 16))

/* Reply from a pointer: on target this arranges one IOV part. */
static inline int pgw_stub_resmgr_ptr(resmgr_context_t *ctp, const void *p,
                                      size_t bytes)
{
    (void)ctp;
    (void)p;
    (void)bytes;
    return _RESMGR_NPARTS(1);
}

#define _RESMGR_PTR(ctp, p, n) pgw_stub_resmgr_ptr((ctp), (p), (n))

#endif /* PGW_STUB_SYS_RESMGR_H */

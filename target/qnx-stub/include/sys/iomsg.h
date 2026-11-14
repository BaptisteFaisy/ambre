/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <sys/iomsg.h> - building on the PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real QNX header from the SDP is used on target; this file exists so the
 * QNX glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirements: QNX-01 (resource manager message layer), QNX-07
 * (mount/umount messages), FS-09 (open flags and file modes), FS-10 (statvfs).
 *
 * Provenance: QNX SDP 8.0 C Library Reference, `_io_connect` (structure of a
 * resource manager's connect message):
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/_io_connect.html
 * The `_io_connect` layout below is copied field by field from that page.
 * Message type numbers, extended-type numbers and `eflag` bits are *symbolic*
 * here: the drivers never compare them numerically, they only pass them to the
 * resource manager library, so their values are not part of the project's ABI
 * (see docs/qnx-abi.md). They are asserted against the real header on QNX by
 * drivers/tests/qnx_abi_assert.c.
 */
#ifndef PGW_STUB_SYS_IOMSG_H
#define PGW_STUB_SYS_IOMSG_H

#include <stddef.h>
#include <stdint.h>
#include <sys/ftype.h>

/* --- I/O message types (symbolic on the PC) ------------------------------- */

#define _IO_CONNECT 0x01
#define _IO_READ    0x03
#define _IO_WRITE   0x04
#define _IO_CLOSE   0x05
#define _IO_STAT    0x06
#define _IO_NOTIFY  0x07
#define _IO_MSG     0x09
#define _IO_DEVCTL  0x0a
#define _IO_UNBLOCK 0x0c
#define _IO_CHMOD   0x10
#define _IO_CHOWN   0x11
#define _IO_UTIME   0x12
#define _IO_READLINK 0x13
#define _IO_MMAP    0x14
#define _IO_SYNC    0x16
#define _IO_LSEEK   0x17
#define _IO_RENAME  0x18
#define _IO_UNLINK  0x19
#define _IO_MKNOD   0x1a
#define _IO_LINK    0x1b
#define _IO_SPACE   0x1d

/* --- status values of a connect reply ------------------------------------ */

/* An open can be answered with "the path is a link, restart there": the status
 * carries _IO_CONNECT_RET_FLAG and _IO_CONNECT_RET_LINK and the reply is a
 * _io_connect_link_reply followed by the target path (spec 5.3, FS-08).
 * Symbolic on the PC: the value only ever travels back to the client. */
#define _IO_CONNECT_RET_FLAG 0x0001
#define _IO_CONNECT_RET_LINK 0x0002

/* --- _io_connect subtypes (symbolic) -------------------------------------- */

#define _IO_CONNECT_COMBINE       0x00
#define _IO_CONNECT_COMBINE_CLOSE 0x01
#define _IO_CONNECT_OPEN          0x02
#define _IO_CONNECT_UNLINK        0x03
#define _IO_CONNECT_RENAME        0x04
#define _IO_CONNECT_MKNOD         0x05
#define _IO_CONNECT_READLINK      0x06
#define _IO_CONNECT_LINK          0x07
#define _IO_CONNECT_RSVD_UNBLOCK  0x08
#define _IO_CONNECT_MOUNT         0x09

/* --- _io_connect.eflag bits (symbolic) ------------------------------------ */

#define _IO_CONNECT_EFLAG_DIR    0x01u
#define _IO_CONNECT_EFLAG_DOT    0x02u
#define _IO_CONNECT_EFLAG_DOTDOT 0x04u

/* --- _io_connect.extra_type (symbolic) ------------------------------------ */

#define _IO_CONNECT_EXTRA_NONE      0
#define _IO_CONNECT_EXTRA_SYMLINK   1
#define _IO_CONNECT_EXTRA_MQUEUE    2
#define _IO_CONNECT_EXTRA_SOCKET    3
#define _IO_CONNECT_EXTRA_SEM       4
#define _IO_CONNECT_EXTRA_RESMGR_LINK 5
#define _IO_CONNECT_EXTRA_PROC_SYMLINK 6
#define _IO_CONNECT_EXTRA_RENAME    7
#define _IO_CONNECT_EXTRA_MOUNT     8
#define _IO_CONNECT_EXTRA_MOUNT_OCB 9
#define _IO_CONNECT_EXTRA_TYMEM     10
#define _IO_CONNECT_EXTRA_LINK      11

/* --- ioflag access bits (documented: _io_connect page, "ioflag") ---------- */

/* QNX converts the traditional O_RDONLY/O_WRONLY/O_RDWR trio into two bits so
 * that they do not overlap any other O_* flag: O_RDONLY(0)->RD, O_WRONLY(1)->WR,
 * O_RDWR(2)->RD|WR. Source: the _io_connect() page cited above. */
#define _IO_FLAG_RD 0x01u
#define _IO_FLAG_WR 0x02u

/* --- extended types of a read/write message ------------------------------- */

#define _IO_XTYPE_NONE      0u
#define _IO_XTYPE_OFFSET    1u
#define _IO_XTYPE_READDIR   2u
#define _IO_XTYPE_MASK      0xffff0000u

/* --- message structures --------------------------------------------------- */

/* Connect message, field for field from the _io_connect() reference page. */
struct _io_connect {
    uint16_t type;
    uint16_t subtype;
    uint32_t file_type;
    uint16_t reply_max;
    uint16_t entry_max;
    uint32_t key;
    uint32_t handle;
    uint32_t ioflag;
    uint32_t mode;
    uint16_t sflag;
    uint16_t access;
    uint16_t zero;
    uint16_t path_len;
    uint8_t eflag;
    uint8_t extra_type;
    uint16_t extra_len;
    char path[1];
};

/* Reply to a connect message that redirects the client to another path
 * (a symlink met during resolution): path_len bytes follow the header. */
struct _io_connect_link_reply {
    uint32_t file_type;
    uint16_t eflag;
    uint8_t prefix_len;
    uint8_t path_len;
};

/* Open: a connect message or a link reply, same union on target. */
typedef union {
    struct _io_connect connect;
    struct _io_connect_link_reply link_reply;
} io_open_t;

/* Common header of an I/O message. `combine_len` is the size of this message
 * when it is part of a combine message, 0 when it stands alone. */
struct _io_read {
    uint16_t type;
    uint16_t combine_len;
    uint32_t nbytes;
    uint32_t xtype;
    uint32_t zero;
};

struct _io_write {
    uint16_t type;
    uint16_t combine_len;
    uint32_t nbytes;
    uint32_t xtype;
    uint32_t zero;
    /* data follows */
};

typedef union {
    struct _io_read i;
    struct _io_read o;
} io_read_t;

typedef union {
    struct _io_write i;
    struct _io_write o;
} io_write_t;

/* The pread()/pwrite() offset travels in a trailer of this shape. */
struct _xtype_offset {
    int64_t offset;
};

#define _IO_READ_GET_NBYTES(m) ((m)->i.nbytes)
#define _IO_SET_READ_NBYTES(ctp, n) ((void)(ctp), (void)(n))
#define _IO_WRITE_GET_NBYTES(m) ((m)->i.nbytes)
#define _IO_SET_WRITE_NBYTES(ctp, n) ((void)(ctp), (void)(n))

struct _io_stat {
    uint16_t type;
    uint16_t combine_len;
    uint32_t zero;
};

typedef union {
    struct _io_stat i;
    struct _io_stat o;
} io_stat_t;

struct _io_lseek {
    uint16_t type;
    uint16_t combine_len;
    uint16_t whence;
    uint16_t zero;
    uint64_t offset;
};

typedef union {
    struct _io_lseek i;
    uint64_t o;
} io_lseek_t;

struct _io_close {
    uint16_t type;
    uint16_t combine_len;
    uint32_t zero;
};

typedef union {
    struct _io_close i;
    struct _io_close o;
} io_close_t;

struct _io_devctl {
    uint16_t type;
    uint16_t combine_len;
    int32_t dcmd;
    uint32_t nbytes;
    uint32_t zero;
    /* data follows */
};

struct _io_devctl_reply {
    uint32_t zero;
    int32_t nbytes;
    uint32_t zero2;
};

typedef union {
    struct _io_devctl i;
    struct _io_devctl_reply o;
} io_devctl_t;

struct _io_chmod {
    uint16_t type;
    uint16_t combine_len;
    uint32_t mode;
    uint32_t zero;
};

typedef union {
    struct _io_chmod i;
    struct _io_chmod o;
} io_chmod_t;

struct _io_chown {
    uint16_t type;
    uint16_t combine_len;
    int32_t uid;
    int32_t gid;
    uint32_t zero;
};

typedef union {
    struct _io_chown i;
    struct _io_chown o;
} io_chown_t;

struct _io_utime {
    uint16_t type;
    uint16_t combine_len;
    uint32_t actime;
    uint32_t modtime;
    uint32_t nsec;
    uint32_t zero;
};

typedef union {
    struct _io_utime i;
    struct _io_utime o;
} io_utime_t;

struct _io_space {
    uint16_t type;
    uint16_t combine_len;
    uint16_t subtype;
    int16_t whence;
    uint64_t start;
    uint64_t len;
    uint32_t zero;
};

typedef union {
    struct _io_space i;
    uint64_t o;
} io_space_t;

struct _io_sync {
    uint16_t type;
    uint16_t combine_len;
    uint32_t flag;
    uint32_t zero;
};

typedef union {
    struct _io_sync i;
    struct _io_sync o;
} io_sync_t;

/* The mount message: the client asks the driver to mount a filesystem at a
 * path. The extra carries the path and the mount flags (QNX-07). */
struct _io_mount_extra {
    char path[1];
};

typedef struct _io_mount_extra io_mount_extra_t;

typedef union {
    struct _io_connect connect;
    struct _io_mount_extra extra;
} io_mount_t;

/* unlink/rmdir: a connect message whose `mode` says S_IFDIR for rmdir. */
typedef io_open_t io_unlink_t;
/* rename: the new path in the connect message, the old one in the extra. */
typedef io_open_t io_rename_t;
/* mknod (mkdir arrives here with S_IFDIR) and link. */
typedef io_open_t io_mknod_t;
typedef io_open_t io_link_t;
typedef io_open_t io_readlink_t;

typedef struct _io_rename_extra {
    char path[1];
} io_rename_extra_t;

typedef struct _io_link_extra {
    char path[1];
} io_link_extra_t;

/* statvfs()/fstatvfs() answer, carried by devctl(DCMD_FSYS_STATVFS). */
struct __msg_statvfs {
    uint32_t f_bsize;
    uint32_t f_frsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint64_t f_favail;
    uint32_t f_fsid;
    uint32_t f_flag;
    uint32_t f_namemax;
    char f_basetype[16];
    char f_mntonname[1];
};

/* Values of f_flag, as QNX reports them in struct __msg_statvfs. Symbolic:
 * the drivers build them from _MOUNT_* of <sys/mount.h>. */
#define ST_RDONLY 0x0001u
#define ST_NOSUID 0x0002u

/* ---------------------------------------------------------------------------
 * Payload accessors, mirrored from the SDP 8.0 ($QNX_TARGET/usr/include/
 * sys/iomsg.h:1010,1030, backed by __PAYLOAD_OF of sys/platform.h:79). For an
 * io_devctl_t the two macros return the SAME address, because `i` and `o` are
 * the two views of one 16-byte header — that is the property the devctl
 * handlers of this project rely on (a request read from the payload and an
 * answer written into it, docs/QNX-ABI.md §3).
 *
 * The stub spells the pointer arithmetic out instead of the SDP's bounded-
 * member builtin: the builtin exists to keep __builtin_object_size honest
 * under _FORTIFY_SOURCE, and the PC build of the glue does not use it. The
 * VALUE is the same expression the SDP documents ("(void *)(&(obj)->hdr_member
 * + 1)"), which is what matters for a syntax check.
 * --------------------------------------------------------------------------- */
#define __PAYLOAD_OF(obj, hdr_member)     ((void *)((char *)(obj) + sizeof((obj)->hdr_member)))
#define _IO_INPUT_PAYLOAD(msg)    __PAYLOAD_OF((msg), i)
#define _IO_OUTPUT_PAYLOAD(msg)   __PAYLOAD_OF((msg), o)

#endif /* PGW_STUB_SYS_IOMSG_H */

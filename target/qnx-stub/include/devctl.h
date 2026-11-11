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

int devctl(int fd, int dcmd, void *dev_data_ptr, size_t n_bytes,
           int *dev_info_ptr);

#endif /* PGW_STUB_DEVCTL_H */

/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <hw/pci.h> - building on the PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real QNX header from the SDP is used on target; this file exists so the
 * QNX glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirements: VIO-01, VIO-02, VIO-04, VFS-01, SEC-02.
 *
 * Note on the path: QNX 7.0 and earlier exposed the bus layer as <hw/pci.h>
 * (pci_attach(), pci_read_config*(), ...). QNX 8.0 replaces it with the PCI
 * server client library, <pci/pci.h> plus the capability plug-ins. The drivers
 * use the 8.0 API, so this header only forwards: it keeps the historical
 * `#include <hw/pci.h>` spelling working for a reader coming from the old
 * documentation, and it makes the dependency explicit in one place.
 */
#ifndef PGW_STUB_HW_PCI_H
#define PGW_STUB_HW_PCI_H

#include <pci/cap_msix.h>
#include <pci/pci.h>

#endif /* PGW_STUB_HW_PCI_H */

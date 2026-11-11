/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <pci/cap_msix.h> (QNX 8.0 PCI MSI-X capability plug-in) - PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real header from the SDP is used on target; this file exists so the QNX
 * glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirement: VIO-04 (receive interrupts through the QNX 8.0
 * interfaces, MSI-X first, falling back to the classic INTx line).
 *
 * Provenance: QNX SDP 8.0 PCI Server capability plug-ins (`pci_cap-0x11.so`
 * implements MSI-X); the names and call shapes below are the ones the drivers
 * use. The QNX build compiles against the real header.
 */
#ifndef PGW_STUB_PCI_CAP_MSIX_H
#define PGW_STUB_PCI_CAP_MSIX_H

#include <stdint.h>
#include <pci/pci.h>

/* How many interrupt vectors the capability can deliver. */
int cap_msix_get_nirq(pci_cap_t cap);
/* Reserve `nirq` vectors (must not exceed the value above). */
int cap_msix_set_nirq(pci_devhdl_t hdl, pci_cap_t cap, unsigned nirq);
/* Bind vector `entry` to the logical interrupt `irq` the kernel will see. */
int cap_msix_set_irq_entry(pci_devhdl_t hdl, pci_cap_t cap, unsigned entry,
                           unsigned irq);
int cap_msix_unmask_irq_entry(pci_devhdl_t hdl, pci_cap_t cap, unsigned entry);
int cap_msix_enable(pci_devhdl_t hdl, pci_cap_t cap);
int cap_msix_disable(pci_devhdl_t hdl, pci_cap_t cap);

#endif /* PGW_STUB_PCI_CAP_MSIX_H */

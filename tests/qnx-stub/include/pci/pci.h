/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <pci/pci.h> (the QNX 8.0 PCI server client API) - PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real header from the SDP is used on target; this file exists so the QNX
 * glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirements: VIO-01 (find the 9P device on the PCI bus), VIO-02
 * (modern capability list), VIO-04 (MSI-X with an INTx fallback), VFS-01
 * (find the virtiofs device), SEC-02 (the driver only ever touches its own
 * device).
 *
 * Provenance: QNX SDP 8.0 PCI Server, `<pci/pci.h>` and `<pci/cap_*.h>` (the
 * capability plug-ins), whose names and call shapes are the ones used here.
 * Only the subset the drivers call is declared; the QNX build compiles against
 * the real headers and is the authority on prototypes.
 */
#ifndef PGW_STUB_PCI_PCI_H
#define PGW_STUB_PCI_PCI_H

#include <stddef.h>
#include <stdint.h>

/* Device handle returned by pci_device_attach(). */
typedef void *pci_devhdl_t;
/* A BDF address packed into 32 bits: bus, device, function. */
typedef uint32_t pci_bdf_t;
/* A capability entry returned by pci_device_find_capid(). */
typedef uint32_t pci_cap_t;
/* A base address register descriptor. */
typedef struct _pci_ba {
    uint64_t BaseAddr;
    uint64_t Size;
    uint32_t Flags;
} pci_ba_t;

#define PCI_BUS(bdf) (((bdf) >> 16) & 0xffu)
#define PCI_DEV(bdf) (((bdf) >> 11) & 0x1fu)
#define PCI_FUNC(bdf) (((bdf) >> 8) & 0x07u)
#define PCI_BDF(bus, dev, func) (((uint32_t)(bus) << 16) | ((uint32_t)(dev) << 11) | \
                                 ((uint32_t)(func) << 8))

/* Class code wildcard for pci_device_find(). */
#define PCI_CCODE_ANY 0xffffffffu

/* Capability IDs (<pci/pci.h>, the PCI specification numbering). */
#define CAPID_MSI  0x05u
#define CAPID_MSIX 0x11u
#define CAPID_PCI_EXPRESS 0x10u

/* Attach flags. */
#define pci_attachFlags_DEFAULT 0x0000u

/* BAR flags. */
#define PCI_BAR_IO  0x00000001u
#define PCI_BAR_MEM 0x00000000u

/* Find the n-th device with this vendor/device/class; 0 when none is left. */
pci_bdf_t pci_device_find(unsigned index, uint16_t vid, uint16_t did,
                          uint32_t ccode);
/* Open a device so its config space and BARs can be used (SEC-02: one device). */
pci_devhdl_t pci_device_attach(pci_bdf_t bdf, unsigned flags, int32_t *err);
int pci_device_detach(pci_devhdl_t hdl);
int pci_device_read_irq(pci_devhdl_t hdl, int *nirq, int *irq);
/* Walk the capability list: find the capability whose id is `capid`. */
int pci_device_find_capid(pci_bdf_t bdf, uint8_t capid, int32_t *cap);
int pci_device_read_cap(pci_bdf_t bdf, void *cap, size_t nbytes);
int pci_device_read_ba(pci_devhdl_t hdl, unsigned ba, pci_ba_t *info, int *nba);
/* Map a BAR into the process address space; needs mem_phys (QNX-06). */
void *pci_device_map_region(pci_devhdl_t hdl, unsigned ba, unsigned mmap_flags);
int pci_device_unmap_region(pci_devhdl_t hdl, void *addr);

#endif /* PGW_STUB_PCI_PCI_H */

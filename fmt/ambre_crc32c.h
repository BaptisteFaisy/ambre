/*
 * ambre_crc32c.h - Portable CRC-32C (Castagnoli)
 * Polynomial 0x1EDC6F41, used in iSCSI, SCTP, Btrfs, and MCAP.
 */

#ifndef AMBRE_CRC32C_H
#define AMBRE_CRC32C_H

#include <stdint.h>
#include <stddef.h>

uint32_t ambre_crc32c(uint32_t crc, const void *buf, size_t len);

#endif /* AMBRE_CRC32C_H */

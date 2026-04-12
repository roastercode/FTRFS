// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTRFS — EDAC layer: CRC32 + Reed-Solomon FEC
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <linux/kernel.h>
#include <linux/crc32.h>
#include "ftrfs.h"

/*
 * ftrfs_crc32 - compute CRC32 checksum
 * @buf: data buffer
 * @len: length in bytes
 *
 * Returns CRC32 checksum. Uses kernel's hardware-accelerated CRC32
 * (same as ext4/btrfs).
 */
__u32 ftrfs_crc32(const void *buf, size_t len)
{
	return crc32_le(0xFFFFFFFF, buf, len) ^ 0xFFFFFFFF;
}

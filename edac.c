// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTRFS — EDAC layer: CRC32 + Reed-Solomon FEC
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * Reed-Solomon encoding/decoding uses the kernel's lib/reed_solomon
 * library (RS(255,239) over GF(2^8), primitive polynomial 0x187).
 * This avoids duplicating well-tested RS code already present in the
 * kernel (used by NAND MTD, DVB, etc.) and addresses the concern raised
 * during review (Eric Biggers, linux-fsdevel, April 2026).
 *
 * RS parameters:
 *   FTRFS_RS_SYMSIZE = 8         (GF(2^8))
 *   FTRFS_RS_FCR     = 0         (first consecutive root)
 *   FTRFS_RS_PRIM    = 1         (primitive element)
 *   FTRFS_RS_NROOTS  = FTRFS_RS_PARITY = 16 (parity symbols)
 *   data per subblock: FTRFS_SUBBLOCK_DATA = 239 bytes
 *   codeword length:   255 bytes (FTRFS_SUBBLOCK_TOTAL)
 */

#include <linux/kernel.h>
#include <linux/crc32.h>
#include <linux/slab.h>
#include <linux/rslib.h>
#include "ftrfs.h"

/* RS codec handle — allocated once at module init */
static struct rs_control *ftrfs_rs_ctrl;

/*
 * ftrfs_rs_init_tables — initialize the RS codec
 * Called once from ftrfs_init() before any mount.
 */
void ftrfs_rs_init_tables(void)
{
	/*
	 * init_rs(symsize, gfpoly, fcr, prim, nroots)
	 * symsize = 8      -> GF(2^8)
	 * gfpoly  = 0x187  -> primitive polynomial x^8+x^7+x^2+x+1
	 * fcr     = 0      -> first consecutive root
	 * prim    = 1      -> primitive element alpha
	 * nroots  = 16     -> 16 parity symbols, corrects up to 8 errors
	 */
	ftrfs_rs_ctrl = init_rs(8, 0x187, 0, 1, FTRFS_RS_PARITY);
	if (!ftrfs_rs_ctrl)
		pr_err("ftrfs: failed to initialize RS codec\n");
	else
		pr_debug("ftrfs: RS codec initialized (RS(%d,%d))\n",
			 FTRFS_SUBBLOCK_TOTAL, FTRFS_SUBBLOCK_DATA);
}

/*
 * ftrfs_rs_exit — release the RS codec at module exit
 */
void ftrfs_rs_exit_tables(void)
{
	if (ftrfs_rs_ctrl) {
		free_rs(ftrfs_rs_ctrl);
		ftrfs_rs_ctrl = NULL;
	}
}

/*
 * ftrfs_rs_encode — encode @len data bytes, produce FTRFS_RS_PARITY parity
 * @data:   input data (@len bytes, must be <= FTRFS_SUBBLOCK_DATA)
 * @len:    number of data bytes (FTRFS_SUBBLOCK_DATA for the bitmap path,
 *          FTRFS_INODE_RS_DATA for inodes, etc.)
 * @parity: output parity (FTRFS_RS_PARITY bytes)
 *
 * lib/reed_solomon supports shortened RS codes natively: passing a
 * length less than FTRFS_SUBBLOCK_DATA produces a valid codeword,
 * mathematically equivalent to padding the data with zeros up to the
 * full subblock length. The same length must be passed to the matching
 * ftrfs_rs_decode call.
 *
 * The kernel encode_rs8 API takes uint8_t *data directly; parity is
 * returned via a uint16_t *par buffer (low byte holds the parity symbol).
 */
int ftrfs_rs_encode(uint8_t *data, size_t len, uint8_t *parity)
{
	uint16_t par[FTRFS_RS_PARITY];
	int i;

	if (!ftrfs_rs_ctrl)
		return -EINVAL;
	if (len > FTRFS_SUBBLOCK_DATA)
		return -EINVAL;

	memset(par, 0, sizeof(par));
	encode_rs8(ftrfs_rs_ctrl, data, len, par, 0);

	for (i = 0; i < FTRFS_RS_PARITY; i++)
		parity[i] = (uint8_t)par[i];

	return 0;
}

/*
 * ftrfs_rs_decode — decode and correct a shortened RS codeword in place
 * @data:   data bytes (@len bytes), corrected in place on success
 * @len:    number of data bytes (must match the length passed to encode)
 * @parity: parity bytes (FTRFS_RS_PARITY)
 *
 * Returns:
 *   < 0  uncorrectable: -EBADMSG on RS uncorrectable (more than
 *        FTRFS_RS_PARITY/2 symbol errors), -EINVAL on bad input.
 *   = 0  no errors detected, no correction needed.
 *   > 0  number of symbol errors that were corrected in place.
 */
int ftrfs_rs_decode(uint8_t *data, size_t len, uint8_t *parity)
{
	uint16_t par[FTRFS_RS_PARITY];
	int i, nerr;

	if (!ftrfs_rs_ctrl)
		return -EINVAL;
	if (len > FTRFS_SUBBLOCK_DATA)
		return -EINVAL;

	for (i = 0; i < FTRFS_RS_PARITY; i++)
		par[i] = parity[i];

	/*
	 * decode_rs8 takes uint8_t *data and corrects in place. No
	 * temporary buffer or post-decode copy-back is needed.
	 */
	nerr = decode_rs8(ftrfs_rs_ctrl, data, par, len,
			  NULL, 0, NULL, 0, NULL);

	if (nerr < 0) {
		pr_err_ratelimited("ftrfs: RS block uncorrectable (len=%zu)\n",
				   len);
		return -EBADMSG;
	}

	return nerr;
}

/*
 * ftrfs_rs_encode_region -- encode N RS(255,239) shortened subblocks
 * across a region.
 *
 * @data_buf:      base pointer of the data region
 * @data_stride:   distance in bytes between the start of two
 *                 consecutive data subblocks (e.g.
 *                 FTRFS_SUBBLOCK_TOTAL=255 for the bitmap)
 * @parity_buf:    base pointer of the parity region (may equal
 *                 data_buf + data_len for the interleaved case)
 * @parity_stride: distance between the start of two consecutive
 *                 parity blobs (e.g. FTRFS_SUBBLOCK_TOTAL=255 for
 *                 the bitmap, FTRFS_RS_PARITY=16 for contiguous
 *                 parity placement)
 * @data_len:      number of data bytes per subblock (e.g.
 *                 FTRFS_SUBBLOCK_DATA=239 for a full subblock)
 * @n_subblocks:   number of subblocks to encode
 *
 * Returns 0 on success, negative on the first encode failure.
 * On failure the buffer is in an indeterminate state.
 */
int ftrfs_rs_encode_region(u8 *data_buf, size_t data_stride,
			   u8 *parity_buf, size_t parity_stride,
			   size_t data_len, unsigned int n_subblocks)
{
	unsigned int i;

	if (!data_buf || !parity_buf)
		return -EINVAL;

	for (i = 0; i < n_subblocks; i++) {
		u8 *d = data_buf   + (size_t)i * data_stride;
		u8 *p = parity_buf + (size_t)i * parity_stride;
		int rc = ftrfs_rs_encode(d, data_len, p);

		if (rc < 0)
			return rc;
	}
	return 0;
}

/*
 * ftrfs_rs_decode_region -- decode N RS(255,239) shortened subblocks
 * across a region. Symmetric to ftrfs_rs_encode_region.
 *
 * @results:       optional, n_subblocks entries. On exit each
 *                 entry holds the return value of ftrfs_rs_decode
 *                 for the corresponding subblock: < 0 uncorrectable,
 *                 = 0 no errors, > 0 number of corrected symbols.
 *                 Pass NULL to skip per-subblock reporting.
 *
 * Returns 0 if every subblock decoded successfully, the first
 * negative error otherwise. Decoding does not stop on error: all
 * subblocks are processed, results[] reflects the per-subblock
 * outcome, and the worst negative error is returned.
 */
int ftrfs_rs_decode_region(u8 *data_buf, size_t data_stride,
			   u8 *parity_buf, size_t parity_stride,
			   size_t data_len, unsigned int n_subblocks,
			   int *results)
{
	unsigned int i;
	int worst = 0;

	if (!data_buf || !parity_buf)
		return -EINVAL;

	for (i = 0; i < n_subblocks; i++) {
		u8 *d = data_buf   + (size_t)i * data_stride;
		u8 *p = parity_buf + (size_t)i * parity_stride;
		int rc = ftrfs_rs_decode(d, data_len, p);

		if (results)
			results[i] = rc;
		if (rc < 0 && worst >= 0)
			worst = rc;
	}
	return worst;
}

/*
 * ftrfs_crc32 — compute CRC32 checksum
 * @buf: data buffer
 * @len: length in bytes
 *
 * Uses the kernel's hardware-accelerated crc32_le (same as ext4/btrfs).
 * Seed 0xFFFFFFFF, final XOR 0xFFFFFFFF (standard CRC-32/ISO-HDLC).
 */
__u32 ftrfs_crc32(const void *buf, size_t len)
{
	return crc32_le(0xFFFFFFFF, buf, len) ^ 0xFFFFFFFF;
}

/*
 * ftrfs_crc32_sb -- compute CRC32 over the meaningful regions of the
 *                   superblock, excluding s_crc32 itself and s_pad.
 *
 * Coverage:
 *   [0, offsetof(s_crc32))               = 64 bytes
 *   [offsetof(s_uuid), offsetof(s_pad))  = 1621 bytes
 *
 * Chained via crc32_le without intermediate XOR. Must match the
 * userspace mkfs.ftrfs implementation byte-for-byte so that
 * superblocks formatted by mkfs validate at mount time.
 *
 * The v3 format extension (commit 2ec4cb4) added 28 bytes of
 * feature fields between s_bitmap_blk and s_pad; the second
 * coverage region is sized to include them. v2 superblocks
 * predating that extension are correctly rejected by the
 * resulting CRC mismatch.
 */
__u32 ftrfs_crc32_sb(const struct ftrfs_super_block *fsb)
{
	const u8 *base = (const u8 *)fsb;
	u32 c;

	c = crc32_le(0xFFFFFFFF, base, 64);
	c = crc32_le(c, base + 68, 1689 - 68);
	return c ^ 0xFFFFFFFF;
}

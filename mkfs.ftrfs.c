/* SPDX-License-Identifier: GPL-2.0-only
 * mkfs.ftrfs — format a block device or image as FTRFS
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * Usage: mkfs.ftrfs <device_or_image>
 *
 * Layout:
 *   Block 0    : superblock
 *   Block 1..N : inode table
 *   Block N+1  : bitmap block (RS FEC protected)
 *   Block N+2  : root directory data
 *   Block N+3+ : data blocks
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <errno.h>
#include <stddef.h>

#define FTRFS_MAGIC         0x46545246
#define FTRFS_BLOCK_SIZE    4096
#define FTRFS_MAX_FILENAME  255
#define FTRFS_DIRECT_BLOCKS 12

/* RS FEC constants (must match kernel ftrfs.h) */
#define FTRFS_RS_PARITY        16
#define FTRFS_SUBBLOCK_DATA    239
#define FTRFS_SUBBLOCK_TOTAL   255
#define FTRFS_BITMAP_SUBBLOCKS 16
#define FTRFS_BITMAP_DATA_BYTES (FTRFS_BITMAP_SUBBLOCKS * FTRFS_SUBBLOCK_DATA)

/* Minimal CRC32 for userspace mkfs */
static uint32_t crc32_table[256];
static int crc32_init_done = 0;

static void crc32_init(void)
{
	uint32_t poly = 0xEDB88320;
	for (int i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int j = 0; j < 8; j++)
			c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
		crc32_table[i] = c;
	}
	crc32_init_done = 1;
}

/*
 * crc32_internal — compute CRC32, returning the raw internal state (no XOR).
 * seed: initial internal state (0xFFFFFFFF for first block, carry for chaining)
 */
static uint32_t crc32_internal(uint32_t seed, const void *buf, size_t len)
{
	if (!crc32_init_done) crc32_init();
	uint32_t c = seed;
	const uint8_t *p = buf;
	while (len--)
		c = crc32_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
	return c;
}

static uint32_t crc32(const void *buf, size_t len)
{
	return crc32_internal(0xFFFFFFFF, buf, len) ^ 0xFFFFFFFF;
}

/* On-disk structures (must match kernel ftrfs.h) */
#define FTRFS_RS_JOURNAL_SIZE  64

struct ftrfs_rs_event {
	uint64_t re_block_no;
	uint64_t re_timestamp;
	uint32_t re_error_bits;
	uint32_t re_crc32;
} __attribute__((packed)); /* 24 bytes */

struct ftrfs_super_block {
	uint32_t s_magic;
	uint32_t s_block_size;
	uint64_t s_block_count;
	uint64_t s_free_blocks;
	uint64_t s_inode_count;
	uint64_t s_free_inodes;
	uint64_t s_inode_table_blk;
	uint64_t s_data_start_blk;
	uint32_t s_version;
	uint32_t s_flags;
	uint32_t s_crc32;
	uint8_t  s_uuid[16];
	uint8_t  s_label[32];
	struct ftrfs_rs_event s_rs_journal[FTRFS_RS_JOURNAL_SIZE]; /* 1536 bytes */
	uint8_t  s_rs_journal_head;
	uint64_t s_bitmap_blk;       /* on-disk block bitmap block number */
	uint8_t  s_pad[2435];        /* padding to 4096 bytes */
} __attribute__((packed));

/*
 * crc32_sb — CRC32 over superblock fields matching ftrfs_crc32_sb() in kernel.
 * Covers [0, offsetof(s_crc32)) and [offsetof(s_uuid), offsetof(s_pad)).
 */
static uint32_t crc32_sb(const struct ftrfs_super_block *sb)
{
	const uint8_t *base = (const uint8_t *)sb;
	uint32_t c;

	c = crc32_internal(0xFFFFFFFF, base, 64);
	c = crc32_internal(c, base + 68, 1661 - 68);
	return c ^ 0xFFFFFFFF;
}


/*
 * rs_encode_bitmap — protect 239-byte subblocks with 16-byte RS parity.
 *
 * Exact reproduction of lib/reed_solomon encode_rs8() with parameters:
 *   init_rs(8, 0x187, fcr=0, prim=1, nroots=16)
 *
 * Uses index-form genpoly exactly as codec_init() builds it, and
 * the same LFSR feedback loop as encode_rs.c.
 *
 * Layout: [data0..238][par0..15][data239..477][par..] (16 subblocks)
 */
static void rs_encode_bitmap(uint8_t *block)
{
	/* GF(2^8) with primitive polynomial 0x187 */
	static const int nn = 255;
	uint16_t alpha_to[256], index_of[256];
	uint16_t genpoly[17]; /* index form, nroots+1 elements */
	int sr, i, j, root;

	/* Build GF tables */
	sr = 1;
	for (i = 0; i < nn; i++) {
		index_of[sr] = i;
		alpha_to[i]  = sr;
		sr <<= 1;
		if (sr & 256)
			sr ^= 0x187;
		sr &= nn;
	}
	alpha_to[nn] = 0;
	index_of[0]  = nn;

	/* Build generator polynomial in index form — exactly codec_init() */
	/* fcr=0, prim=1: roots are alpha^0 .. alpha^15 */
	uint16_t gp[17];
	memset(gp, 0, sizeof(gp));
	gp[0] = 1;
	root = 0; /* fcr * prim */
	for (i = 0; i < 16; i++) {
		gp[i + 1] = 1;
		for (j = i; j > 0; j--) {
			if (gp[j] != 0) {
				int idx = (index_of[gp[j]] + root) % nn;
				gp[j] = gp[j - 1] ^ alpha_to[idx];
			} else {
				gp[j] = gp[j - 1];
			}
		}
		gp[0] = alpha_to[(index_of[gp[0]] + root) % nn];
		root += 1; /* += prim */
	}
	/* Convert to index form */
	for (i = 0; i <= 16; i++)
		genpoly[i] = index_of[gp[i]];

	/* Encode each subblock — exactly encode_rs.c LFSR */
	for (int k = 0; k < FTRFS_BITMAP_SUBBLOCKS; k++) {
		uint8_t  *data   = block + k * FTRFS_SUBBLOCK_TOTAL;
		uint16_t  par[16];
		memset(par, 0, sizeof(par));

		for (i = 0; i < FTRFS_SUBBLOCK_DATA; i++) {
			uint16_t fb = index_of[(data[i] ^ (uint8_t)par[0]) & nn];
			if (fb != (uint16_t)nn) {
				for (j = 1; j < 16; j++)
					par[j] ^= alpha_to[(fb + genpoly[16 - j]) % nn];
			}
			memmove(&par[0], &par[1], sizeof(uint16_t) * 15);
			par[15] = (fb != (uint16_t)nn)
				? alpha_to[(fb + genpoly[0]) % nn]
				: 0;
		}

		/* Write parity bytes after data */
		uint8_t *parity = data + FTRFS_SUBBLOCK_DATA;
		for (j = 0; j < 16; j++)
			parity[j] = (uint8_t)par[j];
	}
}

struct ftrfs_inode {
	uint16_t i_mode;
	uint16_t i_nlink;
	uint32_t i_uid;
	uint32_t i_gid;
	uint64_t i_size;
	uint64_t i_atime;
	uint64_t i_mtime;
	uint64_t i_ctime;
	uint32_t i_flags;
	uint32_t i_crc32;
	uint64_t i_direct[FTRFS_DIRECT_BLOCKS];
	uint64_t i_indirect;
	uint64_t i_dindirect;
	uint64_t i_tindirect;
	uint8_t  i_reserved[84];
} __attribute__((packed));

struct ftrfs_dir_entry {
	uint64_t d_ino;
	uint16_t d_rec_len;
	uint8_t  d_name_len;
	uint8_t  d_file_type;
	char     d_name[FTRFS_MAX_FILENAME + 1];
} __attribute__((packed));

static void write_block(int fd, uint64_t block, const void *buf)
{
	off_t off = (off_t)block * FTRFS_BLOCK_SIZE;
	if (lseek(fd, off, SEEK_SET) < 0) {
		perror("lseek");
		exit(1);
	}
	if (write(fd, buf, FTRFS_BLOCK_SIZE) != FTRFS_BLOCK_SIZE) {
		perror("write");
		exit(1);
	}
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <device_or_image>\n", argv[0]);
		return 1;
	}

	int fd = open(argv[1], O_RDWR);
	if (fd < 0) { perror("open"); return 1; }

	struct stat st;
	if (fstat(fd, &st) < 0) { perror("fstat"); return 1; }

	uint64_t total_bytes;
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &total_bytes) < 0) {
			perror("ioctl BLKGETSIZE64");
			return 1;
		}
	} else {
		total_bytes = (uint64_t)st.st_size;
	}

	uint64_t total_blocks = total_bytes / FTRFS_BLOCK_SIZE;
	if (total_blocks < 16) {
		fprintf(stderr, "ftrfs: image too small (need >= 16 blocks)\n");
		return 1;
	}

	/* Layout:
	 * Block 0      : superblock
	 * Block 1-4    : inode table (4 blocks = 64 inodes @ 256B each)
	 * Block 5      : bitmap block (RS FEC protected)
	 * Block 6      : root dir data
	 * Block 7+     : free data blocks
	 */
	uint64_t inode_table_blk = 1;
	uint64_t inode_table_len = 4;
	uint64_t bitmap_blk      = inode_table_blk + inode_table_len;
	uint64_t data_start_blk  = bitmap_blk + 2;
	uint64_t inodes_per_block = FTRFS_BLOCK_SIZE / sizeof(struct ftrfs_inode);
	uint64_t total_inodes    = inode_table_len * inodes_per_block;
	uint64_t root_dir_blk    = bitmap_blk + 1;

	uint8_t zero[FTRFS_BLOCK_SIZE];
	memset(zero, 0, sizeof(zero));

	/* Zero inode table blocks */
	for (uint64_t i = 0; i < inode_table_len; i++)
		write_block(fd, inode_table_blk + i, zero);

	/*
	 * Write bitmap block — all bits set (= all blocks free).
	 * 16 subblocks of 239 data bytes each, each followed by 16 bytes
	 * of RS(255,239) parity. Total: 16 * 255 = 4080 bytes, 16 unused.
	 */
	uint8_t bitmap_block[FTRFS_BLOCK_SIZE];
	memset(bitmap_block, 0, sizeof(bitmap_block));
	for (int s = 0; s < FTRFS_BITMAP_SUBBLOCKS; s++)
		memset(bitmap_block + s * FTRFS_SUBBLOCK_TOTAL, 0xFF, FTRFS_SUBBLOCK_DATA);
	rs_encode_bitmap(bitmap_block);
	write_block(fd, bitmap_blk, bitmap_block);

	/* Write root directory data block (empty dir, just . and ..) */
	uint8_t dir_block[FTRFS_BLOCK_SIZE];
	memset(dir_block, 0, sizeof(dir_block));

	struct ftrfs_dir_entry *de = (struct ftrfs_dir_entry *)dir_block;
	/* Entry "." */
	de->d_ino      = 1; /* root inode = 1 */
	de->d_name_len = 1;
	de->d_file_type = 2; /* DT_DIR */
	de->d_name[0]  = '.';
	de->d_rec_len  = sizeof(struct ftrfs_dir_entry);

	/* Entry ".." */
	de = (struct ftrfs_dir_entry *)(dir_block + sizeof(struct ftrfs_dir_entry));
	de->d_ino      = 1;
	de->d_name_len = 2;
	de->d_file_type = 2;
	de->d_name[0]  = '.';
	de->d_name[1]  = '.';
	de->d_rec_len  = sizeof(struct ftrfs_dir_entry);

	write_block(fd, root_dir_blk, dir_block);

	/* Write root inode (inode 1) */
	uint8_t inode_block[FTRFS_BLOCK_SIZE];
	memset(inode_block, 0, sizeof(inode_block));

	struct ftrfs_inode *ri = (struct ftrfs_inode *)inode_block;
	/* inode 1 = first slot in block 1 */
	ri->i_mode   = 0040755; /* drwxr-xr-x */
	ri->i_uid    = 0;
	ri->i_gid    = 0;
	ri->i_nlink  = 2;
	ri->i_size   = FTRFS_BLOCK_SIZE;
	ri->i_direct[0] = root_dir_blk;
	ri->i_crc32  = crc32(ri, offsetof(struct ftrfs_inode, i_crc32));

	write_block(fd, inode_table_blk, inode_block);

	/* Write superblock */
	struct ftrfs_super_block sb;
	memset(&sb, 0, sizeof(sb));
	sb.s_magic          = FTRFS_MAGIC;
	sb.s_block_size     = FTRFS_BLOCK_SIZE;
	sb.s_block_count    = total_blocks;
	sb.s_free_blocks    = total_blocks - data_start_blk;
	sb.s_inode_count    = total_inodes;
	sb.s_free_inodes    = total_inodes - 1; /* root inode used */
	sb.s_inode_table_blk = inode_table_blk;
	sb.s_data_start_blk  = data_start_blk;
	sb.s_version        = 2; /* v2: on-disk bitmap with RS FEC */
	sb.s_bitmap_blk     = bitmap_blk;
	sb.s_crc32          = crc32_sb(&sb);

	write_block(fd, 0, &sb);
	close(fd);

	printf("mkfs.ftrfs: formatted %s\n", argv[1]);
	printf("  blocks:  %lu (free: %lu)\n",
	       (unsigned long)total_blocks,
	       (unsigned long)sb.s_free_blocks);
	printf("  inodes:  %lu (free: %lu)\n",
	       (unsigned long)total_inodes,
	       (unsigned long)sb.s_free_inodes);
	printf("  inode table: block %lu\n", (unsigned long)inode_table_blk);
	printf("  bitmap:      block %lu (RS FEC protected)\n", (unsigned long)bitmap_blk);
	printf("  data start:  block %lu\n", (unsigned long)data_start_blk);
	return 0;
}

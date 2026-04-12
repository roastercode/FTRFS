/* SPDX-License-Identifier: GPL-2.0-only
 * mkfs.ftrfs — format a block device or image as FTRFS
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * Usage: mkfs.ftrfs <device_or_image>
 *
 * Layout:
 *   Block 0    : superblock
 *   Block 1    : inode table (start)
 *   Block 1+N  : data blocks
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <stddef.h>

#define FTRFS_MAGIC         0x46545246
#define FTRFS_BLOCK_SIZE    4096
#define FTRFS_MAX_FILENAME  255
#define FTRFS_DIRECT_BLOCKS 12

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

static uint32_t crc32(const void *buf, size_t len)
{
	if (!crc32_init_done) crc32_init();
	uint32_t c = 0xFFFFFFFF;
	const uint8_t *p = buf;
	while (len--)
		c = crc32_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFF;
}

/* On-disk structures (must match kernel ftrfs.h) */
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
	uint8_t  s_pad[3948];
} __attribute__((packed));

struct ftrfs_inode {
	uint16_t i_mode;
	uint16_t i_uid;
	uint16_t i_gid;
	uint16_t i_nlink;
	uint64_t i_size;
	uint64_t i_atime;
	uint64_t i_mtime;
	uint64_t i_ctime;
	uint32_t i_blocks;
	uint32_t i_flags;
	uint64_t i_direct[FTRFS_DIRECT_BLOCKS];
	uint64_t i_indirect;
	uint64_t i_dindirect;
	uint32_t i_crc32;
	uint8_t  i_pad[2];
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

	uint64_t total_bytes = (S_ISBLK(st.st_mode))
		? (uint64_t)st.st_size  /* block dev: use ioctl in real mkfs */
		: (uint64_t)st.st_size;

	uint64_t total_blocks = total_bytes / FTRFS_BLOCK_SIZE;
	if (total_blocks < 16) {
		fprintf(stderr, "ftrfs: image too small (need >= 16 blocks)\n");
		return 1;
	}

	/* Layout:
	 * Block 0      : superblock
	 * Block 1-4    : inode table (4 blocks = 128 inodes @ 128B each)
	 * Block 5      : root dir data
	 * Block 6+     : free data blocks
	 */
	uint64_t inode_table_blk = 1;
	uint64_t inode_table_len = 4;
	uint64_t data_start_blk  = inode_table_blk + inode_table_len + 1;
	uint64_t inodes_per_block = FTRFS_BLOCK_SIZE / sizeof(struct ftrfs_inode);
	uint64_t total_inodes    = inode_table_len * inodes_per_block;
	uint64_t root_dir_blk    = inode_table_blk + inode_table_len;

	uint8_t zero[FTRFS_BLOCK_SIZE];
	memset(zero, 0, sizeof(zero));

	/* Zero inode table blocks */
	for (uint64_t i = 0; i < inode_table_len; i++)
		write_block(fd, inode_table_blk + i, zero);

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
	ri->i_blocks = 1;
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
	sb.s_version        = 1;
	sb.s_crc32          = crc32(&sb, offsetof(struct ftrfs_super_block, s_crc32));

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
	printf("  data start:  block %lu\n", (unsigned long)data_start_blk);
	return 0;
}

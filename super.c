// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTRFS — Superblock operations
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/statfs.h>
#include "ftrfs.h"

/* Inode cache (slab allocator) */
static struct kmem_cache *ftrfs_inode_cachep;

/*
 * alloc_inode — allocate a new inode with ftrfs_inode_info embedded
 */
static struct inode *ftrfs_alloc_inode(struct super_block *sb)
{
	struct ftrfs_inode_info *fi;

	fi = kmem_cache_alloc(ftrfs_inode_cachep, GFP_KERNEL);
	if (!fi)
		return NULL;

	memset(fi->i_direct, 0, sizeof(fi->i_direct));
	fi->i_indirect  = 0;
	fi->i_dindirect = 0;
	fi->i_tindirect = 0;
	fi->i_flags     = 0;

	return &fi->vfs_inode;
}

/*
 * free_inode — return inode to slab cache (kernel 5.9+ uses free_inode)
 */
static void ftrfs_free_inode(struct inode *inode)
{
	kmem_cache_free(ftrfs_inode_cachep, FTRFS_I(inode));
}

/*
 * statfs — filesystem statistics
 */
static int ftrfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block   *sb  = dentry->d_sb;
	struct ftrfs_sb_info *sbi = FTRFS_SB(sb);

	buf->f_type    = FTRFS_MAGIC;
	buf->f_bsize   = sb->s_blocksize;
	buf->f_blocks  = le64_to_cpu(sbi->s_ftrfs_sb->s_block_count);
	buf->f_bfree   = sbi->s_free_blocks;
	buf->f_bavail  = sbi->s_free_blocks;
	buf->f_files   = le64_to_cpu(sbi->s_ftrfs_sb->s_inode_count);
	buf->f_ffree   = sbi->s_free_inodes;
	buf->f_namelen = FTRFS_MAX_FILENAME;

	return 0;
}

/*
 * put_super — release superblock resources
 */
static void ftrfs_put_super(struct super_block *sb)
{
	struct ftrfs_sb_info *sbi = FTRFS_SB(sb);

	if (sbi) {
		ftrfs_destroy_bitmap(sb);
		brelse(sbi->s_sbh);
		kfree(sbi->s_ftrfs_sb);
		kfree(sbi);
		sb->s_fs_info = NULL;
	}
}

/*
 * evict_inode — called when inode nlink drops to 0 and last reference released
 * Frees the inode number back to the bitmap.
 */
/*
 * ftrfs_free_data_blocks -- release all data blocks of a deleted inode.
 *
 * Frees direct blocks and the single indirect block (and all blocks
 * it points to). Called from evict_inode when nlink drops to 0.
 */
static void ftrfs_free_data_blocks(struct inode *inode)
{
	struct ftrfs_inode_info *fi = FTRFS_I(inode);
	struct super_block      *sb = inode->i_sb;
	int i;

	/* Free direct blocks */
	for (i = 0; i < FTRFS_DIRECT_BLOCKS; i++) {
		u64 blk = le64_to_cpu(fi->i_direct[i]);

		if (blk) {
			ftrfs_free_block(sb, blk);
			fi->i_direct[i] = 0;
		}
	}

	/* Free single indirect block and all blocks it points to */
	if (fi->i_indirect) {
		u64 indirect_blk = le64_to_cpu(fi->i_indirect);
		struct buffer_head *ibh = sb_bread(sb, indirect_blk);

		if (ibh) {
			__le64 *ptrs = (__le64 *)ibh->b_data;
			u64 nptrs = FTRFS_BLOCK_SIZE / sizeof(__le64);
			u64 j;

			for (j = 0; j < nptrs; j++) {
				u64 blk = le64_to_cpu(ptrs[j]);

				if (blk)
					ftrfs_free_block(sb, blk);
			}
			brelse(ibh);
		}
		ftrfs_free_block(sb, indirect_blk);
		fi->i_indirect = 0;
	}
}

static void ftrfs_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	/*
	 * If the file is truly deleted (nlink == 0), free all data blocks,
	 * zero i_mode on disk so the inode table scan at next mount
	 * correctly identifies this slot as free, then release the inode
	 * number back to the bitmap.
	 */
	if (!inode->i_nlink) {
		ftrfs_free_data_blocks(inode);
		inode->i_mode = 0;
		ftrfs_write_inode_raw(inode);
		ftrfs_free_inode_num(inode->i_sb, (u64)inode->i_ino);
	}
	clear_inode(inode);
}

static const struct super_operations ftrfs_super_ops = {
	.alloc_inode    = ftrfs_alloc_inode,
	.free_inode     = ftrfs_free_inode,
	.evict_inode    = ftrfs_evict_inode,
	.put_super      = ftrfs_put_super,
	.write_inode    = ftrfs_write_inode,
	.statfs         = ftrfs_statfs,
};

/*
 * ftrfs_dirty_super - propagate the authoritative in-memory superblock
 * (sbi->s_ftrfs_sb) onto the buffer head, recompute s_crc32, and mark
 * the buffer dirty for writeback.
 *
 * Every site that mutates the on-disk superblock (free_blocks,
 * free_inodes, RS journal, ...) must call this helper instead of
 * mark_buffer_dirty(sbi->s_sbh) directly. Without the CRC refresh,
 * the on-disk superblock keeps a stale checksum that fails verification
 * at the next mount.
 *
 * Caller MUST hold sbi->s_lock so that the snapshot copied to the
 * buffer head is taken atomically with respect to other writers.
 */
/*
 * ftrfs_sb_to_rs_staging -- serialize the CRC32-covered region of a
 * superblock into a contiguous staging buffer for RS encode/decode.
 *
 * Output buffer layout (1688 bytes):
 *   [   0..  63]  copy of sb_bytes[0..63]   (region A, 64 bytes)
 *   [  64..1684]  copy of sb_bytes[68..1688] (region B, 1621 bytes)
 *   [1685..1687]  zero pad (3 bytes for shortened RS)
 *
 * The 4 bytes at sb_bytes[64..67] (s_crc32) are excluded, exactly as
 * ftrfs_crc32_sb() does. Same coverage on both protection layers.
 *
 * Must match mkfs.ftrfs.c::sb_to_rs_staging() byte-for-byte.
 */
static void ftrfs_sb_to_rs_staging(const struct ftrfs_super_block *sb,
				   u8 staging[FTRFS_SB_RS_STAGING_BYTES])
{
	const u8 *base = (const u8 *)sb;

	memcpy(staging,        base,        64);
	memcpy(staging + 64,   base + 68,   1689 - 68);
	memset(staging + 1685, 0,           3);
}

/*
 * ftrfs_sb_from_rs_staging -- inverse of ftrfs_sb_to_rs_staging.
 * Restores the (possibly RS-corrected) bytes from staging back onto
 * the superblock, leaving s_crc32 (bytes [64..67]) untouched. The
 * 3 trailing zero-pad bytes of staging are not copied back.
 *
 * Used on the mount-time RS recovery path in ftrfs_fill_super.
 */
static void ftrfs_sb_from_rs_staging(const u8 staging[FTRFS_SB_RS_STAGING_BYTES],
				     struct ftrfs_super_block *sb)
{
	u8 *base = (u8 *)sb;

	memcpy(base,        staging,        64);
	memcpy(base + 68,   staging + 64,   1689 - 68);
}

void ftrfs_dirty_super(struct ftrfs_sb_info *sbi)
{
	struct ftrfs_super_block *fsb;
	u32 crc;

	if (!sbi || !sbi->s_sbh || !sbi->s_ftrfs_sb)
		return;

	fsb = (struct ftrfs_super_block *)sbi->s_sbh->b_data;

	/*
	 * Copy the authoritative in-memory image onto the buffer head.
	 * Several callers update sbi->s_ftrfs_sb in place without touching
	 * fsb; this memcpy serializes them onto disk.
	 */
	memcpy(fsb, sbi->s_ftrfs_sb, sizeof(*fsb));

	/* Encode RS parity over CRC32-covered region (skipping s_crc32). */
	{
		u8 staging[FTRFS_SB_RS_STAGING_BYTES];
		u8 *parity_dst = (u8 *)fsb + FTRFS_SB_RS_PARITY_OFFSET;

		ftrfs_sb_to_rs_staging(fsb, staging);
		ftrfs_rs_encode_region(staging, FTRFS_SB_RS_DATA_LEN,
				       parity_dst, FTRFS_RS_PARITY,
				       FTRFS_SB_RS_DATA_LEN,
				       FTRFS_SB_RS_SUBBLOCKS);
	}

	crc = ftrfs_crc32_sb(fsb);
	fsb->s_crc32 = cpu_to_le32(crc);
	sbi->s_ftrfs_sb->s_crc32 = fsb->s_crc32;

	mark_buffer_dirty(sbi->s_sbh);
}

/*
 * ftrfs_log_rs_event - record a Reed-Solomon correction in the superblock
 * @sb:        mounted superblock
 * @block_no:  block number where correction occurred
 * @err_bits:  number of symbols corrected
 *
 * Writes to the persistent ring buffer in the superblock.
 * Safe to call from any context (spinlock protected).
 */
void ftrfs_log_rs_event(struct super_block *sb, u64 block_no, u32 err_bits)
{
	struct ftrfs_sb_info  *sbi = FTRFS_SB(sb);
	struct ftrfs_rs_event *ev;
	u8 head;

	if (!sbi || !sbi->s_sbh)
		return;

	spin_lock(&sbi->s_lock);

	/*
	 * Update the in-memory authoritative copy first; ftrfs_dirty_super
	 * then propagates it to the buffer head with a refreshed s_crc32.
	 */
	head = sbi->s_ftrfs_sb->s_rs_journal_head % FTRFS_RS_JOURNAL_SIZE;
	ev   = &sbi->s_ftrfs_sb->s_rs_journal[head];

	ev->re_block_no   = cpu_to_le64(block_no);
	ev->re_timestamp  = cpu_to_le64(ktime_get_ns());
	ev->re_error_bits = cpu_to_le32(err_bits);
	{
		u32 ev_crc = ftrfs_crc32(ev,
					  offsetof(struct ftrfs_rs_event,
						   re_crc32));
		ev->re_crc32 = cpu_to_le32(ev_crc);
	}

	sbi->s_ftrfs_sb->s_rs_journal_head = (head + 1) % FTRFS_RS_JOURNAL_SIZE;

	ftrfs_dirty_super(sbi);

	spin_unlock(&sbi->s_lock);

	pr_debug("ftrfs: RS correction block=%llu symbols=%u\n",
		 block_no, err_bits);
}

/*
 * ftrfs_fill_super — read superblock from disk and initialize VFS sb
 */
int ftrfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct ftrfs_sb_info     *sbi;
	struct ftrfs_super_block *fsb;
	struct buffer_head       *bh;
	struct inode             *root_inode;
	__u32                     crc;
	int                       ret = -EINVAL;

	/* Set block size */
	if (!sb_set_blocksize(sb, FTRFS_BLOCK_SIZE)) {
		errorf(fc, "ftrfs: unable to set block size %d", FTRFS_BLOCK_SIZE);
		return -EINVAL;
	}

	/* Read block 0 — superblock */
	bh = sb_bread(sb, 0);
	if (!bh) {
		errorf(fc, "ftrfs: unable to read superblock");
		return -EIO;
	}

	fsb = (struct ftrfs_super_block *)bh->b_data;

	/* Verify magic */
	if (le32_to_cpu(fsb->s_magic) != FTRFS_MAGIC) {
		errorf(fc, "ftrfs: bad magic 0x%08x (expected 0x%08x)",
		       le32_to_cpu(fsb->s_magic), FTRFS_MAGIC);
		goto out_brelse;
	}

	/* Verify CRC32 of superblock (excluding the crc32 field itself) */
	crc = ftrfs_crc32_sb(fsb);
	if (crc != le32_to_cpu(fsb->s_crc32)) {
		u8 staging[FTRFS_SB_RS_STAGING_BYTES];
		u8 *parity_src = (u8 *)fsb + FTRFS_SB_RS_PARITY_OFFSET;
		int rs_results[FTRFS_SB_RS_SUBBLOCKS];
		int rc;

		pr_warn("ftrfs: superblock CRC32 mismatch (got 0x%08x, expected 0x%08x), attempting RS recovery\n",
			crc, le32_to_cpu(fsb->s_crc32));

		ftrfs_sb_to_rs_staging(fsb, staging);
		rc = ftrfs_rs_decode_region(staging, FTRFS_SB_RS_DATA_LEN,
					    parity_src, FTRFS_RS_PARITY,
					    FTRFS_SB_RS_DATA_LEN,
					    FTRFS_SB_RS_SUBBLOCKS,
					    rs_results);
		if (rc < 0) {
			errorf(fc, "ftrfs: superblock CRC32 mismatch and RS uncorrectable");
			goto out_brelse;
		}

		ftrfs_sb_from_rs_staging(staging, fsb);

		crc = ftrfs_crc32_sb(fsb);
		if (crc != le32_to_cpu(fsb->s_crc32)) {
			errorf(fc, "ftrfs: superblock CRC32 still mismatch after RS recovery");
			goto out_brelse;
		}

		pr_warn("ftrfs: superblock corrected by RS FEC\n");
	}

	/*
	 * Validate v3 feature fields.
	 *
	 * s_data_protection_scheme: range-check against the enum maximum.
	 *   The three high-order bytes of the __le32 act as a structural
	 *   sentinel; any value above FTRFS_DATA_PROTECTION_MAX is rejected.
	 * s_feat_incompat:  unknown bits are a hard refusal (any read).
	 * s_feat_ro_compat: unknown bits force SB_RDONLY but allow mount.
	 * s_feat_compat:    informational, never gates mount.
	 */
	{
		u32 scheme = le32_to_cpu(fsb->s_data_protection_scheme);
		u64 unknown_incompat  = le64_to_cpu(fsb->s_feat_incompat) &
					~FTRFS_FEAT_INCOMPAT_SUPP;
		u64 unknown_ro_compat = le64_to_cpu(fsb->s_feat_ro_compat) &
					~FTRFS_FEAT_RO_COMPAT_SUPP;
		u64 unknown_compat    = le64_to_cpu(fsb->s_feat_compat) &
					~FTRFS_FEAT_COMPAT_SUPP;

		if (scheme > FTRFS_DATA_PROTECTION_MAX) {
			errorf(fc, "ftrfs: invalid data_protection_scheme %u (max %u)",
			       scheme, FTRFS_DATA_PROTECTION_MAX);
			goto out_brelse;
		}

		if (unknown_incompat) {
			errorf(fc, "ftrfs: unsupported incompat features 0x%016llx",
			       unknown_incompat);
			goto out_brelse;
		}

		if (unknown_ro_compat && !sb_rdonly(sb)) {
			pr_warn("ftrfs: unsupported ro_compat features 0x%016llx, forcing read-only mount\n",
				unknown_ro_compat);
			sb->s_flags |= SB_RDONLY;
		}

		if (unknown_compat)
			pr_info("ftrfs: unknown compat features 0x%016llx (informational)\n",
				unknown_compat);
	}

	/* Allocate in-memory sb info */
	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi) {
		ret = -ENOMEM;
		goto out_brelse;
	}

	sbi->s_ftrfs_sb = kzalloc(sizeof(*sbi->s_ftrfs_sb), GFP_KERNEL);
	if (!sbi->s_ftrfs_sb) {
		ret = -ENOMEM;
		goto out_free_sbi;
	}

	memcpy(sbi->s_ftrfs_sb, fsb, sizeof(*fsb));
	sbi->s_sbh         = bh;
	sbi->s_free_blocks = le64_to_cpu(fsb->s_free_blocks);
	sbi->s_free_inodes = le64_to_cpu(fsb->s_free_inodes);
	spin_lock_init(&sbi->s_lock);

	sb->s_fs_info  = sbi;
	sb->s_magic    = FTRFS_MAGIC;
	sb->s_op       = &ftrfs_super_ops;
	sb->s_maxbytes = MAX_LFS_FILESIZE;

	/* Read root inode (inode 1) */
	root_inode = ftrfs_iget(sb, 1);
	if (IS_ERR(root_inode)) {
		ret = PTR_ERR(root_inode);
		pr_err("ftrfs: failed to read root inode: %d\n", ret);
		goto out_free_fsb;
	}

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto out_free_fsb;
	}

	if (ftrfs_setup_bitmap(sb)) {
		ret = -ENOMEM;
		goto out_put_root;
	}

	pr_info("ftrfs: mounted v%u (blocks=%llu free=%lu inodes=%llu scheme=%u feat=0x%016llx/0x%016llx/0x%016llx)\n",
		le32_to_cpu(fsb->s_version),
		le64_to_cpu(fsb->s_block_count),
		sbi->s_free_blocks,
		le64_to_cpu(fsb->s_inode_count),
		le32_to_cpu(fsb->s_data_protection_scheme),
		le64_to_cpu(fsb->s_feat_compat),
		le64_to_cpu(fsb->s_feat_incompat),
		le64_to_cpu(fsb->s_feat_ro_compat));

	return 0;

out_put_root:
	dput(sb->s_root);
	sb->s_root = NULL;
out_free_fsb:
	kfree(sbi->s_ftrfs_sb);
out_free_sbi:
	kfree(sbi);
	sb->s_fs_info = NULL;
out_brelse:
	brelse(bh);
	return ret;
}

/*
 * fs_context ops — kernel 5.15+ mount API
 */
static int ftrfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, ftrfs_fill_super);
}

/*
 * ftrfs_reconfigure — handle mount -o remount
 *
 * xfstests calls remount,ro after each test to verify filesystem
 * integrity. FTRFS accepts the reconfigure request without
 * taking any action — ro/rw transitions are handled by the VFS.
 */
static int ftrfs_reconfigure(struct fs_context *fc)
{
	return 0;
}

static const struct fs_context_operations ftrfs_context_ops = {
	.get_tree   = ftrfs_get_tree,
	.reconfigure = ftrfs_reconfigure,
};

static int ftrfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &ftrfs_context_ops;
	return 0;
}

static struct file_system_type ftrfs_fs_type = {
	.owner            = THIS_MODULE,
	.name             = "ftrfs",
	.init_fs_context  = ftrfs_init_fs_context,
	.kill_sb          = kill_block_super,
	.fs_flags         = FS_REQUIRES_DEV,
};

/*
 * Inode cache constructor
 */
static void ftrfs_inode_init_once(void *obj)
{
	struct ftrfs_inode_info *fi = obj;

	inode_init_once(&fi->vfs_inode);
}

/*
 * Module init / exit
 */
static int __init ftrfs_init(void)
{
	int ret;

	/* Verify on-disk structure sizes at compile time */
	BUILD_BUG_ON(sizeof(struct ftrfs_super_block) != FTRFS_BLOCK_SIZE);
	BUILD_BUG_ON(sizeof(struct ftrfs_inode) != 256);

	/* Initialize GF(2^8) tables for RS FEC — once, before any mount */
	ftrfs_rs_init_tables();

	ftrfs_inode_cachep =
		kmem_cache_create("ftrfs_inode_cache",
				  sizeof(struct ftrfs_inode_info),
				  0,
				  SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
				  ftrfs_inode_init_once);

	if (!ftrfs_inode_cachep) {
		pr_err("ftrfs: failed to create inode cache\n");
		return -ENOMEM;
	}

	ret = register_filesystem(&ftrfs_fs_type);
	if (ret) {
		pr_err("ftrfs: failed to register filesystem: %d\n", ret);
		kmem_cache_destroy(ftrfs_inode_cachep);
		return ret;
	}

	pr_info("ftrfs: module loaded (FTRFS Fault-Tolerant Radiation-Robust FS)\n");
	return 0;
}

static void __exit ftrfs_exit(void)
{
	unregister_filesystem(&ftrfs_fs_type);
	rcu_barrier();
	kmem_cache_destroy(ftrfs_inode_cachep);
	ftrfs_rs_exit_tables();
	pr_info("ftrfs: module unloaded\n");
}

module_init(ftrfs_init);
module_exit(ftrfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aurelien DESBRIERES <aurelien@hackers.camp>");
MODULE_DESCRIPTION("FTRFS: Fault-Tolerant Radiation-Robust Filesystem");
MODULE_VERSION("0.1.0");
MODULE_ALIAS_FS("ftrfs");
MODULE_SOFTDEP("pre: reed_solomon");

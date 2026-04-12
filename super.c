// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTRFS — Superblock operations
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
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
	fi->i_flags     = 0;

	return &fi->vfs_inode;
}

/*
 * destroy_inode — return inode to slab cache
 */
static void ftrfs_destroy_inode(struct inode *inode)
{
	kmem_cache_free(ftrfs_inode_cachep, FTRFS_I(inode));
}

/*
 * statfs — filesystem statistics
 */
static int ftrfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block   *sb = dentry->d_sb;
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
		brelse(sbi->s_sbh);
		kfree(sbi->s_ftrfs_sb);
		kfree(sbi);
		sb->s_fs_info = NULL;
	}
}

static const struct super_operations ftrfs_super_ops = {
	.alloc_inode    = ftrfs_alloc_inode,
	.destroy_inode  = ftrfs_destroy_inode,
	.put_super      = ftrfs_put_super,
	.statfs         = ftrfs_statfs,
};

/*
 * ftrfs_fill_super — read superblock from disk and initialize VFS sb
 */
int ftrfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct ftrfs_sb_info    *sbi;
	struct ftrfs_super_block *fsb;
	struct buffer_head      *bh;
	struct inode            *root_inode;
	__u32                    crc;
	int                      ret = -EINVAL;

	/* Set block size */
	if (!sb_set_blocksize(sb, FTRFS_BLOCK_SIZE)) {
		if (!silent)
			pr_err("ftrfs: unable to set block size %d\n",
			       FTRFS_BLOCK_SIZE);
		return -EINVAL;
	}

	/* Read block 0 — superblock */
	bh = sb_bread(sb, 0);
	if (!bh) {
		if (!silent)
			pr_err("ftrfs: unable to read superblock\n");
		return -EIO;
	}

	fsb = (struct ftrfs_super_block *)bh->b_data;

	/* Verify magic */
	if (le32_to_cpu(fsb->s_magic) != FTRFS_MAGIC) {
		if (!silent)
			pr_err("ftrfs: bad magic 0x%08x (expected 0x%08x)\n",
			       le32_to_cpu(fsb->s_magic), FTRFS_MAGIC);
		goto out_brelse;
	}

	/* Verify CRC32 of superblock (excluding the crc32 field itself) */
	crc = ftrfs_crc32(fsb, offsetof(struct ftrfs_super_block, s_crc32));
	if (crc != le32_to_cpu(fsb->s_crc32)) {
		if (!silent)
			pr_err("ftrfs: superblock CRC32 mismatch "
			       "(got 0x%08x, expected 0x%08x)\n",
			       crc, le32_to_cpu(fsb->s_crc32));
		goto out_brelse;
	}

	/* Allocate in-memory sb info */
	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi) {
		ret = -ENOMEM;
		goto out_brelse;
	}

	sbi->s_ftrfs_sb   = kzalloc(sizeof(*sbi->s_ftrfs_sb), GFP_KERNEL);
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

	pr_info("ftrfs: mounted (blocks=%llu free=%lu inodes=%llu)\n",
		le64_to_cpu(fsb->s_block_count),
		sbi->s_free_blocks,
		le64_to_cpu(fsb->s_inode_count));

	return 0;

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
 * mount — VFS entry point
 */
static struct dentry *ftrfs_mount(struct file_system_type *fs_type,
				  int flags, const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, ftrfs_fill_super);
}

static struct file_system_type ftrfs_fs_type = {
	.owner    = THIS_MODULE,
	.name     = "ftrfs",
	.mount    = ftrfs_mount,
	.kill_sb  = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
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

	ftrfs_inode_cachep = kmem_cache_create(
		"ftrfs_inode_cache",
		sizeof(struct ftrfs_inode_info),
		0,
		SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD | SLAB_ACCOUNT,
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
	/*
	 * Ensure all RCU callbacks have run before destroying the cache,
	 * same pattern as ext2/ext4.
	 */
	rcu_barrier();
	kmem_cache_destroy(ftrfs_inode_cachep);
	pr_info("ftrfs: module unloaded\n");
}

module_init(ftrfs_init);
module_exit(ftrfs_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>");
MODULE_DESCRIPTION("FTRFS: Fault-Tolerant Radiation-Robust Filesystem");
MODULE_VERSION("0.1.0");
MODULE_ALIAS_FS("ftrfs");

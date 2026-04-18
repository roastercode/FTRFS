// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTRFS — File operations
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/iomap.h>
#include <linux/pagemap.h>
#include <linux/buffer_head.h>
#include "ftrfs.h"

/* Forward declaration — defined after iomap_ops */
static ssize_t ftrfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from);

const struct file_operations ftrfs_file_operations = {
	.llseek         = generic_file_llseek,
	.read_iter      = generic_file_read_iter,
	.write_iter     = ftrfs_file_write_iter,
	.mmap           = generic_file_mmap,
	.fsync          = generic_file_fsync,
	.splice_read    = filemap_splice_read,
};

const struct inode_operations ftrfs_file_inode_operations = {
	.getattr        = simple_getattr,
};

/*
 * FTRFS_INDIRECT_PTRS: number of block pointers per indirect block.
 * Each pointer is a u64 (8 bytes), so 4096 / 8 = 512 entries.
 * Single indirect capacity: 512 blocks = 2 MiB.
 */
#define FTRFS_INDIRECT_PTRS (FTRFS_BLOCK_SIZE / sizeof(__le64))

/*
 * ftrfs_iomap_begin -- map a file range to disk blocks for iomap.
 * Handles read (no allocation) and write (allocate on demand).
 * Supports direct blocks (iblock 0..11) and single indirect
 * (iblock 12..523, covering up to ~2 MiB per file).
 */
static int ftrfs_iomap_begin(struct inode *inode, loff_t pos, loff_t length,
			     unsigned int flags, struct iomap *iomap,
			     struct iomap *srcmap)
{
	struct ftrfs_inode_info *fi  = FTRFS_I(inode);
	struct super_block      *sb  = inode->i_sb;
	u64  iblock    = pos >> FTRFS_BLOCK_SHIFT;
	u64  new_block;
	u64  phys;

	iomap->offset = iblock << FTRFS_BLOCK_SHIFT;
	iomap->length = FTRFS_BLOCK_SIZE;
	iomap->bdev   = sb->s_bdev;
	iomap->flags  = 0;

	if (iblock < FTRFS_DIRECT_BLOCKS) {
		/* --- Direct block --- */
		phys = le64_to_cpu(fi->i_direct[iblock]);
		if (phys) {
			iomap->type = IOMAP_MAPPED;
			iomap->addr = phys << FTRFS_BLOCK_SHIFT;
			return 0;
		}
		if (!(flags & IOMAP_WRITE)) {
			iomap->type = IOMAP_HOLE;
			iomap->addr = IOMAP_NULL_ADDR;
			return 0;
		}
		new_block = ftrfs_alloc_block(sb);
		if (!new_block) {
			pr_err("ftrfs: iomap: no free blocks\n");
			return -ENOSPC;
		}
		fi->i_direct[iblock] = cpu_to_le64(new_block);
		mark_inode_dirty(inode);
		iomap->type = IOMAP_MAPPED;
		iomap->addr = new_block << FTRFS_BLOCK_SHIFT;
		return 0;
	}

	if (iblock < FTRFS_DIRECT_BLOCKS + FTRFS_INDIRECT_PTRS) {
		/* --- Single indirect block --- */
		u64 indirect_slot = iblock - FTRFS_DIRECT_BLOCKS;
		u64 indirect_blk  = le64_to_cpu(fi->i_indirect);
		struct buffer_head *ibh;
		__le64 *ptrs;

		if (!indirect_blk) {
			if (!(flags & IOMAP_WRITE)) {
				iomap->type = IOMAP_HOLE;
				iomap->addr = IOMAP_NULL_ADDR;
				return 0;
			}
			indirect_blk = ftrfs_alloc_block(sb);
			if (!indirect_blk) {
				pr_err("ftrfs: iomap: no free blocks for indirect\n");
				return -ENOSPC;
			}
			ibh = sb_getblk(sb, indirect_blk);
			if (!ibh) {
				ftrfs_free_block(sb, indirect_blk);
				return -EIO;
			}
			lock_buffer(ibh);
			memset(ibh->b_data, 0, FTRFS_BLOCK_SIZE);
			set_buffer_uptodate(ibh);
			unlock_buffer(ibh);
			mark_buffer_dirty(ibh);
			brelse(ibh);
			fi->i_indirect = cpu_to_le64(indirect_blk);
			mark_inode_dirty(inode);
		}

		ibh = sb_bread(sb, indirect_blk);
		if (!ibh)
			return -EIO;
		ptrs = (__le64 *)ibh->b_data;
		phys = le64_to_cpu(ptrs[indirect_slot]);

		if (phys) {
			brelse(ibh);
			iomap->type = IOMAP_MAPPED;
			iomap->addr = phys << FTRFS_BLOCK_SHIFT;
			return 0;
		}
		if (!(flags & IOMAP_WRITE)) {
			brelse(ibh);
			iomap->type = IOMAP_HOLE;
			iomap->addr = IOMAP_NULL_ADDR;
			return 0;
		}
		new_block = ftrfs_alloc_block(sb);
		if (!new_block) {
			brelse(ibh);
			pr_err("ftrfs: iomap: no free blocks\n");
			return -ENOSPC;
		}
		ptrs[indirect_slot] = cpu_to_le64(new_block);
		mark_buffer_dirty(ibh);
		brelse(ibh);
		iomap->type = IOMAP_MAPPED;
		iomap->addr = new_block << FTRFS_BLOCK_SHIFT;
		return 0;
	}

	/* Beyond single indirect: not supported in v1 */
	pr_err_ratelimited("ftrfs: iomap: offset beyond indirect blocks\n");
	return -EOPNOTSUPP;
}

static int ftrfs_iomap_end(struct inode *inode, loff_t pos, loff_t length,
			   ssize_t written, unsigned int flags,
			   struct iomap *iomap)
{
	return 0;
}

const struct iomap_ops ftrfs_iomap_ops = {
	.iomap_begin = ftrfs_iomap_begin,
	.iomap_end   = ftrfs_iomap_end,
};

/*
 * Write path — ftrfs_iomap_write_ops
 * get_folio/put_folio use generic helpers (no journaling required).
 */
static struct folio *ftrfs_iomap_get_folio(struct iomap_iter *iter,
					   loff_t pos, unsigned int len)
{
	return iomap_get_folio(iter, pos, len);
}

static void ftrfs_iomap_put_folio(struct inode *inode, loff_t pos,
				  unsigned int copied, struct folio *folio)
{
	folio_unlock(folio);
	folio_put(folio);
}

static const struct iomap_write_ops ftrfs_iomap_write_ops = {
	.get_folio = ftrfs_iomap_get_folio,
	.put_folio = ftrfs_iomap_put_folio,
};

static ssize_t ftrfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	return iomap_file_buffered_write(iocb, from, &ftrfs_iomap_ops,
					 &ftrfs_iomap_write_ops, NULL);
}

/*
 * Writeback path — ftrfs_writeback_ops
 */
static ssize_t ftrfs_writeback_range(struct iomap_writepage_ctx *wpc,
				     struct folio *folio, u64 offset,
				     unsigned int len, u64 end_pos)
{
	if (offset < wpc->iomap.offset ||
	    offset >= wpc->iomap.offset + wpc->iomap.length) {
		int ret;

		memset(&wpc->iomap, 0, sizeof(wpc->iomap));
		ret = ftrfs_iomap_begin(wpc->inode,
					offset, INT_MAX, 0,
					&wpc->iomap, NULL);
		if (ret)
			return ret;
	}
	return iomap_add_to_ioend(wpc, folio, offset, end_pos, len);
}

static const struct iomap_writeback_ops ftrfs_writeback_ops = {
	.writeback_range  = ftrfs_writeback_range,
	.writeback_submit = iomap_ioend_writeback_submit,
};

static int ftrfs_writepages(struct address_space *mapping,
			    struct writeback_control *wbc)
{
	struct iomap_writepage_ctx wpc = {
		.inode = mapping->host,
		.wbc   = wbc,
		.ops   = &ftrfs_writeback_ops,
	};

	return iomap_writepages(&wpc);
}

/*
 * Read path — uses iomap_bio_read_ops (kernel-provided)
 */
static int ftrfs_read_folio(struct file *file, struct folio *folio)
{
	struct iomap_read_folio_ctx ctx = {
		.ops       = &iomap_bio_read_ops,
		.cur_folio = folio,
	};

	iomap_read_folio(&ftrfs_iomap_ops, &ctx, NULL);
	return 0;
}

static void ftrfs_readahead(struct readahead_control *rac)
{
	struct iomap_read_folio_ctx ctx = {
		.ops = &iomap_bio_read_ops,
		.rac = rac,
	};

	iomap_readahead(&ftrfs_iomap_ops, &ctx, NULL);
}

static sector_t ftrfs_bmap(struct address_space *mapping, sector_t block)
{
	return iomap_bmap(mapping, block, &ftrfs_iomap_ops);
}

const struct address_space_operations ftrfs_aops = {
	.read_folio       = ftrfs_read_folio,
	.readahead        = ftrfs_readahead,
	.writepages       = ftrfs_writepages,
	.bmap             = ftrfs_bmap,
	.dirty_folio      = iomap_dirty_folio,
	.invalidate_folio = iomap_invalidate_folio,
	.release_folio    = iomap_release_folio,
	.migrate_folio    = filemap_migrate_folio,
};

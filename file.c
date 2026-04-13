// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTRFS — File operations (skeleton)
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * NOTE: read/write use generic_file_* for now.
 * The EDAC/RS layer will intercept at the block I/O level (next iteration).
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include "ftrfs.h"

const struct file_operations ftrfs_file_operations = {
	.llseek         = generic_file_llseek,
	.read_iter      = generic_file_read_iter,
	.write_iter     = generic_file_write_iter,
	.mmap           = generic_file_mmap,
	.fsync          = generic_file_fsync,
	.splice_read    = filemap_splice_read,
};

const struct inode_operations ftrfs_file_inode_operations = {
	.getattr        = simple_getattr,
};

/*
 * ftrfs_get_block — map logical block number to physical block
 *
 * Supports direct blocks only for now. Indirect blocks planned for v2.
 */
static int ftrfs_get_block(struct inode *inode, sector_t iblock,
			    struct buffer_head *bh_result, int create)
{
	struct ftrfs_inode_info *fi = FTRFS_I(inode);
	__le64 phys;

	if (iblock >= FTRFS_DIRECT_BLOCKS) {
		pr_err("ftrfs: indirect block access not yet supported\n");
		return -EOPNOTSUPP;
	}

	phys = fi->i_direct[iblock];
	if (!phys)
		return 0;

	map_bh(bh_result, inode->i_sb, le64_to_cpu(phys));
	return 0;
}

static int ftrfs_read_folio(struct file *file, struct folio *folio)
{
	return block_read_full_folio(folio, ftrfs_get_block);
}

static int ftrfs_writepages(struct address_space *mapping,
			    struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, ftrfs_get_block);
}


static void ftrfs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, ftrfs_get_block);
}

static int ftrfs_write_begin(const struct kiocb *iocb,
			     struct address_space *mapping,
			     loff_t pos, unsigned len,
			     struct folio **foliop, void **fsdata)
{
	return block_write_begin(mapping, pos, len, foliop, ftrfs_get_block);
}

static int ftrfs_write_end(const struct kiocb *iocb,
			   struct address_space *mapping,
			   loff_t pos, unsigned len, unsigned copied,
			   struct folio *folio, void *fsdata)
{
	return generic_write_end(iocb, mapping, pos, len, copied, folio, fsdata);
}

static sector_t ftrfs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, ftrfs_get_block);
}
const struct address_space_operations ftrfs_aops = {
	.read_folio     = ftrfs_read_folio,
	.readahead      = ftrfs_readahead,
	.write_begin    = ftrfs_write_begin,
	.write_end      = ftrfs_write_end,
	.writepages     = ftrfs_writepages,
	.bmap           = ftrfs_bmap,
	.dirty_folio    = block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
};

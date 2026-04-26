/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FTRFS — Fault-Tolerant Radiation-Robust Filesystem
 * Based on: Fuchs, Langer, Trinitis — ARCS 2015
 *
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#ifndef _FTRFS_H
#define _FTRFS_H

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/types.h>

/* inode_state_read_once returns inode_state_flags in kernel 7.0 */
#define ftrfs_inode_is_new(inode) \
	(inode_state_read_once(inode) & I_NEW)

/* Magic number: 'FTRF' */
#define FTRFS_MAGIC         0x46545246

/* Block size: 4096 bytes */
#define FTRFS_BLOCK_SIZE    4096
#define FTRFS_BLOCK_SHIFT   12

/* RS FEC: 16 parity bytes per 239-byte subblock (RS(255,239)) */
#define FTRFS_RS_PARITY     16
#define FTRFS_INODE_RS_DATA offsetof(struct ftrfs_inode, i_reserved)  /* 172 bytes */
#define FTRFS_INODE_RS_PAR  16  /* parity bytes stored in i_reserved[0..15] */
#define FTRFS_SUBBLOCK_DATA 239
#define FTRFS_SUBBLOCK_TOTAL (FTRFS_SUBBLOCK_DATA + FTRFS_RS_PARITY)

/* On-disk bitmap block layout (RS FEC protected) */
#define FTRFS_BITMAP_SUBBLOCKS  16   /* subblocks per bitmap block */
#define FTRFS_BITMAP_DATA_BYTES (FTRFS_BITMAP_SUBBLOCKS * FTRFS_SUBBLOCK_DATA) /* 3824 */
#define FTRFS_BITMAP_MAX_BLOCKS (FTRFS_BITMAP_DATA_BYTES * 8) /* 30592 */

/* Filesystem limits */
#define FTRFS_MAX_FILENAME  255
#define FTRFS_DIRECT_BLOCKS 12
#define FTRFS_INDIRECT_BLOCKS 1
#define FTRFS_DINDIRECT_BLOCKS 1

/*
 * Radiation Event Journal entry — 24 bytes
 * Records each RS FEC correction event persistently in the superblock.
 * 64 entries give operators a map of physical degradation over time.
 * No existing Linux filesystem provides this at the block layer.
 */
struct ftrfs_rs_event {
	__le64  re_block_no;    /* corrected block number */
	__le64  re_timestamp;   /* nanoseconds since boot  */
	__le32  re_error_bits;  /* number of symbols corrected */
	__le32  re_crc32;       /* CRC32 of this entry */
} __packed;                 /* 24 bytes */

#define FTRFS_RS_JOURNAL_SIZE  64   /* entries in the radiation event journal */

/*
 * On-disk format versions
 *   v2 -- bitmap RS FEC (introduced 2026-04-17)
 *   v3 -- extension points: feature bitmaps + data protection scheme
 */
#define FTRFS_VERSION_V2        2
#define FTRFS_VERSION_V3        3
#define FTRFS_VERSION_CURRENT   FTRFS_VERSION_V3

/*
 * Data protection scheme values for s_data_protection_scheme.
 *
 * NONE              -- no FEC on data blocks (legacy behaviour, deprecated).
 * INODE_OPT_IN      -- RS FEC enabled per-inode via FTRFS_INODE_FL_RS_ENABLED.
 *                     This is the v0.1.0 baseline behaviour. Deprecated by
 *                     threat model 6.3 (see Documentation/threat-model.md).
 * UNIVERSAL_INLINE  -- RS parity bytes embedded inline within each data block.
 *                     Reserved for stage 4 of the staged plan.
 * UNIVERSAL_SHADOW  -- RS parity stored in a dedicated out-of-band region.
 *                     Reserved for stage 4 of the staged plan.
 * UNIVERSAL_EXTENT  -- RS parity attached as an extent-based filesystem
 *                     attribute. Reserved for stage 4 of the staged plan.
 *
 * The kernel range-checks this field at mount and refuses values above
 * FTRFS_DATA_PROTECTION_MAX. Three unused upper bytes of the __le32 act
 * as a structural sentinel: any single-byte corruption in the high-order
 * bytes produces a value outside the valid range and is rejected.
 */
#define FTRFS_DATA_PROTECTION_NONE              0
#define FTRFS_DATA_PROTECTION_INODE_OPT_IN      1
#define FTRFS_DATA_PROTECTION_UNIVERSAL_INLINE  2
#define FTRFS_DATA_PROTECTION_UNIVERSAL_SHADOW  3
#define FTRFS_DATA_PROTECTION_UNIVERSAL_EXTENT  4
#define FTRFS_DATA_PROTECTION_INODE_UNIVERSAL   5
#define FTRFS_DATA_PROTECTION_MAX               FTRFS_DATA_PROTECTION_INODE_UNIVERSAL

/*
 * Feature flag masks.
 *
 * s_feat_compat     -- informational flags. Unknown bits are logged but
 *                     do not prevent mount.
 * s_feat_incompat   -- structural format extensions. Unknown bits cause
 *                     mount to be refused (read or write).
 * s_feat_ro_compat  -- features that prevent safe write. Unknown bits cause
 *                     mount to be forced read-only with a warning.
 *
 * In FTRFS_VERSION_V3 no feature bits are allocated; all three masks are
 * zero. Future features will allocate bits and update the corresponding
 * SUPP mask.
 */
#define FTRFS_FEAT_COMPAT_SUPP      0ULL
#define FTRFS_FEAT_INCOMPAT_SUPP    0ULL
#define FTRFS_FEAT_RO_COMPAT_SUPP   0ULL

/*
 * On-disk superblock — block 0
 * Total size: fits in one 4096-byte block
 */
struct ftrfs_super_block {
	__le32  s_magic;            /* FTRFS_MAGIC */
	__le32  s_block_size;       /* Block size in bytes */
	__le64  s_block_count;      /* Total blocks */
	__le64  s_free_blocks;      /* Free blocks */
	__le64  s_inode_count;      /* Total inodes */
	__le64  s_free_inodes;      /* Free inodes */
	__le64  s_inode_table_blk;  /* Block where inode table starts */
	__le64  s_data_start_blk;   /* First data block */
	__le32  s_version;          /* Filesystem version */
	__le32  s_flags;            /* Flags */
	__le32  s_crc32;            /* CRC32 of this superblock */
	__u8    s_uuid[16];         /* UUID */
	__u8    s_label[32];        /* Volume label */
	 struct ftrfs_rs_event s_rs_journal[FTRFS_RS_JOURNAL_SIZE]; /* 1536 bytes */
	__u8    s_rs_journal_head;  /* next write index (ring buffer) */
	__le64  s_bitmap_blk;       /* On-disk block bitmap block number */
	__le64  s_feat_compat;      /* Compatible feature flags (informational) */
	__le64  s_feat_incompat;    /* Incompatible features: refuse mount if unknown bit set */
	__le64  s_feat_ro_compat;   /* RO-compat features: force RO mount if unknown bit set */
	__le32  s_data_protection_scheme; /* enum FTRFS_DATA_PROTECTION_* */
	__u8    s_pad[2407];        /* Padding to 4096 bytes */
} __packed;

/*
 * On-disk inode
 * Size: 256 bytes
 *
 * Addressing capacity:
 *   direct  (12)  =              48 KiB
 *   indirect (1)  =               2 MiB
 *   dindirect (1) =               1 GiB
 *   tindirect (1) =             512 GiB
 *
 * uid/gid: __le32 to support uid > 65535 (standard kernel convention)
 * timestamps: __le64 nanoseconds (required for space mission precision)
 */
struct ftrfs_inode {
	__le16  i_mode;             /* File mode */
	__le16  i_nlink;            /* Hard link count */
	__le32  i_uid;              /* Owner UID */
	__le32  i_gid;              /* Owner GID */
	__le64  i_size;             /* File size in bytes (64-bit, future-proof) */
	__le64  i_atime;            /* Access time (ns) */
	__le64  i_mtime;            /* Modification time (ns) */
	__le64  i_ctime;            /* Change time (ns) */
	__le32  i_flags;            /* Inode flags */
	__le32  i_crc32;            /* CRC32 of inode (excluding this field) */
	__le64  i_direct[FTRFS_DIRECT_BLOCKS];    /* Direct block pointers */
	__le64  i_indirect;         /* Single indirect (~2 MiB) */
	__le64  i_dindirect;        /* Double indirect (~1 GiB) */
	__le64  i_tindirect;        /* Triple indirect (~512 GiB) */
	__u8    i_reserved[84];     /* Padding to 256 bytes */
} __packed;

/*
 * Inode flags.
 *
 * FTRFS_INODE_FL_RS_ENABLED: deprecated as of stage 3 (v0.3.0+).
 *   Was the per-inode opt-in for RS FEC under the
 *   FTRFS_DATA_PROTECTION_INODE_OPT_IN scheme. Stage 3 replaces
 *   that scheme with FTRFS_DATA_PROTECTION_INODE_UNIVERSAL, where
 *   all inodes are RS-protected unconditionally. The flag is
 *   preserved in the bit definition so v0.1.0/v0.2.0 images that
 *   set it remain mountable; new images do not set it.
 *   See Documentation/threat-model.md section 6.1 and 6.3.
 */
#define FTRFS_INODE_FL_RS_ENABLED   0x0001  /* deprecated, see comment */
#define FTRFS_INODE_FL_VERIFIED     0x0002  /* Integrity verified */

/*
 * On-disk directory entry
 */
struct ftrfs_dir_entry {
	__le64  d_ino;              /* Inode number */
	__le16  d_rec_len;          /* Record length */
	__u8    d_name_len;         /* Name length */
	__u8    d_file_type;        /* File type */
	char    d_name[FTRFS_MAX_FILENAME + 1]; /* Filename */
} __packed;

/*
 * In-memory superblock info (stored in sb->s_fs_info)
 */
struct ftrfs_sb_info {
	/* Block allocator */
	unsigned long    *s_block_bitmap;  /* In-memory free block bitmap */
	unsigned long     s_nblocks;       /* Number of data blocks */
	unsigned long     s_data_start;    /* First data block number */
	/* Inode allocator */
	unsigned long    *s_inode_bitmap;  /* In-memory free inode bitmap */
	unsigned long     s_ninodes;       /* Total number of inodes */
	/* Superblock */
	struct ftrfs_super_block *s_ftrfs_sb; /* On-disk superblock copy */
	struct buffer_head       *s_sbh;      /* Buffer head for superblock */
	struct buffer_head       *s_bitmap_blkh; /* Buffer head for on-disk bitmap */
	spinlock_t                s_lock;     /* Superblock lock */
	unsigned long             s_free_blocks;
	unsigned long             s_free_inodes;
};

/*
 * In-memory inode info (embedded in VFS inode via container_of)
 */
struct ftrfs_inode_info {
	__le64          i_direct[FTRFS_DIRECT_BLOCKS];
	__le64          i_indirect;
	__le64          i_dindirect;
	__le64          i_tindirect;
	__u32           i_flags;
	struct inode    vfs_inode;  /* Must be last */
};

static inline struct ftrfs_inode_info *FTRFS_I(struct inode *inode)
{
	return container_of(inode, struct ftrfs_inode_info, vfs_inode);
}

static inline struct ftrfs_sb_info *FTRFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

/* Function prototypes */
/* super.c */
int ftrfs_fill_super(struct super_block *sb, struct fs_context *fc);
void ftrfs_log_rs_event(struct super_block *sb, u64 block_no, u32 err_bits);
void ftrfs_dirty_super(struct ftrfs_sb_info *sbi);

/* inode.c */
struct inode *ftrfs_iget(struct super_block *sb, unsigned long ino);
struct inode *ftrfs_new_inode(struct inode *dir, umode_t mode);

/* dir.c */
extern const struct file_operations ftrfs_dir_operations;
extern const struct inode_operations ftrfs_dir_inode_operations;

/* file.c */
extern const struct file_operations ftrfs_file_operations;
extern const struct inode_operations ftrfs_file_inode_operations;
extern const struct address_space_operations ftrfs_aops;

/* edac.c */
void ftrfs_rs_init_tables(void);
void ftrfs_rs_exit_tables(void);
__u32 ftrfs_crc32(const void *buf, size_t len);
__u32 ftrfs_crc32_sb(const struct ftrfs_super_block *fsb);
int ftrfs_rs_encode(u8 *data, size_t len, u8 *parity);
int ftrfs_rs_decode(u8 *data, size_t len, u8 *parity);

/*
 * Encode/decode N RS(255,239) shortened subblocks across a
 * region. data and parity may live in the same buffer
 * (interleaved layout, like the bitmap) or in two separate
 * buffers (contiguous-parity layout, like the superblock).
 * The strides decouple the two cases.
 */
int ftrfs_rs_encode_region(u8 *data_buf, size_t data_stride,
			   u8 *parity_buf, size_t parity_stride,
			   size_t data_len, unsigned int n_subblocks);
int ftrfs_rs_decode_region(u8 *data_buf, size_t data_stride,
			   u8 *parity_buf, size_t parity_stride,
			   size_t data_len, unsigned int n_subblocks,
			   int *results);

/* alloc.c */
int  ftrfs_setup_bitmap(struct super_block *sb);
int  ftrfs_write_bitmap(struct super_block *sb);
void ftrfs_destroy_bitmap(struct super_block *sb);
u64  ftrfs_alloc_block(struct super_block *sb);
void ftrfs_free_block(struct super_block *sb, u64 block);
u64  ftrfs_alloc_inode_num(struct super_block *sb);
void ftrfs_free_inode_num(struct super_block *sb, u64 ino);

/* dir.c */
struct dentry *ftrfs_lookup(struct inode *dir, struct dentry *dentry,
			    unsigned int flags);

/* namei.c */
int ftrfs_write_inode(struct inode *inode, struct writeback_control *wbc);
int ftrfs_write_inode_raw(struct inode *inode);

#endif /* _FTRFS_H */

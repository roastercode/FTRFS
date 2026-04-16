# FTRFS On-Disk Format Specification

## Overview

FTRFS uses a simple, flat layout designed for auditability and certification.
All multi-byte fields are little-endian. All structures are `__packed`.
Structure sizes are enforced at compile time by `BUILD_BUG_ON`.

---

## Block Layout

```
Block 0            superblock (4096 bytes, magic 0x46545246)
Block 1..N         inode table (256 bytes per inode)
Block N+1..end     data blocks (4096 bytes each)
```

`N` is determined at format time by `mkfs.ftrfs` based on the requested
inode count. The default is 64 inodes (1 inode table block).

---

## Superblock (block 0)

```c
struct ftrfs_super_block {
    __le32  s_magic;            /* 0x46545246 ('FTRF') */
    __le32  s_block_size;       /* always 4096 */
    __le64  s_block_count;      /* total blocks on device */
    __le64  s_free_blocks;      /* free data blocks */
    __le64  s_inode_count;      /* total inodes */
    __le64  s_free_inodes;      /* free inodes */
    __le64  s_inode_table_blk;  /* first block of inode table */
    __le64  s_data_start_blk;   /* first data block */
    __le32  s_version;          /* filesystem version */
    __le32  s_flags;            /* reserved */
    __le32  s_crc32;            /* CRC32 over meaningful fields */
    __u8    s_uuid[16];         /* volume UUID */
    __u8    s_label[32];        /* volume label */
    struct ftrfs_rs_event
            s_rs_journal[64];   /* Radiation Event Journal (1536 bytes) */
    __u8    s_rs_journal_head;  /* next write index (ring buffer, 0..63) */
    __u8    s_pad[2443];        /* padding to 4096 bytes */
} __packed;
```

`BUILD_BUG_ON(sizeof(struct ftrfs_super_block) != 4096)` is enforced at
module init.

### Superblock CRC32

`s_crc32` is computed by `ftrfs_crc32_sb()` over two non-contiguous regions:

- `[0, offsetof(s_crc32))` — 64 bytes (fields before the checksum)
- `[offsetof(s_uuid), offsetof(s_pad))` — 1585 bytes (UUID, label, RS journal)

Total coverage: 1649 bytes. The padding `s_pad` is excluded. The two regions
are chained via `crc32_le()` without intermediate XOR, matching the standard
CRC-32/ISO-HDLC convention (`seed = 0xFFFFFFFF`, final XOR `0xFFFFFFFF`).

---

## Inode (256 bytes)

```c
struct ftrfs_inode {
    __le16  i_mode;             /* file mode (S_IFREG, S_IFDIR, ...) */
    __le16  i_nlink;            /* hard link count */
    __le32  i_uid;              /* owner UID (__le32, supports uid > 65535) */
    __le32  i_gid;              /* owner GID */
    __le64  i_size;             /* file size in bytes */
    __le64  i_atime;            /* access time, nanoseconds since epoch */
    __le64  i_mtime;            /* modification time, nanoseconds */
    __le64  i_ctime;            /* change time, nanoseconds */
    __le32  i_flags;            /* FTRFS_INODE_FL_* */
    __le32  i_crc32;            /* CRC32 over [0, offsetof(i_crc32)) */
    __le64  i_direct[12];       /* direct block pointers */
    __le64  i_indirect;         /* single indirect (~2 MiB) */
    __le64  i_dindirect;        /* double indirect (~1 GiB) */
    __le64  i_tindirect;        /* triple indirect (~512 GiB) */
    __u8    i_reserved[84];     /* RS FEC parity in [0..15], zero in [16..83] */
} __packed;
```

`BUILD_BUG_ON(sizeof(struct ftrfs_inode) != 256)` is enforced at module init.

### Inode CRC32

`i_crc32` covers the 168 bytes from offset 0 to `offsetof(i_crc32)`.
Verified on every `ftrfs_iget()`, updated on every `ftrfs_write_inode_raw()`.

### Inode RS FEC

When `FTRFS_INODE_FL_RS_ENABLED` is set in `i_flags`:

- **Protected data**: 172 bytes (`offsetof(i_reserved)`) — one RS(255,239)
  subblock
- **Parity**: 16 bytes stored in `i_reserved[0..15]`
- **Zeroed region**: `i_reserved[16..83]` is always zero (anti-slack,
  no hidden data)
- **Encode**: `ftrfs_write_inode_raw()` calls `ftrfs_rs_encode()` before
  writing
- **Decode**: `ftrfs_iget()` calls `ftrfs_rs_decode()` before CRC32
  verification, corrects in place if errors are found

The RS codec is `lib/reed_solomon` (`encode_rs8` / `decode_rs8`),
initialized with `init_rs(8, 0x187, 0, 1, 16)`.

---

## Directory Entry

```c
struct ftrfs_dir_entry {
    __le64  d_ino;              /* inode number */
    __le16  d_rec_len;          /* record length */
    __u8    d_name_len;         /* filename length (max 255) */
    __u8    d_file_type;        /* DT_REG=1, DT_DIR=4, ... */
    char    d_name[256];        /* filename, NUL-terminated */
} __packed;
```

Directory entries are stored in direct data blocks of the directory inode.
A zeroed `d_ino` marks a free slot. The `.` and `..` entries are stored
on disk and skipped during `ftrfs_readdir()` (emitted by `dir_emit_dots()`).

---

## Radiation Event Journal

The journal is a persistent ring buffer of 64 entries embedded in the
superblock. Each entry records one RS correction event:

```c
struct ftrfs_rs_event {
    __le64  re_block_no;    /* absolute block number where correction occurred */
    __le64  re_timestamp;   /* ktime_get_ns() at time of correction */
    __le32  re_error_bits;  /* number of symbols corrected (0..8) */
    __le32  re_crc32;       /* CRC32 of this entry over [0, offsetof(re_crc32)) */
} __packed;                 /* 24 bytes */
```

`s_rs_journal_head` is the index of the next entry to write (0..63).
Writes are serialized under `sbi->s_lock` (spinlock). The superblock
buffer is marked dirty after each write.

This journal is distinct from and complementary to VxWorks HRFS
(crash consistency), btrfs scrub (userspace tool), and NVMe SMART
(controller-level). No existing Linux filesystem provides this
filesystem-level radiation event history.

---

## Addressing Capacity

| Level      | Pointers | Block size | Capacity  |
|------------|----------|------------|-----------|
| direct     | 12       | 4 KiB      | 48 KiB    |
| indirect   | 1        | 512 ptrs   | ~2 MiB    |
| dindirect  | 1        | 512²       | ~1 GiB    |
| tindirect  | 1        | 512³       | ~512 GiB  |

Indirect blocks are not yet implemented. The current implementation
supports direct blocks only (48 KiB per file maximum in this version).

---

## Known Limitations (current version)

- No on-disk bitmap: free block/inode state is derived from superblock
  counters at mount time and maintained in memory. A dedicated bitmap block
  is planned.
- No indirect block support: files are limited to 48 KiB (12 direct blocks).
- No journaling: crash consistency relies on `mark_buffer_dirty()` ordering.
  This is acceptable for the SEU correction use case (no power-loss scenario
  assumed for MRAM targets) but is a known gap for general embedded use.
- No xattr/ACL support.
- `RENAME_EXCHANGE` and `RENAME_WHITEOUT` return `-EINVAL`.

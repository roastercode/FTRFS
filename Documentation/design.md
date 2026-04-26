# FTRFS On-Disk Format Specification

## Overview

FTRFS uses a simple, flat layout designed for auditability and certification.
All multi-byte fields are little-endian. All structures are `__packed`.
Structure sizes are enforced at compile time by `BUILD_BUG_ON`.

---

## Block Layout (v3)

The block layout is unchanged from v2. Only the superblock structure
gained extension fields (see below); all block positions and the
RS FEC bitmap layout are identical.

```
Block 0            superblock (4096 bytes, magic 0x46545246)
Block 1..N         inode table (256 bytes per inode)
Block N+1          bitmap block (RS FEC protected)
Block N+2          root directory data
Block N+3..end     data blocks (4096 bytes each)
```

`N` is determined at format time by `mkfs.ftrfs` based on the requested
inode count. The default is 4 inode table blocks (64 inodes).

For the default configuration:
```
Block 0      superblock
Block 1-4    inode table (4 blocks × 16 inodes/block = 64 inodes)
Block 5      bitmap block (RS FEC protected)
Block 6      root directory data
Block 7+     data blocks
```

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
    __le32  s_version;          /* filesystem version (3 = format extension points) */
    __le32  s_flags;            /* reserved */
    __le32  s_crc32;            /* CRC32 over meaningful fields */
    __u8    s_uuid[16];         /* volume UUID */
    __u8    s_label[32];        /* volume label */
    struct ftrfs_rs_event
            s_rs_journal[64];   /* Radiation Event Journal (1536 bytes) */
    __u8    s_rs_journal_head;  /* next write index (ring buffer, 0..63) */
    __le64  s_bitmap_blk;       /* on-disk block bitmap block number */
    __le64  s_feat_compat;      /* informational features (v3+) */
    __le64  s_feat_incompat;    /* incompatible features (v3+) */
    __le64  s_feat_ro_compat;   /* RO-compat features (v3+) */
    __le32  s_data_protection_scheme; /* enum, see below (v3+) */
    __u8    s_pad[2407];        /* padding to 4096 bytes */
} __packed;
```

`BUILD_BUG_ON(sizeof(struct ftrfs_super_block) != 4096)` is enforced at
module init.

### Superblock CRC32

`s_crc32` is computed by `ftrfs_crc32_sb()` over two non-contiguous regions:

- `[0, offsetof(s_crc32))` -- 64 bytes (fields before the checksum)
- `[offsetof(s_uuid), offsetof(s_pad))` -- 1621 bytes (UUID, label, RS journal,
  s_rs_journal_head, s_bitmap_blk, and the four v3 feature fields)

Total coverage: 1685 bytes. The padding `s_pad` is excluded. The two regions
are chained via `crc32_le()` without intermediate XOR, matching the standard
CRC-32/ISO-HDLC convention (`seed = 0xFFFFFFFF`, final XOR `0xFFFFFFFF`).

Both the kernel (`ftrfs_crc32_sb()` in `edac.c`) and `mkfs.ftrfs` use
`[68, 1689)` as the second region, covering all v3 fields up to but
excluding `s_pad`.

A v2 superblock read by a v3 kernel fails this CRC32 check (the kernel
covers 28 more bytes than v2 wrote into the parity computation), and is
correctly rejected at mount time. There is no version-detection logic
beyond this; the CRC32 mismatch is the version barrier.

---

## Format versions

| Version | Tag           | Introduced                                | Notes |
|---------|---------------|-------------------------------------------|-------|
| 2       | (pre-v0.1.0)  | 2026-04-17 (commit fd371f3)               | On-disk bitmap with RS FEC. |
| 3       | v0.2.0+       | 2026-04-26 (commit 2ec4cb4 + follow-ups)  | Adds extension points: feature bitmaps and `s_data_protection_scheme`. Block layout unchanged. |

Mounting a v2 image on a v3 kernel is refused at the CRC32 check (see
"Superblock CRC32" above). Mounting a v3 image on a v2 kernel is
similarly refused for the same reason. There is no in-place migration
tool; v2 images that need to be carried forward must be reformatted
after data evacuation.

---

## Feature flags

Three independent 64-bit bitmaps, all introduced in v3 and all set to
zero in the v3 baseline. Future features will allocate bits.

| Field               | Semantics if an unknown bit is set                          |
|---------------------|-------------------------------------------------------------|
| `s_feat_compat`     | Informational `pr_info` only; does not gate mount.          |
| `s_feat_incompat`   | Mount is refused (read or write).                           |
| `s_feat_ro_compat`  | Mount is forced read-only with a `pr_warn`.                 |

The corresponding "supported" masks `FTRFS_FEAT_*_SUPP` are defined in
`ftrfs.h`. They are all `0ULL` in v3 and grow as features land.

The `s_feat_ro_compat` graceful-degradation behaviour is deliberate
and aligned with the threat model (`Documentation/threat-model.md`
section 5): for long-unattended and mission-critical deployments,
fail-stop on a feature mismatch is itself a mission failure. A v3+
kernel encountering an unknown ro_compat bit on a v3+ image preserves
read access (telemetry, last-known-good logs, autonomous-erase
triggers) instead of refusing to mount.

---

## Data protection schemes

`s_data_protection_scheme` is an `__le32` enum that records which
data-block protection scheme the format was written with. The kernel
range-checks it at mount and refuses values above
`FTRFS_DATA_PROTECTION_MAX`.

| Value | Symbol                                  | Meaning |
|-------|------------------------------------------|---------|
| 0     | `FTRFS_DATA_PROTECTION_NONE`             | No FEC on data blocks (legacy mode, deprecated). |
| 1     | `FTRFS_DATA_PROTECTION_INODE_OPT_IN`     | RS FEC enabled per-inode via `FTRFS_INODE_FL_RS_ENABLED`. v0.1.0 baseline behaviour. Deprecated by threat model 6.3. |
| 2     | `FTRFS_DATA_PROTECTION_UNIVERSAL_INLINE` | (Reserved) RS parity bytes embedded inline within each data block. |
| 3     | `FTRFS_DATA_PROTECTION_UNIVERSAL_SHADOW` | (Reserved) RS parity stored in a dedicated out-of-band region. |
| 4     | `FTRFS_DATA_PROTECTION_UNIVERSAL_EXTENT` | (Reserved) RS parity attached as an extent-based filesystem attribute. |

The choice of `__le32` (4 bytes) over `__u8` (1 byte) is a structural
sentinel: any single-byte SEU on the three high-order zero bytes
produces a value above the enum maximum and is rejected by the
mount-time range check.

In v3, mkfs writes value 1 (`INODE_OPT_IN`). Stages 3 and 4 of the
staged plan (see `Documentation/roadmap.md`) introduce the
universal schemes.

---

## Bitmap Block (block N+1)

The bitmap block stores the free-block allocation state, protected by
Reed-Solomon FEC. It is read and decoded at mount time by `ftrfs_setup_bitmap()`.

### Layout

The 4096-byte bitmap block is divided into 16 subblocks of 255 bytes each:

```
[data0..238][parity0..15][data239..477][parity..] ... (16 subblocks)
```

Each subblock:
- 239 bytes of bitmap data
- 16 bytes of RS(255,239) parity

Total bitmap capacity: 16 × 239 × 8 = 30,592 bits (blocks addressable).

### RS FEC parameters

```c
init_rs(8, 0x187, fcr=0, prim=1, nroots=16)
```

- GF(2^8), primitive polynomial 0x187
- `fcr=0`: generator polynomial roots are alpha^0..alpha^15
- 16 parity bytes per subblock
- Corrects up to 8 symbol errors per subblock

`mkfs.ftrfs` encodes parity using the same GF arithmetic and generator
polynomial as `codec_init()` in `lib/reed_solomon/reed_solomon.c`, and
the same LFSR feedback loop as `encode_rs.c`. Parity is verified
byte-for-byte against the kernel at validation time.

### Mount-time behavior

`ftrfs_setup_bitmap()`:
1. Reads bitmap block from `s_bitmap_blk`
2. Decodes each subblock via `ftrfs_rs_decode()` (calls `decode_rs8`)
3. If corrections made: logs event to Radiation Event Journal,
   writes corrected bitmap back immediately
4. Copies bitmap data into in-memory `sbi->s_block_bitmap`

`ftrfs_write_bitmap()` re-encodes RS parity and marks the bitmap buffer
dirty on every alloc/free.

---

## Inode (256 bytes)

```c
struct ftrfs_inode {
    __le16  i_mode;             /* file mode */
    __le16  i_nlink;            /* hard link count */
    __le32  i_uid;
    __le32  i_gid;
    __le64  i_size;
    __le64  i_atime;
    __le64  i_mtime;
    __le64  i_ctime;
    __le32  i_flags;            /* FTRFS_INODE_FL_* */
    __le32  i_crc32;            /* CRC32 over [0, offsetof(i_crc32)) */
    __le64  i_direct[12];       /* direct block pointers (48 KiB max) */
    __le64  i_indirect;         /* single indirect (~2 MiB, planned) */
    __le64  i_dindirect;        /* double indirect (~1 GiB, planned) */
    __le64  i_tindirect;        /* triple indirect (~512 GiB, planned) */
    __u8    i_reserved[84];     /* RS parity in [0..15], zero in [16..83] */
} __packed;
```

`BUILD_BUG_ON(sizeof(struct ftrfs_inode) != 256)` enforced at module init.

### Inode CRC32

Covers `[0, offsetof(i_crc32))` = 168 bytes.
Verified on every `ftrfs_iget()`, updated on every `ftrfs_write_inode_raw()`.

### Inode RS FEC

When `FTRFS_INODE_FL_RS_ENABLED` is set:
- **Protected**: 172 bytes (`offsetof(i_reserved)`)
- **Parity**: 16 bytes in `i_reserved[0..15]`
- **Zeroed**: `i_reserved[16..83]` always zero

---

## Directory Entry

```c
struct ftrfs_dir_entry {
    __le64  d_ino;
    __le16  d_rec_len;
    __u8    d_name_len;
    __u8    d_file_type;        /* DT_REG=1, DT_DIR=4, ... */
    char    d_name[256];
} __packed;
```

A zeroed `d_ino` marks a free slot. `.` and `..` are stored on disk and
emitted by `dir_emit_dots()` during readdir.

---

## Radiation Event Journal

64-entry ring buffer embedded in the superblock. Each entry:

```c
struct ftrfs_rs_event {
    __le64  re_block_no;    /* absolute block number */
    __le64  re_timestamp;   /* ktime_get_ns() */
    __le32  re_error_bits;  /* symbols corrected (0..8) */
    __le32  re_crc32;       /* CRC32 of this entry */
} __packed;                 /* 24 bytes */
```

`s_rs_journal_head` is the next write index (0..63). Writes serialized
under `sbi->s_lock`. Superblock buffer marked dirty after each write.

---

## Known Limitations (current version)

- No indirect block support: files limited to 48 KiB (12 direct blocks)
- No journaling: crash consistency relies on `mark_buffer_dirty()` ordering
- No xattr/ACL support
- `RENAME_EXCHANGE` and `RENAME_WHITEOUT` return `-EINVAL`
- `SB_RDONLY` not yet checked before superblock writes

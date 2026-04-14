# FTRFS — Fault-Tolerant Radiation-Robust Filesystem

FTRFS is a Linux kernel filesystem designed for dependable storage in
radiation-intensive environments. It provides per-block CRC32 checksumming
and Reed-Solomon forward error correction (FEC), targeting embedded Linux
systems operating in space, avionics, or other harsh conditions.

This implementation is an independent open-source realization of the design
described in:

> Fuchs, C.M., Langer, M., Trinitis, C. (2015).
> *FTRFS: A Fault-Tolerant Radiation-Robust Filesystem for Space Use.*
> ARCS 2015, Lecture Notes in Computer Science, vol 9017. Springer.
> DOI: https://doi.org/10.1007/978-3-319-16086-3_8
> Full text (no paywall): https://www.cfuchs.net/chris/publication-list/ARCS2015/FTRFS.pdf

The original design was developed at TU Munich (Institute for Astronautics)
in the context of the MOVE-II CubeSat mission.

## Why FTRFS exists

FTRFS does not compete with ext4 or btrfs for general-purpose use.

ext4 checksums **detect** corruption. fsverity **detects** tampering on
read-only data. VxWorks HRFS protects against **crash consistency**. None of
these correct silent bit flips in data at rest without an external redundant
copy.

On embedded space hardware with a single MRAM or NOR flash device, there is
no block layer redundancy available. Reed-Solomon FEC integrated at the
filesystem block level is the only mechanism that can **recover** corrupted
data in place on a single-device system. That is the specific problem FTRFS
addresses.

Additionally, FTRFS targets certification environments (DO-178C, ECSS-E-ST-40C,
IEC 61508) where code auditability is a hard requirement. No existing Linux
filesystem — ext4 (~100k lines), btrfs (~200k lines) — can realistically be
certified under these frameworks. FTRFS is designed to stay under 5000 lines
of auditable code.

## AI Tooling Disclosure

This implementation was developed with Claude (Anthropic) as a coding
assistant, in compliance with the Linux kernel AI coding policy
(Documentation/process/coding-assistants.rst, merged with Linux 7.0).

The human submitter takes full responsibility for every line of code,
has reviewed, tested, and debugged it on real hardware (arm64 KVM,
kernel 7.0 final), and certifies the DCO accordingly.

v3 patches will carry the formal attribution tag:

    Assisted-by: Claude:claude-sonnet-4-6

## On-disk Layout

```
Block 0        : superblock (magic 0x46545246, CRC32, 4096 bytes)
Block 1..N     : inode table (256 bytes/inode, CRC32 per inode)
Block N+1..end : data blocks (CRC32 + RS FEC per block)
```

## Inode Design (v2 — 256 bytes)

```
Addressing capacity:
  direct   (12) =     48 KiB
  indirect  (1) =      2 MiB
  dindirect (1) =      1 GiB
  tindirect (1) =    512 GiB   (new in v2)

uid/gid:     __le32  (supports uid > 65535)
timestamps:  __le64  (nanosecond precision)
i_size:      __le64  (future-proof for growing MRAM densities)
i_reserved:  84 bytes (reserved for future extensions)
```

BUILD_BUG_ON enforces sizeof(ftrfs_inode) == 256 and
sizeof(ftrfs_super_block) == 4096 at compile time.

## Status

| Feature                        | Status              | Version |
|-------------------------------|---------------------|---------|
| Superblock mount/umount        | ✅ implemented      | v1      |
| Inode read (CRC32)             | ✅ implemented      | v1      |
| Directory read/lookup          | ✅ implemented      | v1      |
| File read (generic)            | ✅ implemented      | v1      |
| Block/inode allocator          | ✅ implemented      | v1      |
| Reed-Solomon FEC encoder       | ✅ implemented      | v1      |
| address_space_operations       | ✅ implemented      | v2      |
| Write path (create/mkdir)      | ✅ implemented      | v2      |
| Triple indirect (~512 GiB)     | ✅ implemented      | v2      |
| uid/gid __le32                 | ✅ implemented      | v2      |
| insert_inode_locked fix        | ✅ implemented      | v2      |
| mkfs.ftrfs (BLKGETSIZE64)      | ✅ implemented      | v2      |
| 0 BUG/WARN/Oops kernel 7.0    | ✅ validated        | v2      |
| Assisted-by tag (DCO)         | ✅ declared         | v2      |
| iomap IO path                  | ✅ implemented      | v3      |
| rename                         | ✅ implemented      | v3      |
| RS FEC decoder                 | ✅ implemented      | v3      |
| Radiation Event Journal        | ✅ implemented      | v3      |
| kthread scrubber (RT)          | 🔧 planned          | v4      |
| xfstests equivalent            | ✅ validated        | v3      |

## Roadmap

### v3 (next)

**iomap IO path** — ✅ Implemented. Replaced buffer_head based read/write
with iomap API (kernel 7.0). Implements ftrfs_iomap_begin/end, iomap_writeback_ops
with Berlekamp-Massey writeback_range, and iomap_bio_read_ops for read path.
buffer_head retained for metadata IO (inode table, directory blocks) as per
Wilcox review scope.

**rename** — ✅ Implemented `ftrfs_rename` in `namei.c`. Handles same-dir and cross-dir rename for files and directories. RENAME_EXCHANGE and RENAME_WHITEOUT not supported (returns -EINVAL).

**RS FEC decoder** — ✅ Implemented full RS(255,239) decoder in `edac.c`.
Berlekamp-Massey error locator polynomial, Chien search for error positions,
Forney algorithm for in-place magnitude correction. Corrects up to 8 symbol
errors per 255-byte subblock. Returns -EBADMSG if uncorrectable.

**Radiation Event Journal** — ✅ Implemented. Persistent ring buffer of
64 entries × 24 bytes in the superblock reserved area:

```c
struct ftrfs_rs_event {
    __le64  re_block_no;    /* corrected block number */
    __le64  re_timestamp;   /* nanoseconds since boot  */
    __le32  re_error_bits;  /* number of symbols corrected */
    __le32  re_crc32;       /* CRC32 of this entry */
} __packed;                 /* 24 bytes per entry */
```

ftrfs_log_rs_event() writes under spinlock and marks the superblock
buffer dirty for persistence. sizeof(ftrfs_super_block) == 4096
enforced by BUILD_BUG_ON. No existing Linux filesystem provides this
filesystem-level radiation event history — VxWorks HRFS, btrfs scrub,
and NVMe SMART all operate at different layers.

**xfstests** — ✅ Manual equivalent of generic/001, 002, 010 validated on
qemuarm64 kernel 7.0. Full xfstests Yocto recipe planned for v4.

### v4 (future)

**kthread scrubber with RT priority** — A kernel thread that periodically
walks all blocks, verifies CRC32, triggers RS correction if needed, and
updates the Radiation Event Journal. Configurable via sysfs:
`/sys/fs/ftrfs/<dev>/scrub_interval_ms` and `scrub_rt_priority`.
Unlike btrfs scrub (a userspace tool), this runs in kernel with bounded
latency — essential for hard real-time embedded targets.

## HPC Validation

FTRFS has been validated as a data partition in an arm64 HPC cluster
running Slurm 25.11.4, built with Yocto Styhead (5.1) and deployed on
KVM/QEMU virtual machines (cortex-a57, Linux 7.0.0 final).

Cluster topology: 1 master + 3 compute nodes, each with a dedicated
FTRFS partition (64 MiB, /dev/vdb).

### Benchmark — April 14, 2026 (v2 write path)

| Test                                  | Result  |
|---------------------------------------|---------|
| Job submission latency (single node)  | 0.074s  |
| 3-node parallel job (N=3, ntasks=3)   | 0.384s  |
| 9-job throughput submission           | 0.478s  |
| FTRFS mount                           | ✅      |
| ftrfs.ko (arm64, kernel 7.0-rc7)      | ✅      |
| 0 BUG/WARN/Oops                       | ✅      |

### Benchmark — April 14, 2026 (v3 final — kernel 7.0, post-reboot clean system)

| Test                                  | Result  | vs v2   |
|---------------------------------------|---------|---------|
| Job submission latency (single node)  | 0.260s  | n/a *   |
| 3-node parallel job (N=3, ntasks=3)   | 0.343s  | -11%    |
| 9-job throughput submission           | 0.062s  | -87%    |
| rename file (mv old.txt new.txt)      | ✅      | new     |
| rename directory (cross-dir)          | ✅      | new     |
| RS FEC decode (no errors)             | ✅      | new     |
| Radiation Event Journal write         | ✅      | new     |
| iomap IO path (read/write)            | ✅      | new     |
| xfstests generic/001,002,010 equiv.   | ✅      | new     |
| 0 BUG/WARN/Oops                       | ✅      | —       |

(*) v2 baseline was measured on kernel 7.0-rc7 with a different Slurm/network
configuration (192.168.57.x). v3 uses corrected 192.168.56.x subnet with
arm64-* node names. Direct latency comparison is not meaningful.
QEMU TCG (software emulation, no KVM) is the dominant latency factor —
results reflect emulated cortex-a57, not bare metal.

Yocto layer: https://github.com/roastercode/yocto-hardened/tree/arm64-ftrfs

## RFC Thread

RFC v1 submitted to linux-fsdevel — April 13, 2026
RFC v2 submitted to linux-fsdevel — April 14, 2026
Message-ID: `<20260413230601.525400-1-aurelien@hackers.camp>`
Archive: https://lore.kernel.org/linux-fsdevel/

Reviewers: Matthew Wilcox, Pedro Falcato, Darrick J. Wong, Andreas Dilger,
Gao Xiang.

Key feedback and responses:

- **Matthew Wilcox**: Use iomap instead of buffer_head →
  ✅ implemented in v3 (file.c)
- **Darrick J. Wong**: i_size __le64, no journaling →
  i_size __le64 is intentional (future MRAM density);
  journaling not needed for SEU correction use case
- **Andreas Dilger**: ext4+fsverity covers the need →
  ext4 detects, does not correct; no existing FS addresses
  in-place RS correction on single-device embedded systems
- **Gao Xiang**: AI tooling concern →
  disclosed per coding-assistants.rst; human reviewed,
  tested, and signed; FTRFS implements a published 2015
  academic design (TU Munich / MOVE-II CubeSat)

## Requirements

- Linux kernel 7.0 or later
- Architecture: any (tested x86_64 and arm64/qemuarm64)
- Yocto Styhead (5.1) for embedded integration

## Building

### Out-of-tree module

```sh
make
sudo insmod ftrfs.ko
```

### Cross-compilation (Yocto)

```sh
source <yocto>/oe-init-build-env <build-dir>
bitbake ftrfs-module
```

### Format and mount

```sh
gcc -o mkfs.ftrfs mkfs.ftrfs.c
dd if=/dev/zero of=test.img bs=4096 count=16384
./mkfs.ftrfs test.img
sudo insmod ftrfs.ko
sudo mount -t ftrfs test.img /mnt
```

## Source Layout

```
ftrfs/
├── Kconfig          — kernel configuration
├── Makefile         — build system
├── ftrfs.h          — on-disk and in-memory structures
├── super.c          — superblock, mount/umount, Radiation Event Journal, module init
├── inode.c          — inode operations, CRC32 verification
├── dir.c            — directory operations (readdir, lookup)
├── file.c           — file operations, iomap IO path (read/write/writeback)
├── namei.c          — create, mkdir, unlink, rmdir, link, rename, write_inode
├── alloc.c          — block and inode bitmap allocator
├── edac.c           — CRC32 checksumming, RS FEC encoder + decoder (Berlekamp-Massey)
├── mkfs.ftrfs.c     — userspace formatter
└── COPYING          — GNU General Public License v2
```

## License

GNU General Public License v2.0 only.

## Author

Aurelien DESBRIERES <aurelien@hackers.camp>

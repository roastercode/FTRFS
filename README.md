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
kernel 7.0-rc7), and certifies the DCO accordingly.

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
| iomap IO path                  | 🔧 planned          | v3      |
| rename                         | 🔧 planned          | v3      |
| RS FEC decoder                 | 🔧 planned          | v3      |
| Radiation Event Journal        | 🔧 planned          | v3      |
| kthread scrubber (RT)          | 🔧 planned          | v4      |
| xfstests run                   | 🔧 planned          | v3      |

## Roadmap

### v3 (next)

**iomap IO path** — Replace buffer_head based read/write with iomap, as
recommended by Matthew Wilcox (linux-fsdevel review). Required for upstream
consideration. ext2 in kernel 7.0 already uses iomap for DAX/DIO; buffered
IO will follow for FTRFS.

**rename** — Implement `ftrfs_rename` in `namei.c`.

**RS FEC decoder** — Complete the Reed-Solomon correction layer in `edac.c`.
The encoder is present in v2; the decoder (in-place correction of corrupted
blocks) is the critical missing piece.

**Radiation Event Journal** — A persistent ring buffer in the superblock
recording every RS correction event:

```c
struct ftrfs_rs_event {
    __le64  block_no;    /* corrected block number */
    __le64  timestamp;   /* nanoseconds since boot  */
    __le32  error_bits;  /* number of bits corrected */
    __le32  crc32;       /* integrity of this entry  */
} __packed;              /* 24 bytes per entry       */
```

64 entries × 24 bytes = 1536 bytes, fits within existing superblock
reserved space. This gives operators a persistent map of physical
degradation — which zones of MRAM/NOR Flash accumulate errors over time.
No existing Linux filesystem provides this. VxWorks HRFS, btrfs scrub,
and NVMe SMART all operate at different layers without this filesystem-level
view of radiation-induced physical corruption history.

**xfstests** — Minimal test run (generic/001, generic/002, generic/010)
before v3 submission.

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
KVM/QEMU virtual machines (cortex-a57, Linux 7.0.0-rc7).

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
  accepted, planned for v3
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
├── super.c          — superblock, mount/umount, module init
├── inode.c          — inode operations, CRC32 verification
├── dir.c            — directory operations (readdir, lookup)
├── file.c           — file operations, address_space_operations
├── namei.c          — create, mkdir, unlink, rmdir, link, write_inode
├── alloc.c          — block and inode bitmap allocator
├── edac.c           — CRC32 checksumming, RS FEC encoder
├── mkfs.ftrfs.c     — userspace formatter
└── COPYING          — GNU General Public License v2
```

## License

GNU General Public License v2.0 only.

## Author

Aurelien DESBRIERES <aurelien@hackers.camp>

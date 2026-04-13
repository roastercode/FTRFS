# FTRFS — Fault-Tolerant Radiation-Robust Filesystem

FTRFS is a Linux kernel filesystem designed for dependable storage in
radiation-intensive environments. It provides memory protection, checksumming,
and forward error correction (FEC) using Reed-Solomon codes, targeting
embedded Linux systems operating in space or other harsh conditions.

This implementation is an out-of-tree kernel module targeting Linux 7.0+.

## Background

FTRFS was originally described in:

> Fuchs, C.M., Langer, M., Trinitis, C. (2015).
> *FTRFS: A Fault-Tolerant Radiation-Robust Filesystem for Space Use.*
> In: Architecture of Computing Systems – ARCS 2015.
> Lecture Notes in Computer Science, vol 9017. Springer, Cham.
> https://doi.org/10.1007/978-3-319-16086-3_8
>
> Open-access PDF: https://www.cfuchs.net/chris/publication-list/ARCS2015/FTRFS.pdf

The filesystem was developed at the Institute for Astronautics,
Technical University Munich (TUM), in the context of the MOVE-II CubeSat
mission. This implementation is an independent open-source realization
of the concepts described in that paper.

## Design Goals

FTRFS does not compete with ext4 or btrfs for general-purpose use.
It targets embedded critical systems where:

- **Code auditability** is a hard requirement (DO-178C, ECSS-E-ST-40C, IEC 61508)
- **Silent data corruption** from single-event upsets (SEU) is the primary threat
- **No external redundancy** (RAID, backup) is available
- **Minimal footprint** is a design constraint, not a trade-off

No existing Linux filesystem can be certified under DO-178C or ECSS-E-ST-40C
due to code complexity (ext4: ~100k lines, btrfs: ~200k lines). FTRFS is
designed to stay under 5000 lines of auditable code with RS FEC as a
first-class design constraint.

## Data Integrity Layers

- **CRC32** — per-block and per-inode checksums (hardware-accelerated via crc32_le)
- **Reed-Solomon FEC** — encoder implemented, decoder planned (v2)
- **EDAC** — error detection and correction tracking

ext4 checksums detect corruption. fsverity detects tampering on read-only data.
Neither corrects silent bit flips in data at rest without an external redundant
copy. RS FEC integrated at the filesystem block level is the only mechanism
that can recover corrupted data in place on a single-device system.

## On-disk Layout

```
Block 0        : superblock (magic 0x46545246, CRC32-protected, 4096 bytes)
Block 1..N     : inode table (256 bytes/inode, CRC32 per inode)
Block N+1..end : data blocks (CRC32 + RS FEC per block)
```

## Inode Design (v2 — 256 bytes)

```
Addressing capacity:
  direct  (12) =     48 KiB
  indirect (1) =      2 MiB
  dindirect (1) =     1 GiB
  tindirect (1) =   512 GiB

uid/gid: __le32 (supports uid > 65535)
timestamps: __le64 nanoseconds (space mission precision)
i_size: __le64 (future-proof for growing MRAM densities)
```

BUILD_BUG_ON enforces sizeof(ftrfs_inode) == 256 and
sizeof(ftrfs_super_block) == 4096 at compile time.

## Status

| Feature                        | Status           |
|-------------------------------|------------------|
| Superblock mount/umount        | ✅ implemented   |
| Inode read (CRC32)             | ✅ implemented   |
| Directory read/lookup          | ✅ implemented   |
| File read (generic)            | ✅ implemented   |
| address_space_operations       | ✅ implemented   |
| Block/inode allocator          | ✅ implemented   |
| Write path (create/mkdir)      | ✅ implemented   |
| Triple indirect blocks (~512G) | ✅ implemented   |
| uid/gid __le32                 | ✅ implemented   |
| unlock_new_inode fix           | ✅ implemented   |
| Reed-Solomon FEC encoder       | ✅ implemented   |
| Reed-Solomon FEC decoder       | 🔧 v2 planned    |
| iomap-based IO path            | 🔧 v2 planned    |
| rename                         | 🔧 v2 planned    |
| xattr / SELinux                | 🔧 v2 planned    |
| fsck.ftrfs                     | 🔧 v2 planned    |
| xfstests run                   | 🔧 v2 planned    |

## v2 Roadmap

Following RFC feedback from linux-fsdevel (April 2026):

1. **iomap IO path** — replace buffer_head based read/write with iomap
   (Matthew Wilcox recommendation, required for upstream consideration)
2. **Reed-Solomon decoder** — complete the FEC layer for in-place correction
3. **rename** — implement VFS rename operation
4. **xfstests** — minimal test suite run before v2 submission
5. **s_hash_algo** — superblock field to select checksum algorithm
   (CRC32, SHA-256, BLAKE3) for post-quantum readiness
6. **mkfs.ftrfs** — proper userspace tool with Yocto recipe

## HPC Validation

FTRFS has been validated as a data partition in an arm64 HPC cluster
running Slurm 25.11.4, built with Yocto Styhead (5.1) and deployed
on KVM/QEMU virtual machines (cortex-a57, Linux 7.0.0-rc7).

Cluster topology: 1 master + 3 compute nodes, each with a dedicated
FTRFS partition (64 MiB, /dev/vdb, mounted at /var/tmp/ftrfs).

### Benchmark results — April 13, 2026

| Test                                  | Result  |
|---------------------------------------|---------|
| Job submission latency (single node)  | 0.074s  |
| 3-node parallel job (N=3, ntasks=3)   | 0.384s  |
| 9-job throughput submission           | 0.478s  |
| Job distribution                      | perfect |
| FTRFS mount                           | ✅      |
| ftrfs.ko load (arm64 kernel 7.0-rc7)  | ✅      |

Yocto layer: https://github.com/roastercode/yocto-hardened/tree/arm64-ftrfs

## RFC Thread

RFC submitted to linux-fsdevel — April 13, 2026.
Message-ID: <20260413142357.515792-1-aurelien@hackers.camp>
Archive: https://lore.kernel.org/linux-fsdevel/

Reviewers: Matthew Wilcox, Pedro Falcato, Darrick J. Wong, Andreas Dilger.

Key feedback:
- Use iomap instead of buffer_head (Matthew Wilcox) — accepted for v2
- i_size __le64 intentional for future MRAM density growth (Darrick J. Wong)
- FUSE prototype suggested (Andreas Dilger) — considered but deferred;
  embedded target environments do not support FUSE overhead
- btrfs+FEC suggested (Darrick J. Wong) — btrfs complexity precludes
  DO-178C/ECSS certification, which is a primary design goal

## Requirements

- Linux kernel 7.0 or later
- Architecture: any (tested on x86_64 and arm64 / qemuarm64)
- Yocto Styhead (5.1) for embedded integration

## Building

### As an out-of-tree module (against running kernel)

```sh
make
sudo insmod ftrfs.ko
```

### Cross-compilation for arm64 (Yocto environment)

```sh
source <yocto-build>/oe-init-build-env <build-dir>
bitbake ftrfs-module
```

### Format a block device or image

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
├── Kconfig          — kernel configuration entry
├── Makefile         — build system (out-of-tree + Yocto)
├── ftrfs.h          — on-disk and in-memory data structures
├── super.c          — superblock operations, mount/umount, module init
├── inode.c          — inode operations, iget with CRC32 verification
├── dir.c            — directory operations (readdir, lookup)
├── file.c           — file operations, address_space_operations
├── namei.c          — create, mkdir, unlink, rmdir, link, write_inode
├── alloc.c          — block and inode bitmap allocator
├── edac.c           — CRC32 checksumming, Reed-Solomon FEC encoder
├── mkfs.ftrfs.c     — userspace filesystem formatter
└── COPYING          — GNU General Public License v2
```

## Contributing

Patches should follow the Linux kernel coding style and be verified with:

```sh
scripts/checkpatch.pl --no-tree -f <file>
```

Submissions target the linux-fsdevel mailing list.

## License

GNU General Public License v2.0 only.
See COPYING for the full license text.

## Author

Aurelien DESBRIERES <aurelien@hackers.camp>

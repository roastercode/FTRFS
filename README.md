# FTRFS — Fault-Tolerant Radiation-Robust Filesystem

FTRFS is a Linux kernel filesystem designed for dependable storage in
radiation-intensive environments. It provides per-block CRC32 checksumming
and Reed-Solomon forward error correction (FEC) using the kernel's
`lib/reed_solomon` library, targeting embedded Linux systems on single-device
storage (MRAM, NOR flash) where no external redundancy is available.

This implementation is an independent open-source realization of the design
described in:

> Fuchs, C.M., Langer, M., Trinitis, C. (2015).
> *FTRFS: A Fault-Tolerant Radiation-Robust Filesystem for Space Use.*
> ARCS 2015, Lecture Notes in Computer Science, vol 9017. Springer.
> DOI: https://doi.org/10.1007/978-3-319-16086-3_8
> Full text: https://www.cfuchs.net/chris/publication-list/ARCS2015/FTRFS.pdf

The original design was developed at TU Munich (Institute for Astronautics)
in the context of the MOVE-II CubeSat mission.

---

## Why FTRFS

FTRFS does not compete with ext4 or btrfs for general-purpose use.

**The problem it solves:** on a single MRAM or NOR flash device in a radiation
environment, a single-event upset (SEU) silently flips bits in data at rest.
ext4 checksums detect this corruption but cannot correct it. There is no
redundant copy to fall back to.

FTRFS integrates Reed-Solomon FEC at the filesystem block level so that
corrupted data can be corrected in place, on a single device, without
operator intervention.

A secondary constraint is code auditability. Standards such as DO-178C
(avionics), ECSS-E-ST-40C (space), and IEC 61508 (nuclear/industrial) require
complete auditability of safety-critical software. ext4 (~100k lines) and
btrfs (~200k lines) are not realistically certifiable under these frameworks.
FTRFS is designed to stay under 5000 lines of auditable code.

---

## Current Status

Out-of-tree module, actively maintained and validated on arm64 kernel 7.0.
RFC v3 submitted to linux-fsdevel (April 2026), incorporating review feedback
before v4 resubmission.

| Feature                                      | Status         |
|----------------------------------------------|----------------|
| Superblock mount/umount                      | ✅ implemented |
| Inode read/write with CRC32                  | ✅ implemented |
| RS FEC on inode metadata                     | ✅ implemented |
| Directory read/lookup/create                 | ✅ implemented |
| File read/write (iomap path)                 | ✅ implemented |
| Block and inode allocator                    | ✅ implemented |
| RS FEC encode/decode (lib/reed_solomon)      | ✅ implemented |
| Radiation Event Journal                      | ✅ implemented |
| rename (same-dir and cross-dir)              | ✅ implemented |
| On-disk bitmap block with RS FEC (v2)        | ✅ implemented |
| mkfs parity matches lib/reed_solomon         | ✅ validated   |
| mkfs -N <inodes> option                      | ✅ implemented |
| Single indirect block (~2 MiB per file)      | ✅ implemented |
| Data block free on delete (evict_inode)      | ✅ implemented |
| inode bitmap consistency at remount          | ✅ fixed       |
| evict_inode: zero i_mode on disk             | ✅ fixed       |
| ftrfs_reconfigure() for remount support      | ✅ fixed       |
| migrate_folio in ftrfs_aops                  | ✅ fixed       |
| readdir d_off unique per entry               | ✅ fixed       |
| checkpatch.pl --strict: 0 issues             | ✅ validated   |
| arm64 KVM, kernel 7.0, 0 BUG/WARN           | ✅ validated   |
| Slurm HPC cluster validation (4 nodes)       | ✅ validated   |
| xfstests generic/002, 010, 098, 257          | ✅ PASS        |
| xfstests generic/001                         | needs >2 GiB test image |
| kthread scrubber (RT priority)               | 🔧 planned     |
| Double/triple indirect blocks                | 🔧 planned     |

---

## On-disk Layout (v2)

```
Block 0        superblock (magic 0x46545246, 4096 bytes, CRC32 verified)
Block 1..N     inode table (256 bytes/inode, configurable via mkfs -N)
Block N+1      bitmap block (RS FEC protected — 16 subblocks RS(255,239))
Block N+2      root directory data
Block N+3..end data blocks
```

Default: `mkfs.ftrfs -N 256` → 16 inode table blocks, bitmap at block 17,
data start at block 19.

File addressing:
- 12 direct blocks = 48 KiB
- 1 single indirect block = 512 × 4 KiB = 2 MiB
- Total per file: ~2 MiB (v1)

---

## xfstests Results (2026-04-18, arm64 kernel 7.0)

| Test | Result | Notes |
|------|--------|-------|
| generic/002 | ✅ PASS | file create/delete |
| generic/010 | ✅ PASS | dbm — needs indirect blocks |
| generic/098 | ✅ PASS | pwrite at offset > 48 KiB |
| generic/257 | ✅ PASS | directory d_off uniqueness |
| generic/001 | env limit | needs >2 GiB test image (not a FTRFS bug) |

Zero BUG/WARN/Oops/inconsistency in dmesg across all tests.

---

## HPC Validation

Validated as a data partition in an arm64 Slurm 25.11.4 cluster built
with Yocto Styhead (5.1), deployed on KVM/QEMU (cortex-a57, Linux 7.0.0).
Cluster: 1 master + 3 compute nodes, each with FTRFS on `/data`.

| Test                             | Result   |
|----------------------------------|----------|
| Job submission latency           | ~0.25s   |
| 3-node parallel job              | 0.34s    |
| 9-job batch throughput           | 4.37s    |
| FTRFS mount (4 nodes)            | zero RS errors ✅ |
| FTRFS write from Slurm job       | ✅       |
| 0 BUG/WARN/Oops                  | ✅       |

Yocto layer: https://github.com/roastercode/yocto-hardened/tree/arm64-ftrfs

---

## Reed-Solomon Implementation

FTRFS uses the kernel's `lib/reed_solomon` library (`encode_rs8`, `decode_rs8`).
No custom Galois Field arithmetic is implemented in the kernel module.
The codec is initialized once at module load:

```c
init_rs(8, 0x187, 0, 1, 16)
// GF(2^8), primitive polynomial 0x187
// fcr=0: roots alpha^0..alpha^15
// 16 parity bytes per 239-byte subblock
// Corrects up to 8 symbol errors per subblock
```

---

## Requirements

- Linux kernel 7.0 or later
- `CONFIG_REED_SOLOMON=y` (selected automatically by Kconfig)
- Tested: x86_64 (build), arm64/qemuarm64 (runtime)
- Yocto Styhead (5.1) for embedded integration

---

## Building

### Out-of-tree module

```sh
make KERNEL_SRC=/lib/modules/$(uname -r)/build
sudo insmod ftrfs.ko
```

### Cross-compilation (Yocto)

```sh
source <yocto>/oe-init-build-env <build-dir>
bitbake ftrfs-module
bitbake mkfs-ftrfs
```

### Format and mount

```sh
gcc -o mkfs.ftrfs mkfs.ftrfs.c
dd if=/dev/zero of=test.img bs=4096 count=16384
./mkfs.ftrfs -N 256 test.img
sudo insmod ftrfs.ko
sudo mount -t ftrfs test.img /mnt
```

---

## RFC Thread

RFC v3 submitted to linux-fsdevel, April 14, 2026.
Message-ID: `<20260414120726.5713-1-aurelien@hackers.camp>`

Active reviewers: Matthew Wilcox, Pedro Falcato, Darrick J. Wong,
Andreas Dilger, Eric Biggers, Gao Xiang.

Status: incorporating review feedback. Next submission (v4) planned after
indirect block support (done), xfstests coverage, and Eric Biggers response.

---

## AI Tooling Disclosure

Developed with Claude (Anthropic) as a coding assistant, per
`Documentation/process/coding-assistants.rst` (Linux 7.0).
The submitter takes full responsibility for all code and has reviewed,
tested, and debugged every patch on real hardware.

Commit attribution: `Assisted-by: Claude:claude-sonnet-4-6`

---

## License

GNU General Public License v2.0 only.

## Author

Aurelien DESBRIERES `<aurelien@hackers.camp>`

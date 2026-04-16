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

This is an out-of-tree module, actively maintained and validated on arm64
kernel 7.0. It has been submitted as an RFC to linux-fsdevel (April 2026)
and is currently incorporating review feedback before a future resubmission.

| Feature                             | Status         |
|-------------------------------------|----------------|
| Superblock mount/umount             | ✅ implemented |
| Inode read/write with CRC32         | ✅ implemented |
| RS FEC on inode metadata            | ✅ implemented |
| Directory read/lookup/create        | ✅ implemented |
| File read/write (iomap path)        | ✅ implemented |
| Block and inode allocator           | ✅ implemented |
| RS FEC encode/decode (lib/reed_solomon) | ✅ implemented |
| Radiation Event Journal             | ✅ implemented |
| rename (same-dir and cross-dir)     | ✅ implemented |
| checkpatch.pl --strict: 0 issues    | ✅ validated   |
| arm64 KVM, kernel 7.0, 0 BUG/WARN  | ✅ validated   |
| Slurm HPC cluster validation        | ✅ validated   |
| On-disk bitmap allocator            | 🔧 planned     |
| kthread scrubber (RT priority)      | 🔧 planned     |
| xfstests recipe (Yocto)             | 🔧 planned     |

---

## On-disk Layout

```
Block 0        : superblock (magic 0x46545246, 4096 bytes)
Block 1..N     : inode table (256 bytes/inode)
Block N+1..end : data blocks
```

The superblock contains a persistent Radiation Event Journal: a ring buffer
of 64 entries recording each RS correction event (block number, timestamp,
symbols corrected, per-entry CRC32).

The inode is 256 bytes. The first 172 bytes are protected by a RS(255,239)
codeword; parity is stored in `i_reserved[0..15]`. A per-inode CRC32 covers
the full inode.

See [Documentation/design.md](Documentation/design.md) for the complete
on-disk format specification.

---

## Reed-Solomon Implementation

FTRFS uses the kernel's `lib/reed_solomon` library (`rslib.h`, `encode_rs8`,
`decode_rs8`). No custom Galois Field arithmetic is implemented. The codec
is initialized once at module load via `init_rs(8, 0x187, 0, 1, 16)`:

- Symbol size: 8 bits (GF(2^8))
- Primitive polynomial: 0x187
- Parity symbols: 16 (corrects up to 8 symbol errors per subblock)
- Data per subblock: 239 bytes
- Codeword: 255 bytes (RS(255,239))

---

## HPC Validation

FTRFS has been validated as a data partition in an arm64 Slurm 25.11.4
cluster built with Yocto Styhead (5.1), deployed on KVM/QEMU (cortex-a57,
Linux 7.0.0). Cluster: 1 master + 3 compute nodes, each with a 64 MiB
FTRFS partition on `/dev/vdb`.

| Test                             | Result  |
|----------------------------------|---------|
| Job submission latency           | 0.378s  |
| 3-node parallel job              | 0.336s  |
| 9-job throughput                 | 0.052s  |
| 0 BUG/WARN/Oops                  | ✅      |

Yocto layer: https://github.com/roastercode/yocto-hardened/tree/arm64-ftrfs

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
```

### Format and mount

```sh
gcc -o mkfs.ftrfs mkfs.ftrfs.c
dd if=/dev/zero of=test.img bs=4096 count=16384
./mkfs.ftrfs test.img
sudo insmod ftrfs.ko
sudo mount -t ftrfs test.img /mnt
```

---

## Source Layout

```
ftrfs/
├── Kconfig           kernel configuration (selects REED_SOLOMON)
├── Makefile          build system
├── ftrfs.h           on-disk and in-memory structures
├── super.c           superblock, mount/umount, Radiation Event Journal
├── inode.c           inode read, CRC32 + RS FEC verification
├── dir.c             directory operations (readdir, lookup)
├── file.c            file operations, iomap IO path
├── namei.c           create, mkdir, unlink, rmdir, link, rename
├── alloc.c           block and inode bitmap allocator
├── edac.c            CRC32, RS FEC via lib/reed_solomon
├── mkfs.ftrfs.c      userspace formatter
├── Documentation/
│   ├── design.md     on-disk format specification
│   ├── testing.md    test procedure and results
│   └── roadmap.md    what is done, what remains
└── COPYING           GNU General Public License v2
```

---

## RFC Thread

RFC v3 submitted to linux-fsdevel, April 14, 2026.
Message-ID: `<20260414120726.5713-1-aurelien@hackers.camp>`

Active reviewers: Matthew Wilcox, Pedro Falcato, Darrick J. Wong,
Andreas Dilger, Eric Biggers, Gao Xiang.

Status: incorporating review feedback. Next submission planned after
xfstests integration and on-disk bitmap allocator.

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

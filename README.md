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

The filesystem was developed at the Institute for Astronautics,
Technical University Munich (TUM), in the context of the MOVE-II CubeSat
mission. This implementation is an independent open-source realization
of the concepts described in that paper.

## Design

FTRFS targets POSIX-compatible block devices (MRAM, NOR flash, eMMC)
and provides three layers of data integrity:

- **CRC32** — per-block and per-inode checksums (hardware-accelerated)
- **Reed-Solomon FEC** — forward error correction per data subblock
- **EDAC** — error detection and correction tracking

### On-disk layout

```
Block 0          : superblock (CRC32-protected)
Block 1..N       : inode table (128 bytes/inode, CRC32 per inode)
Block N+1        : root directory data
Block N+2..end   : data blocks (RS FEC + CRC32 per block)
```

### Inode structure

Each inode uses direct addressing (12 direct block pointers) plus
single and double indirection, following the classical UNIX model.
Extended attributes (xattrs) are supported for SELinux labeling.

## Status

| Feature                    | Status         |
|----------------------------|----------------|
| Superblock mount/umount    | ✅ implemented |
| Inode read (CRC32)         | ✅ implemented |
| Directory read/lookup      | ✅ implemented |
| File read (generic)        | ✅ implemented |
| CRC32 checksumming         | ✅ implemented |
| Reed-Solomon FEC           | 🔧 in progress |
| Write path                 | 🔧 in progress |
| xattr / SELinux            | 🔧 planned     |
| Bitmap allocator           | 🔧 planned     |
| fsck.ftrfs                 | 🔧 planned     |

## Requirements

- Linux kernel 7.0 or later
- Architecture: any (tested on arm64 / qemuarm64)
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
dd if=/dev/zero of=test.img bs=4096 count=1024
./mkfs.ftrfs test.img
sudo insmod ftrfs.ko
sudo mount -t ftrfs test.img /mnt
```

## Source layout

```
ftrfs/
├── Kconfig          — kernel configuration entry (planned)
├── Makefile         — build system (out-of-tree + Yocto)
├── ftrfs.h          — on-disk and in-memory data structures
├── super.c          — superblock operations, mount/umount, module init
├── inode.c          — inode operations, iget with CRC32 verification
├── dir.c            — directory operations (readdir, lookup)
├── file.c           — file operations (read/write)
├── edac.c           — CRC32 checksumming (Reed-Solomon FEC: in progress)
├── mkfs.ftrfs.c     — userspace filesystem formatter
└── COPYING          — GNU General Public License v2
```

## Contributing

Patches should follow the Linux kernel coding style and be verified with:

```sh
scripts/checkpatch.pl --no-tree -f <file>
```

Submissions target the linux-fsdevel mailing list once the write path
and Reed-Solomon layer are complete.

## License

GNU General Public License v2.0 only.
See COPYING for the full license text.

## Author

Aurélien DESBRIERES <aurelien@hackers.camp>

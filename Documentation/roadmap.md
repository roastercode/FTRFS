# FTRFS Roadmap

## What is done

### Core filesystem (v1–v3)

- Superblock mount/umount with CRC32 verification
- Inode table read/write with per-inode CRC32
- RS FEC on inode metadata via `lib/reed_solomon` (encode + decode)
- Directory operations: readdir, lookup, create, mkdir, unlink, rmdir,
  link, rename (same-dir and cross-dir)
- File read/write via iomap API (kernel 7.0)
- Block and inode allocator (in-memory bitmap)
- `mkfs.ftrfs` userspace formatter
- Radiation Event Journal in superblock (64-entry ring buffer, per-entry CRC32)
- `MODULE_SOFTDEP("pre: reed_solomon")` for automatic dependency loading
- `checkpatch.pl --strict`: 0 errors, 0 warnings, 0 checks on all files
- Validated: arm64 KVM, kernel 7.0, Yocto Styhead 5.1, Slurm 25.11.4

### On-disk bitmap block with RS FEC (v2) — DONE 2026-04-17

The bitmap block (block 5) stores the free-block allocation state in
16 subblocks of 239 data bytes each, protected by 16 bytes of RS(255,239)
parity. The kernel reads, verifies, and corrects the bitmap at mount time.
`mkfs.ftrfs` encodes parity using the same GF arithmetic as `lib/reed_solomon`,
verified byte-for-byte. No existing Linux filesystem applies RS FEC to its
own allocation metadata.

Validated on arm64 QEMU kernel 7.0:
```
ftrfs: bitmaps initialized (16377 data blocks, 16377 free; 64 inodes, 63 free)
ftrfs: mounted — zero RS errors
```

### RFC submission

- RFC v1, v2, v3 submitted to linux-fsdevel (April 2026)
- Active review by Wilcox, Falcato, Wong, Dilger, Biggers, Gao Xiang
- lib/reed_solomon migration completed (Eric Biggers review)
- iomap IO path completed (Matthew Wilcox review)
- Use case documented, out-of-tree deployment demonstrated in Slurm HPC cluster

---

## What remains before next submission (v4)

### Must have

**xfstests Yocto recipe**
A reproducible xfstests run replacing the current manual functional test
sequence. Target: `generic/001, 002, 010, 098, 257` at minimum.
Required by all reviewers before any kernel acceptance.

**Indirect block support**
Files are currently limited to 48 KiB (12 direct blocks). Single indirect
block support (~2 MiB) is needed for realistic embedded use cases and
directly addresses Pedro Falcato's NACK on use case scope.

**Respond to Eric Biggers**
Confirm that `lib/reed_solomon` is used exactly — `encode_rs8`/`decode_rs8`
with `init_rs(8, 0x187, 0, 1, 16)` — and that mkfs parity matches kernel
byte-for-byte. This is now validated and documented.

**Patch series restructure**
The v3 series accumulated fixup patches that need to be rebased into a
clean atomic series before resubmission.

### Nice to have

**kthread scrubber**
A kernel thread that periodically walks all data blocks, verifies CRC32,
triggers RS correction, and updates the Radiation Event Journal.
Configurable via sysfs: `/sys/fs/ftrfs/<dev>/scrub_interval_ms`.
Unlike btrfs scrub (userspace), this runs in kernel with bounded
latency — relevant for hard real-time embedded targets.

**RS FEC on data blocks at writeback**
Currently RS FEC is applied to inode metadata and bitmap only. Applying
it to data blocks at writeback time would complete the original FTRFS design.

**Shannon entropy in RS journal**
A per-correction entropy estimate in `ftrfs_rs_event` to distinguish
SEU-induced corrections (random bit patterns) from systematic device
degradation (high-entropy bursts).

**SB_RDONLY enforcement**
`SB_RDONLY` flag is not yet checked before writes to the superblock.
Required for correct read-only mount behavior.

---

## FTRFS v2 — long-term vision (after v1 merged)

### Read-only mode with compression

A read-only mount mode combining RS FEC verification with optional
block-level compression (lz4, zstd). Positions FTRFS as an alternative
to squashfs for radiation-exposed read-only partitions — squashfs detects
nothing, FTRFS read-only mode would detect and correct.

### Extended attributes and SELinux enforcing

SELinux enforcing requires `security.selinux` xattr on every inode.
FTRFS with xattr would be the first Linux filesystem providing both
RS FEC correction and mandatory access control enforcement simultaneously.

### Symlinks and rootfs capability

Required for rootfs use. Combined with indirect blocks, xattr, and
SB_RDONLY enforcement, FTRFS would become suitable as a root filesystem.
Target architecture:

```
/     FTRFS read-only (RS FEC + compression + SELinux enforcing)
      replaces squashfs + dm-verity
/data FTRFS read-write (RS FEC + xattr + SELinux enforcing)
      replaces ext4
```

Realistic timeline: 18-24 months after v1 merge.

### Post-quantum metadata authentication

For environments requiring authenticated metadata integrity against an
active adversary, a future extension could add per-block signatures
(NIST FIPS 205 SLH-DSA / SPHINCS+) to the superblock and inode
structures. This is explicitly out of scope for v1 and v2.

---

## Timeline

No fixed timeline. Priority is correctness and test coverage over speed.

Next submission to linux-fsdevel (v4) when:

1. ✅ On-disk bitmap allocator with RS FEC — DONE
2. ⬜ xfstests Yocto recipe passes target test list
3. ⬜ Indirect block support
4. ⬜ Response to all open review comments ready
5. ⬜ Patch series clean and atomic

v2 work begins after v1 is accepted into the kernel tree.

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

### RFC submission

- RFC v1, v2, v3 submitted to linux-fsdevel (April 2026)
- Active review by Wilcox, Falcato, Wong, Dilger, Biggers, Gao Xiang
- lib/reed_solomon migration completed (Eric Biggers review)
- iomap IO path completed (Matthew Wilcox review)
- use case documented, out-of-tree deployment demonstrated

---

## What remains before next submission (v4)

### Must have

**On-disk bitmap allocator with RS FEC**
The current allocator derives free block state from superblock counters
at mount time. A dedicated on-disk bitmap block is needed for correctness
after unclean shutdown. The bitmap block itself will be protected by
RS(255,239) — 16 subblocks of 239 data bytes + 16 parity bytes each —
applying the same FEC mechanism to FTRFS allocation metadata as to user
data. No existing Linux filesystem does this.

**xfstests Yocto recipe**
A reproducible xfstests run replacing the current manual functional test
sequence. Target: `generic/001, 002, 010, 098, 257` at minimum.

**Indirect block support**
Files are currently limited to 48 KiB (12 direct blocks). Single indirect
block support (~2 MiB) is needed for realistic embedded use cases.

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
Currently RS FEC is applied to inode metadata only. Applying it to
data blocks at writeback time would complete the original FTRFS design.

**Shannon entropy in RS journal**
A per-correction entropy estimate in `ftrfs_rs_event` to distinguish
SEU-induced corrections (random bit patterns) from systematic device
degradation (high-entropy bursts).

---

## FTRFS v2 — long-term vision

The v1 design targets a specific and well-defined use case: read-write
data partition FEC on single-device embedded systems. The following
directions are being considered for v2 but are explicitly out of scope
for the current kernel submission.

### Read-only mode with compression

A read-only mount mode combining RS FEC verification with optional
block-level compression (lz4, zstd). This would position FTRFS as an
alternative to squashfs for radiation-exposed read-only partitions,
adding in-place error detection that squashfs does not provide.
squashfs detects nothing — a corrupted block returns corrupt data
silently. FTRFS read-only mode would detect and, where FEC overhead
is budgeted, correct.

Dependencies: read-only mount enforcement (SB_RDONLY), compression
framework integration, fsverity-compatible hash tree for blocks that
exceed RS correction capacity.

### Extended attributes and SELinux enforcing

SELinux enforcing requires `security.selinux` xattr on every inode,
plus `listxattr`, `getxattr`, `setxattr`, `removexattr` VFS hooks.
POSIX ACLs require the same infrastructure. This would allow FTRFS
to be used as a fully SELinux-enforcing read-write filesystem,
replacing ext4 in environments where RS FEC is required alongside
mandatory access control — a combination no existing Linux filesystem
provides today.

Dependencies: xattr block allocation, xattr namespace support,
inode format extension (xattr block pointer in on-disk inode).

### Symlinks and hard links

Required for rootfs use. Currently FTRFS cannot serve as a root
filesystem because symlinks are absent. Adding symlink support
(stored inline in the inode for short targets, in a dedicated block
for long targets) and verifying hard link correctness under RS FEC
is a prerequisite for any rootfs deployment.

Dependencies: inline symlink storage, `i_reserved` field repurposing
or inode format v2.

### Full rootfs capability

The combination of indirect blocks (already planned for v4),
symlinks, xattr, and read-only mount enforcement would make FTRFS
suitable as a root filesystem. The target architecture would then be:

```
/          FTRFS read-only (RS FEC + compression + SELinux enforcing)
           replaces squashfs + dm-verity
           SEU on rootfs → in-place RS correction, no reboot required

/data      FTRFS read-write (RS FEC + xattr + SELinux enforcing)
           replaces ext4
           mission data with full MAC enforcement
```

This is a qualitative shift from the current positioning. It requires
kernel acceptance of v1 first, followed by an incremental patch series
for each feature. Realistic timeline: 18-24 months after v1 merge.

### Post-quantum metadata authentication

The current integrity model uses CRC32 (non-cryptographic) and RS FEC
(error correction, not authentication). For environments requiring
authenticated metadata integrity against an active adversary, a future
extension could add per-block HMAC or hash-based signatures
(NIST FIPS 205 SLH-DSA / SPHINCS+) to the superblock and inode
structures. This would provide quantum-resistant authenticated
integrity while keeping the RS FEC correction layer unchanged.

This is explicitly out of scope for v1 and v2. It is noted here to
acknowledge the direction of the field and ANSSI/BSI recommendations
on post-quantum cryptography for long-lived embedded systems.

---

## Timeline

There is no fixed timeline. The priority is correctness and test
coverage over speed of submission.

Next submission to linux-fsdevel (v4) will happen when:

1. On-disk bitmap allocator with RS FEC is implemented and tested
2. xfstests Yocto recipe passes the target test list
3. Patch series is clean and atomic
4. Responses to all open review comments are ready

v2 work begins after v1 is accepted into the kernel tree.

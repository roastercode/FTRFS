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

## What remains before next submission

### Must have

**On-disk bitmap allocator**
The current allocator derives free block state from superblock counters
at mount time. A dedicated on-disk bitmap block is needed for correctness
after unclean shutdown. This is the most significant gap before
resubmission.

**xfstests Yocto recipe**
A reproducible xfstests run replacing the current manual functional test
sequence. Target: `generic/001, 002, 010, 098, 257` at minimum.

**Indirect block support**
Files are currently limited to 48 KiB (12 direct blocks). Single indirect
block support (~2 MiB) is needed for realistic embedded use cases.

**Patch series restructure**
The v3 series accumulated fixup patches ("v2 fixes", "v3 fixes") that need
to be rebased into a clean atomic series before resubmission.

### Nice to have

**kthread scrubber**
A kernel thread that periodically walks all data blocks, verifies CRC32,
triggers RS correction, and updates the Radiation Event Journal.
Configurable via sysfs: `/sys/fs/ftrfs/<dev>/scrub_interval_ms` and
`scrub_rt_priority`. Unlike btrfs scrub (userspace), this runs in kernel
with bounded latency — relevant for hard real-time embedded targets.

**RS FEC on data blocks**
Currently RS FEC is applied to inode metadata only. Applying it to data
blocks at writeback time (NapFS-style, not inline in write()) would
complete the original FTRFS design.

**Shannon entropy in RS journal**
Adding a per-correction entropy estimate to `ftrfs_rs_event` would
distinguish SEU-induced corrections (random bit patterns) from systematic
device degradation (high-entropy bursts) — drawing on GuardFS (arXiv
2401.17917) approach.

---

## Timeline

There is no fixed timeline. The priority is correctness and test coverage
over speed of submission. The experience with v1–v3 demonstrated that
rapid iteration without adequate testing invites justified rejection.

Next submission to linux-fsdevel will happen when:

1. On-disk bitmap allocator is implemented and tested
2. xfstests Yocto recipe passes the target test list
3. Patch series is clean and atomic
4. Responses to all open review comments are in place

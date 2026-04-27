# FTRFS — Fault-Tolerant Radiation-Robust Filesystem

FTRFS is a Linux kernel filesystem designed for dependable storage in
radiation-intensive environments. It provides CRC32 integrity checks on
metadata (superblock, inodes, Radiation Event Journal) and Reed-Solomon
forward error correction on the on-disk allocation bitmap via the kernel's
`lib/reed_solomon` library. The design targets embedded Linux systems on
single-device storage (MRAM, NOR flash) where no external redundancy is
available. The architectural target is universal RS FEC protection of
all data blocks; the current implementation protects metadata and the
on-disk allocation bitmap, with universal data block protection as the
next major milestone (see `Documentation/threat-model.md`).

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

FTRFS integrates Reed-Solomon FEC at the filesystem block level. In the
current implementation, RS FEC protects the on-disk allocation bitmap
so that a corrupted bitmap is corrected in place at mount time without
operator intervention. Extending the same mechanism to data blocks at
writeback is the next step on the roadmap; today, data block corruption
would be detected only when RS FEC writeback is enabled in a future
release.

A secondary constraint is code auditability. Standards such as DO-178C
(avionics), ECSS-E-ST-40C (space), and IEC 61508 (nuclear/industrial) require
complete auditability of safety-critical software. ext4 (~100k lines) and
btrfs (~200k lines) are not realistically certifiable under these frameworks.
FTRFS is designed to stay under 5000 lines of auditable code.

---

## Threat Model

FTRFS addresses two distinct families of failure that share a common
technical signature — silent bit corruption in data at rest on a
single read-write storage device — but differ in their causal origin
and statistical distribution:

- **Family A** — benign single-event upsets (cosmic rays, MRAM/NOR
  retention loss, industrial radiation environments). Spatially
  uniform, Poisson-distributed.
- **Family B** — adversarial electromagnetic events (HPM, IEMI, EMP,
  RF weapons in conflict zones). Spatially correlated, burst
  distributed, may exceed per-block RS capacity without interleaving.

Both families require in-place correction on a read-write single
device, fully in-kernel, without external redundancy — a combination
not provided by any existing Linux or BSD storage component
(ext4, btrfs, ZFS, dm-verity, dm-integrity, HAMMER2, UFS2).

The full failure model, gap analysis, deployment scenarios, and
normative architectural constraints derived from this threat model
are documented in [`Documentation/threat-model.md`](Documentation/threat-model.md).
That document is normative: subsequent design decisions are evaluated
against the constraints it defines.

---

## Current Status

Out-of-tree module, actively maintained and validated on arm64 kernel 7.0.
RFC v3 submitted to linux-fsdevel (April 2026), incorporating review feedback
before v4 resubmission.

| Feature                                      | Status         |
|----------------------------------------------|----------------|
| Superblock mount/umount                      | ✅ implemented |
| Superblock RS FEC (CRC32 + RS recovery)      | ✅ implemented |
| Inode read/write with CRC32                  | ✅ implemented |
| RS FEC on inode metadata (universal, stage 3)| ✅ implemented |
| Directory read/lookup/create                 | ✅ implemented |
| File read/write (iomap path)                 | ✅ implemented |
| Block and inode allocator                    | ✅ implemented |
| RS FEC encode/decode (lib/reed_solomon)      | ✅ implemented |
| Radiation Event Journal                      | ✅ implemented |
| rename (same-dir and cross-dir)              | ✅ implemented |
| On-disk bitmap block with RS FEC (v2)        | ✅ implemented |
| Format extension points (v3)                 | ✅ implemented |
| Feature flags (compat/incompat/ro_compat)    | ✅ implemented |
| Data protection scheme (s_data_protection_scheme) | ✅ implemented |
| ftrfs_crc32_sb defined in edac.c             | ✅ fixed       |
| lib/reed_solomon API (uint8_t *data)         | ✅ fixed       |
| RS decoder return convention (symbol count)  | ✅ fixed       |
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

## On-disk Layout (v3)

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

### 2026-04-26 -- research deployment (squashfs + real FTRFS partition)

End-to-end validation on a refactored deployment that closes the
gap between the prior tmpfs-backed POC and a production-shaped
configuration:

- **Rootfs**: read-only squashfs (`hpc-arm64-research.bb`, 52 MB)
  with overlayfs-etc for runtime config writes. Replaces the
  ext4 + dm-verity stack used in earlier runs.
- **/data (FTRFS)**: real virtio block device `/dev/vdb` (64 MB),
  formatted with `mkfs.ftrfs` and mounted via
  `mount -t ftrfs /dev/vdb /data`. No more loopback file, no more
  `losetup`. The I/O path traversed by the bench is now the
  upstream-shape that the kernel.org submission will need to
  defend.
- **Dirent fix**: this is the first end-to-end run with the dirent
  slot reuse fix applied (see `Documentation/testing.md` section
  "Dirent Slot Reuse Bug"). M4 (stat bulk on 100 files) returns
  stable values for the first time, where pre-fix the directory
  scan terminated early on a zeroed dirent and stat could not
  reach all entries.

| Test                                       | Result |
|--------------------------------------------|--------|
| FTRFS module load (4 nodes)                | OK     |
| mkfs format v3 with superblock parity      | OK scheme=5 |
| FTRFS mount v3 nominal (4 nodes)           | OK zero RS recovery, zero error |
| Real partition `/dev/vdb` (not loopback)   | OK     |
| 100-file create + sync + rm reproducer     | OK zero ENOENT |
| Job submission latency                     | 0.290s (median, 0.280-0.400) |
| 3-node parallel job                        | 0.360s |
| 9-job batch throughput                     | 4.500s |
| FTRFS write from Slurm job                 | OK     |
| BUILD_BUG_ON dirent size 268               | did not fire |
| Static invariant inv5 (dirent break)       | OK     |

I/O metrics (3 compute nodes x 10 runs = 30 samples per metric):

| ID | Metric                       | Min    | Median | Max    | Stddev | Unit    |
|----|------------------------------|--------|--------|--------|--------|---------|
| M1 | Write seq + fsync (4MB)      |  4.762 |  5.000 |  5.263 |  0.178 | MB/s    |
| M2 | Read seq cold (4MB)          | 14.286 | 20.000 | 25.000 |  2.207 | MB/s    |
| M4 | Stat bulk (100 files)        |  0.140 |  0.150 |  0.170 |  0.007 | seconds |
| M5 | Small write + fsync (10x64B) | 22.000 | 24.000 | 36.000 |  3.116 | ms/file |

Reference: `yocto-hardened/Documentation/iobench-baseline-2026-04-26.md`

### 2026-04-26 -- v3 format with stage 3 item 2 (superblock RS FEC)

The v3 superblock format with stage 3 item 2 (CRC32 + RS FEC
superblock protection, kernel side) was validated end-to-end on the
same cluster configuration. mkfs writes v3 with parity, kernel mounts
v3 and exercises both the nominal CRC32 path and the RS recovery path
under injected corruption.

| Test                                       | Result        |
|--------------------------------------------|---------------|
| FTRFS module load (4 nodes)                | ✅            |
| mkfs format v3 with superblock parity      | ✅ scheme=5, feat=0/0/0 |
| FTRFS mount v3 nominal (4 nodes)           | ✅ zero RS recovery, zero error |
| FTRFS mount v3 with injected superblock corruption (single-node qemu) | ✅ RS recovery succeeded, mount continued |
| Re-mount post-recovery (single-node qemu)  | ✅ zero RS recovery, zero error |
| Job submission latency                     | 0.41s (best of 3) |
| 3-node parallel job                        | 0.55s         |
| 9-job batch throughput                     | 4.84s         |
| FTRFS write from Slurm job                 | ✅            |
| 0 BUG/WARN/Oops/uncorrectable in dmesg (4 nodes) | ✅      |

Latency figures are below the 2026-04-21 reference run
(0.26s / 0.35s / 5.41s) on single-node and 3-node metrics because
those metrics are noise-dominated on this host (see
`context-tir-de-performance.md` section 5). 9-job throughput is the
statistically robust regression indicator on `spartian-1`; the 4.84s
measurement is -16.1% relative to the loaded reference (5.77s,
2026-04-26 commit B), within the +/-20% regression band.
The host workstation was concurrently running a syzkaller fuzzing
campaign during the run.

### 2026-04-21 -- reference run (clean host)

| Test                             | Result   |
|----------------------------------|----------|
| Job submission latency           | ~0.26s   |
| 3-node parallel job              | 0.35s    |
| 9-job batch throughput           | 5.41s    |
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


## Development

Run `tools/checkpatch-precommit.sh` before committing — it runs `checkpatch.pl --strict` on staged C files and rejects the commit on any warning.

---
## RFC Thread

Submitted to `linux-fsdevel@vger.kernel.org`:

| Version | Date | Lore archive |
|---------|------|--------------|
| RFC v1 | 2026-04-13 | https://lore.kernel.org/linux-fsdevel/20260413142357.515792-1-aurelien@hackers.camp/ |
| RFC v2 | 2026-04-13 | https://lore.kernel.org/linux-fsdevel/20260413230601.525400-1-aurelien@hackers.camp/ |
| RFC v3 | 2026-04-14 | https://lore.kernel.org/linux-fsdevel/20260414120726.5713-1-aurelien@hackers.camp/ |

Reviewers who responded publicly: Matthew Wilcox, Pedro Falcato,
Darrick J. Wong, Andreas Dilger, Eric Biggers, Gao Xiang.

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

## Publications

### Technical Report v1 (April 2026)

- **HAL**: [hal-05603801v1](https://hal.science/hal-05603801) — *FTRFS: Bringing Radiation-Robust Filesystem Principles to Contemporary Linux. A Reed--Solomon, MIL-STD-882E-Aligned Implementation (Technical Report -- Version 1)*
- **License**: CC-BY 4.0
- **Source**: [`papers/2026-04-ftrfs-v1/`](./papers/2026-04-ftrfs-v1/) (LaTeX source, bibliography, dataset, build instructions)
- **Anchor commit**: `9a63468`

This report is part of a versioned publication roadmap aligned with
the project's engineering milestones. Subsequent versions (v2, v3, v4)
will follow the tags `v0.3.0-metadata-hardening`,
`v0.4.0-universal-protection`, and `v0.5.0-security-reviewed`
respectively. Each new version is deposited under the same HAL
`idHAL` (`aurelien-desbrieres`), preserving cumulative anteriority
through HAL's native versioning. Each version will additionally be
mirrored to Zenodo via a GitHub release tag, yielding a perennial
DOI independent of the HAL platform.

See `Documentation/roadmap.md` (section "Publication roadmap") for
the engineering-publication alignment table.

## Press & community coverage

- Phoronix — *FTRFS: New Fault-Tolerant File-System Proposed For Linux* (2026-04-13):
  https://www.phoronix.com/news/FTRFS-Linux-File-System
- Phoronix — *Linux 7.1 Staging* (FTRFS mention):
  https://www.phoronix.com/news/Linux-7.1-Staging
- LWN.net — *ftrfs: Fault-Tolerant Radiation-Robust Filesystem*:
  https://lwn.net/Articles/1067452/
- daily.dev:
  https://app.daily.dev/posts/ftrfs-new-fault-tolerant-file-system-proposed-for-linux-m5rbha19y
- Reddit r/filesystems:
  https://www.reddit.com/r/filesystems/comments/1skjj18/
- Reddit r/phoronix_com:
  https://www.reddit.com/r/phoronix_com/comments/1skbg7q/
- X/Twitter @phoronix:
  https://x.com/phoronix/status/2043678672775754091
- X/Twitter @jreuben1:
  https://x.com/jreuben1/status/2043912800376889429
- Telegram Linuxgram (2026-04-13):
  https://t.me/s/linuxgram?before=18454
- YouTube — Genai Linux News:
  https://www.youtube.com/watch?v=EKA93IBcCvk

## License

GNU General Public License v2.0 only.

## Author

Aurelien DESBRIERES `<aurelien@hackers.camp>`

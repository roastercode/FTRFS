# FTRFS Roadmap

This document is the staged plan for FTRFS development. It complements
two normative references and does not override them:

- `Documentation/threat-model.md` (section 6) defines the architectural
  constraints. Items in this roadmap that derive from a threat-model
  constraint cite the constraint number explicitly.
- `Documentation/known-limitations.md` records the present gap between
  the threat model and the implementation. As stages close, items move
  out of known-limitations and into the "what is done" record below.

Stages are sequenced so that each one's exit conditions are the
prerequisites of the next. Closing a stage requires the validation
chain in `context-ftrfs-validation.md` (build clean, manual functional
test, HPC benchmark within the 20% regression policy, dmesg clean on
all four cluster nodes) plus the stage-specific sanity tests.

---

## Status table

| Stage | Title                                        | Status                  | Tag                       |
|-------|----------------------------------------------|-------------------------|---------------------------|
| 1.5   | On-disk bitmap with RS FEC (v2 format)       | CLOSED 2026-04-17       | (pre-tagging)             |
| 2     | Format extension points (v3 format)          | CLOSED 2026-04-26       | `v0.2.0-format-stable`    |
| 3     | Metadata hardening                           | ACTIVE                  | (planned: `v0.3.0-*`)     |
| 4     | Universal data block protection              | PENDING                 | (planned: `v0.4.0-*`)     |
| 5     | Offensive security analysis                  | PENDING                 | (planned: `v0.5.0-*`)     |

Tag naming convention: `vMAJOR.MINOR.PATCH-codename`. Each closing
stage produces an annotated GPG-signed git tag plus a Sigstore-signed
source tarball; the bundle is committed under `releases/`. See
`releases/README.md` for the verification procedure and the identity
table.

---

## Stage 1.5 — On-disk bitmap with RS FEC (v2 format)

**Status:** CLOSED 2026-04-17.

The bitmap block (block 5 in the default layout) stores the
free-block allocation state in 16 subblocks of 239 data bytes,
each protected by 16 bytes of RS(255,239) parity. The kernel
reads, verifies, and corrects the bitmap at mount time;
`mkfs.ftrfs` encodes parity using the same GF arithmetic as
`lib/reed_solomon`, validated byte-for-byte against the kernel
encoder.

This is the first FTRFS stage that meets the threat model
constraint 6.3 partially: the allocation bitmap is now correctable,
not only detectable. The superblock and inode table remain at
detection-only at the close of this stage.

Validated on arm64 QEMU kernel 7.0; benchmark reference run
2026-04-21 (3 metrics, zero RS errors, zero kernel BUG/WARN/Oops)
recorded as the canonical clean-host performance baseline in
`context-tir-de-performance.md` section 4.1.

Predates the Sigstore signing infrastructure; no release tarball.

---

## Stage 2 — Format extension points (v3 format)

**Status:** CLOSED 2026-04-26.
**Tag:** `v0.2.0-format-stable` (commit 56b28c8, GPG-signed annotated).
**Release tarball SHA-256:**
`19a196d18f9506c8f9ff149208ee03f2c9267aac674de3f7c1ba8c225a7684f2`.

Adds forward-compatibility infrastructure to the on-disk superblock
without changing the block layout:

- `s_feat_compat`, `s_feat_incompat`, `s_feat_ro_compat` (each
  `__le64`) for compat/incompat/RO-compat feature gating.
- `s_data_protection_scheme` (`__le32` enum) recording the
  data-block protection scheme the format was written with.
  v3 baseline value is `INODE_OPT_IN` (= 1), preserving v0.1.0
  behaviour.

`ftrfs_crc32_sb` coverage extended to `[68, 1689)` so the new
fields are protected against single-bit corruption at the same
standard as the existing fields. v2 superblocks are explicitly
rejected by v3 kernels via the resulting CRC mismatch; the CRC is
the version barrier, no separate version-detection path.

Two latent build-time defects fixed in passing:

- `ftrfs_crc32_sb` declared in `ftrfs.h` since `fd371f3` and called
  from `super.c`, but never defined. Out-of-tree module builds
  failed at the link stage. Defined in commit `cdfe78b`. (Was
  known-limitations 3.6.)
- `encode_rs8` / `decode_rs8` in `lib/reed_solomon` take
  `uint8_t *data` on linux-mainline 7.0; the wrapper in `edac.c`
  was passing `uint16_t *`. Fixed in commit `867a911`. (Was
  known-limitations 3.7.)

Validated on the 4-node arm64 Slurm cluster on 2026-04-26: all 8
phases of `bin/hpc-benchmark.sh` passed, v3 mount on all 4 nodes,
distributed FTRFS write via Slurm, zero BUG/WARN/Oops. 9-job
throughput +6.7% above the 2026-04-21 reference, within the 20%
regression policy. Latencies +69-83% due to a concurrent unrelated
syzkaller campaign on the host, documented in the release note.

Sigstore bundle committed at
`releases/ftrfs-v0.2.0-format-stable.tar.gz.sigstore.json`.
Identity in the Fulcio cert: `aurelien.desbrieres@gmail.com`,
issuer `https://github.com/login/oauth`, validity 2026-04-26
00:08:01 - 00:18:01 UTC.

### Note: latent superblock writeback bug (discovered later)

A latent defect in the on-disk superblock writeback path,
present since the start of the project but masked by the
absence of mutation+remount test cycles, was discovered while
implementing stage 3 item 1 and resolved in commit ee6b6ae as
part of that work. It is recorded here under stage 2 because
the affected code (`ftrfs_log_rs_event`, the four `alloc.c`
free-counter mutators) was already present at the v0.2.0
release. Two distinct symptoms:

  - `s_crc32` was not recomputed after any superblock mutation,
    so the on-disk superblock checksum drifted out of sync. The
    next remount failed with "superblock CRC32 mismatch".
  - `s_free_blocks` and `s_free_inodes` were updated in the
    in-memory `sbi->s_ftrfs_sb` but never copied onto the
    buffer head before `mark_buffer_dirty(sbi->s_sbh)`. The
    counters were silently lost across umount/remount.

Resolution: a centralized helper `ftrfs_dirty_super(sbi)` in
`super.c` propagates the in-memory authoritative copy onto the
buffer head, refreshes `s_crc32` via `ftrfs_crc32_sb`, mirrors
the new `s_crc32` back to the in-memory copy, and marks the
buffer dirty. All five mutation sites (`ftrfs_log_rs_event` +
4 alloc paths) call the helper instead of `mark_buffer_dirty`
directly. Validated runtime in qemuarm64: counters persist
across remount, and a remount after RS correction now
succeeds.

This trace is kept per MIL-STD-882E hazard-tracking practice:
every failure mode discovered during development is recorded
with its resolution, even when the resolution lands in a later
stage.

---

## Stage 3 — Metadata hardening

**Status:** ACTIVE.

This stage closes three normative gaps recorded in
`known-limitations.md` section 2 against threat-model section 6,
plus one behavioural defect from known-limitations 3.5. Each
gap is tracked as a numbered item with its own status; items
are sequenced so that earlier ones do not depend on later ones
but later ones may consume artefacts of earlier ones.

### Item 1 — Universal inode RS protection (CLOSED 2026-04-26)

**Threat model reference:** 6.1 (no opt-in), 6.3 (correction not
just detection) for the inode case.
**Commits:**
- FTRFS `ee6b6ae ftrfs: stage 3 item 1 -- universal inode RS protection (scheme 5)`
- yocto-hardened `aaa1cca ftrfs: sync layer sources from FTRFS HEAD ee6b6ae`

A new value `FTRFS_DATA_PROTECTION_INODE_UNIVERSAL = 5` is added
to the `s_data_protection_scheme` enum (`FTRFS_DATA_PROTECTION_MAX`
bumped accordingly). mkfs writes scheme=5 going forward.
v0.1.0 / v0.2.0 images that carry scheme=1 INODE_OPT_IN remain
mountable on a stage-3 kernel; the kernel dispatches on the scheme
to decide whether the per-inode RS path is exercised. Block layout
unchanged from v3, no v4 format bump.

The legacy `FTRFS_INODE_FL_RS_ENABLED` flag is preserved as a bit
definition for backward compatibility; the kernel no longer
consults it. The macros `FTRFS_INODE_RS_DATA` (172) and
`FTRFS_INODE_RS_PAR` (16) become live: parity goes into
`i_reserved[0..15]`, the rest of `i_reserved` is forced to zero.

Read path: CRC32 verification first, RS decode only on CRC fail
when scheme is `INODE_UNIVERSAL`. After successful correction the
kernel re-verifies CRC32 against the corrected buffer, logs the
event to the Radiation Event Journal, writes the corrected inode
back to disk, and emits a `pr_warn`. This ordering avoids the
decoder on the fast path (>99 % of reads) and avoids the rare
risk of an SEU on the parity bytes inducing a false correction
on otherwise-valid data.

Write path: RS parity recomputed on every inode write after the
CRC32 field is set. Helpers `ftrfs_rs_encode` and `ftrfs_rs_decode`
in `edac.c` were generalized to accept a `size_t len` argument so
the same two functions serve the bitmap subblock path
(len = 239) and the inode path (len = 172). lib/reed_solomon
supports shortened RS codes natively.

Validated end-to-end on qemuarm64 with kernel 7.0: clean mount,
write/umount/remount cycle preserves counters, single-bit flip
on inode 2 triggers RS recovery + log event + corrected
writeback, subsequent remount succeeds, dmesg clean.

### Item 2 — Superblock RS correction (ACTIVE)

**Threat model reference:** 6.3 (correction not just detection)
for the superblock case.

The superblock today (v0.2.0+, including stage 3 item 1) has
CRC32 detection only; corruption causes mount failure. This item
adds Reed-Solomon FEC covering the same byte range as
`ftrfs_crc32_sb` (`[0, 1689)`), so a corrupted superblock is
correctable in place at mount.

#### Design

- **RS coverage:** `[0, 1689)` (mirror of CRC32 coverage).
- **Subblock decomposition:** 8 RS(255,239) shortened codewords.
  Gives 64 symbols of correction capacity total (8 per codeword),
  strictly superior to a single shortened RS code on the full
  range (8 symbols total). Coherent with the existing bitmap
  pattern. MIL-STD-882E favourable: 8 independent
  failure-correctable regions vs 1 single point.
- **Parity placement:** offset 3968 in the 4096-byte superblock
  (last 128 bytes of `s_pad`, `4096 - 8 * 16 = 3968`). Chosen
  for stability against future format evolution: new fields go
  before the parity in `s_pad`.
- **No format bump:** layout stays v3. `s_pad` was already 2407
  bytes, the 128-byte parity fits with margin.
- **Backward compat:** v0.1.0 / v0.2.0 images mount cleanly
  because the parity zone (zeros on those images) is never
  consulted as long as CRC32 passes. On those images, a
  corruption breaking CRC32 still causes a mount failure as
  before -- no regression.

#### Implementation plan (3 atomic commits A/B/C)

- **Commit A -- RS region helpers in `edac.c`.**
  Factor the "encode N subblocks of len bytes from a buffer"
  pattern into two helpers: `ftrfs_rs_encode_region(buf,
  region_len, subblock_count, parity_offset)` and
  `ftrfs_rs_decode_region(...)`. The bitmap path in `alloc.c`
  is refactored to use them. No on-disk change, no runtime
  change on the bitmap (same RS calls, just extracted into a
  function). Test: bitmap functional unchanged.

- **Commit B -- Superblock RS layout, mkfs side.**
  Constants `FTRFS_SB_RS_OFFSET = 3968` and
  `FTRFS_SB_RS_SUBBLOCKS = 8` in `ftrfs.h`. `mkfs.ftrfs.c`
  encodes 8 RS subblocks on the superblock after `crc32_sb`
  and writes the parity at offset 3968. No kernel decode yet:
  mount continues to fail on CRC fail. Test: mkfs produces a
  superblock whose parity zone is no longer zero; mount on
  fresh image still OK (CRC32 valid, RS never consulted).

- **Commit C -- Stage 3 item 2 closure: kernel decode + dirty_super.**
  `ftrfs_dirty_super` extended to encode RS parity on every
  superblock mutation (before the CRC32 recompute).
  `ftrfs_fill_super` adds, after the CRC32 check, the RS
  recovery path: on CRC fail, decode RS, re-verify CRC, log
  RS event on block 0, mark dirty for writeback. Update
  `design.md`, `known-limitations.md` (KL 6.3 second half
  marked implemented), `README.md`. Tag
  `v0.3.0-metadata-hardening` expected **after** items 3
  and 4 are also closed.

### Item 3 — Fix `ftrfs_rs_decode` return convention (PENDING)

**Reference:** known-limitations 3.5.

Today `ftrfs_rs_decode` returns 0 (success, regardless of
whether corrections occurred) or `-EBADMSG` (uncorrectable).
The symbol count produced by `decode_rs8` is dropped. As a
consequence, the bitmap correction path in
`alloc.c::ftrfs_setup_bitmap` tests `if (rc > 0)` to identify
a correction event -- that branch never matches under the
current decoder semantics.

Fix per option (1) of known-limitations 3.5: return the symbol
count on success, keep `-EBADMSG` for uncorrectable. Propagate
to all callers (bitmap subblock loop in `alloc.c`, inode RS
path in `inode.c`, superblock RS path from item 2). The new
return contract is the prerequisite for item 4 (entropy uses
the count).

### Item 4 — Shannon entropy in RS journal (PENDING)

**Threat model reference:** 6.4 (tamper-evident journal).
Reclassified as Must-have by threat-model section 8 §1.

Each `ftrfs_rs_event` records a per-correction entropy estimate.
Three placement candidates evaluated when the work begins:
- High bits of `re_error_bits` (32-bit field, only low 4 bits
  carry the symbol count which is bounded by 8). No layout
  change.
- Padding bytes adjacent to `s_rs_journal_head` in the superblock.
- A reuse of an unused field in the existing 24-byte event entry.

Entropy is the forensic discriminator between Family A
(Poisson background) and Family B (correlated burst). Computed
on the symbol pattern returned by item 3's new return contract.

### Sanity tests for stage 3 closure

In addition to the standard validation chain

### Sanity tests for stage 3 closure

In addition to the standard validation chain
(`context-ftrfs-validation.md` section 3):

- All inodes show RS-protected at mount (no flag-gated path
  remains).
- A deliberate single-bit flip on the superblock (via `dd` on a
  detached image) is corrected at mount and logged with a
  computed entropy estimate.
- A deliberate single-bit flip on an inode is corrected on the
  next read of that inode and logged.
- `ftrfs_rs_decode` returns the correct symbol count on
  injected single-symbol and multi-symbol-but-correctable
  errors. Bitmap correction events appear in the journal
  (correctness 3.5 fix verified).
- HPC benchmark passes within 20% of the 2026-04-21 reference,
  on a clean host (no concurrent syzkaller).

### Format implication

Stage 3 may or may not require a v4 format bump depending on the
chosen superblock RS layout. If the existing `s_pad` region is
sufficient, the format remains v3 and `s_data_protection_scheme`
stays at `INODE_OPT_IN` (semantic of "all inodes RS-protected,
no data-block FEC yet" rather than the legacy "per-inode
opt-in"). If a backup superblock is introduced, format bumps to
v4 and a new enum value or feature bit captures the change.

### Tag

Planned: `v0.3.0-metadata-hardening` (or a name reflecting the
final layout choice). Annotated, GPG-signed, Sigstore-signed
under the same identity as `v0.1.0-baseline` and
`v0.2.0-format-stable`.

---

## Stage 4 — Universal data block protection

**Status:** PENDING.

This stage closes threat-model constraint 6.1 (universal data
block protection, no opt-in) and 6.2 (burst tolerance through
stripe geometry).

### Scheme selection

`s_data_protection_scheme` enum already reserves the three
candidate schemes (see `Documentation/design.md` section "Data
protection schemes"):

| Value | Scheme                                  | Trade-offs |
|-------|------------------------------------------|------------|
| 2     | `UNIVERSAL_INLINE`                       | RS parity inside each data block. Simplest. Fixed overhead per block. No protection against full-block loss. |
| 3     | `UNIVERSAL_SHADOW`                       | RS parity in a dedicated out-of-band region with stride placement. Burst-tolerant if stride > burst length. Layout cost. |
| 4     | `UNIVERSAL_EXTENT`                       | RS parity as an extent-attribute. Distribution pattern per extent. Most flexible, most complex. |

Stage 4 begins with a written comparison and a chosen scheme,
recorded in a new `Documentation/data-protection-design.md`. The
choice is constrained by the LOC budget of threat-model 6.5
(under 5000 lines auditable, current 2700, available margin
~2300).

### Exit conditions

1. All stage 3 sanity checks still pass.
2. Single-block data corruption (deliberate `dd` flip) is
   auto-corrected on read; correction logged.
3. Burst corruption across multiple consecutive blocks is
   handled per the chosen scheme's stated burst tolerance.
4. No data-block read bypasses RS verification (universal
   coverage, no flag-gated path).
5. Throughput documented in
   `~/git/yocto-hardened/Documentation/benchmark.md`. The
   write-path overhead is expected, the read-path overhead is
   expected to be small except on correction events.

### RFC v4 readiness — exit conditions of stage 4

Per `Documentation/roadmap.md` history (the previous "Must-have
before v4 submission" list, now incorporated here): RFC v4 to
linux-fsdevel cannot be submitted before stage 4 closes,
because reviewers (Pedro Falcato in particular) flagged the
opt-in model as a use-case scope problem. Submitting v4 with
stage 3 closed but stage 4 open would re-trigger the same NACK.

Therefore the following items are **conditions of stage 4
closure**, not separate Must-haves:

- xfstests Yocto recipe with `generic/{001,002,010,098,257}`
  passing inside Yocto. Currently four pass manually
  (002, 010, 098, 257); 001 needs a >2 GiB scratch image
  (known-limitations 5.1).
- Patch series restructured into a clean atomic series
  (rebased fixups, no surgery on already-reviewed patches).
- Response to all open reviewer comments staged in the cover
  letter draft. Eric Biggers' RS comments are already
  addressed and validated.

### Tag

Planned: `v0.4.0-universal-protection`. RFC v4 submission to
linux-fsdevel happens immediately after this tag.

---

## Stage 5 — Offensive security analysis

**Status:** PENDING.

This stage produces `Documentation/security-analysis.md`. The
analysis IS the validation; per
`context-ftrfs-validation.md` section 6.4, there is no separate
acceptance criterion beyond the document landing in tree under
review.

### Scope

- Targeted fuzzing with syzkaller against the FTRFS module
  syscall surface (mount, file ops, ioctls if any), and afl++
  against `mkfs.ftrfs` and the mount-time parsing of corrupted
  images.
- Fault-injection harness for reproducible coverage of the
  scenarios listed in known-limitations 5.2: single-symbol
  errors per protected structure, multi-symbol at and below
  the RS limit, uncorrectable (must fail closed), burst
  errors crossing sub-block boundaries, corruption events on
  metadata structures (superblock, inode table, bitmap,
  directory blocks).
- Adversarial review against the Family B threat actor
  profile of threat-model section 3.

### Exit conditions

1. `Documentation/security-analysis.md` exists, peer-reviewed
   by at least one external reviewer with kernel security
   background.
2. The fault-injection harness is in tree under
   `tools/ftrfs-fuzz/` or equivalent and is invokable as a
   standalone command.
3. No High-severity finding remains open. Medium-severity
   findings are documented in known-limitations with mitigation
   timeline.

### Tag

Planned: `v0.5.0-security-reviewed`.

---

## Long-term vision (post-merge)

The following are out of the staged plan and are sketched here
only to make scope boundaries explicit. None of them is
scheduled, none competes with stages 3 to 5 for attention.

### Read-only mode with compression

A read-only mount mode combining RS FEC verification with
optional block-level compression (lz4, zstd), positioning FTRFS
as an alternative to squashfs for radiation-exposed read-only
partitions: squashfs detects nothing, FTRFS read-only mode
would detect and correct.

### Extended attributes and SELinux

`security.selinux` and POSIX ACL xattrs, prerequisite for
mandatory access control on FTRFS partitions. Combined with the
read-only mode above and symlink support below, makes FTRFS
suitable as a hardened Linux rootfs.

### Symlinks and rootfs capability

Required for any rootfs use. Combined with indirect blocks
(already implemented), xattr, SB_RDONLY enforcement
(known-limitations section 4), and the read-only mode, FTRFS
becomes deployable as both the root and the data partition of
a hardened embedded Linux system.

### Post-quantum metadata authentication

Per-block hash-based signatures (NIST FIPS 205 SLH-DSA /
SPHINCS+) on the superblock and inode structures, providing
post-quantum authenticated integrity for environments where
the threat model includes an active write-capable adversary
on the storage medium. Out of scope for v1 and v2 per
threat-model 2.3.

---

## Document maintenance

This roadmap is updated at each stage closure. The update
consists of:

1. Move the closing stage's section into the "what is done"
   form (status CLOSED, date, tag, key validation evidence).
2. Promote the next stage to ACTIVE.
3. Refresh the status table at the top.
4. Reconcile against `known-limitations.md` (resolved items
   removed there) and against `threat-model.md` (any
   reclassification recorded in section 8).

Editorial corrections (typos, dead links, formatting) do not
require this process and may be applied at any time.

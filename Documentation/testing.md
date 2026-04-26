# FTRFS Testing

## Test Environment

- **Kernel**: Linux 7.0.0 (final)
- **Architecture**: arm64 (QEMU cortex-a57, KVM/TCG)
- **Build system**: Yocto Styhead (5.1)
- **Yocto layer**: https://github.com/roastercode/yocto-hardened/tree/arm64-ftrfs
- **Cluster**: Slurm 25.11.4, 1 master + 3 compute nodes
- **FTRFS partition**: loop image on /tmp (64 MiB) per node

---

## Functional Test Sequence

Run on each node after build:

```sh
sudo insmod /lib/modules/7.0.0/updates/ftrfs.ko
sudo dd if=/dev/zero of=/tmp/ftrfs.img bs=4096 count=16384 2>/dev/null
sudo mkfs.ftrfs /tmp/ftrfs.img
sudo modprobe loop
sudo losetup /dev/loop0 /tmp/ftrfs.img
sudo mount -t ftrfs /dev/loop0 /data

# write / read
echo "test" | sudo tee /data/hello.txt
cat /data/hello.txt               # expected: test

# directory operations
sudo mkdir /data/testdir
sudo mv /data/hello.txt /data/testdir/
sudo rm /data/testdir/hello.txt
sudo rmdir /data/testdir
ls /data/                         # expected: empty

sudo umount /data
sudo losetup -d /dev/loop0
sudo rmmod ftrfs
dmesg | grep ftrfs | tail -5
```

### Expected dmesg output (zero RS errors)

```
ftrfs: loading out-of-tree module taints kernel.
ftrfs: module loaded (FTRFS Fault-Tolerant Radiation-Robust FS)
ftrfs: bitmaps initialized (16377 data blocks, 16377 free; 64 inodes, 63 free)
ftrfs: mounted (blocks=16384 free=16377 inodes=64)
ftrfs: module unloaded
```

Any `uncorrectable` or `corrected` message in dmesg indicates an RS FEC
event. `uncorrectable` at fresh mount after mkfs indicates a parity
mismatch between mkfs and the kernel — rebuild `mkfs-ftrfs`.

---

## Slurm HPC Benchmark

Run via `bin/hpc-benchmark.sh` in the yocto-hardened layer.
See [Documentation/benchmark.md](https://github.com/roastercode/yocto-hardened/tree/arm64-ftrfs/Documentation/benchmark.md)
for the full procedure.

### Results (2026-04-17, kernel 7.0, arm64 KVM/QEMU)

| Test                         | Result   |
|------------------------------|----------|
| Job submission latency (×3)  | ~0.25s   |
| 3-node parallel job          | 0.34s    |
| 9-job batch throughput       | 4.37s    |
| FTRFS mount (4 nodes)        | zero RS errors ✅ |
| FTRFS write from Slurm job   | ✅       |
| 0 BUG/WARN/Oops              | ✅       |

### Results (2026-04-26, kernel 7.0, arm64 KVM/QEMU, post-refactor 4a)

Pre-commit refactor of SB serialization to offsetof + BUILD_BUG_ON +
static_assert (commit 4a). On-disk format unchanged (v3). Behaviour
unchanged. syzkaller stopped on host before this run; see Test load
context section below.

| Test                         | Result   |
|------------------------------|----------|
| Job submission latency (×3)  | ~0.30s   |
| 3-node parallel job          | 0.33s    |
| 9-job batch throughput       | 4.49s    |
| FTRFS mount (4 nodes)        | zero RS errors ✅ |
| FTRFS write from Slurm job   | ✅       |
| 0 BUG/WARN/Oops              | ✅       |

### Test load context

The benchmark host runs alongside other workloads; reported throughput
varies meaningfully with background load. Two baselines are tracked:

- **Loaded baseline (historic, syzkaller running)**: 9-job throughput
  ~5.77s, tolerance band [4.6, 6.9] (±20%). Used while syzkaller was
  fuzzing the host kernel continuously. Archived for reference.
- **Unloaded baseline (current, syzkaller stopped 2026-04-26)**: 9-job
  throughput ~4.49s, tolerance band [3.6, 5.4] (±20%). This is the
  current pre-push regression target.

When syzkaller is restarted, switch back to the loaded band and note
the change in the commit message.

Note: QEMU TCG (software emulation) dominates latency. Results reflect
the emulated cortex-a57 environment, not bare metal.

Note: The shell in the Yocto image is BusyBox `/bin/sh`. Bash-specific
syntax (`for i in $(seq ...)`) is not supported. Use explicit background
jobs with `wait` instead.

---

## Pre-commit Invariants

A layered validation pipeline is enforced before each commit and push.

### Static invariants (`bin/ftrfs-invariants.sh`)

Located in the `yocto-hardened` layer. Runs in seconds, no VM required.
Checks:

1. No SB-related magic numbers (64, 68, 1685, 1688, 1689, 1621) remain
   in the five SB serialization functions across edac.c, super.c, and
   mkfs.ftrfs.c. After commit 4a, these functions use `offsetof` and
   the `FTRFS_SB_RS_*` defines exclusively.
2. mkfs produces a well-formed superblock: magic at offset 0, non-zero
   CRC32 at offset 64, non-zero RS parity at offset 3968.
3. The built `ftrfs.ko` exports the expected T (text) symbols.

### Combined validation (`bin/ftrfs-validate.sh`)

Chains `ftrfs-invariants.sh` (fast, static) then `hpc-benchmark.sh`
(slower, dynamic cluster bench). Use as the canonical pre-push gate:

```sh
ftrfs-validate.sh && git push
```

If invariants fail, the benchmark is not run -- fixing invariants
always takes priority over performance regressions.

### Pre-commit hook (`tools/checkpatch-precommit.sh`)

Runs the upstream Linux kernel style checker (`scripts/checkpatch.pl --strict`)
on staged C files before allowing a commit. Catches style drift and
well-known bug patterns (uninitialized stack variables, missing
endianness conversions, locking imbalance, etc.).

Installation:

```sh
cd ~/git/ftrfs
ln -sf ../../tools/checkpatch-precommit.sh .git/hooks/pre-commit
```

The hook locates the checker in this order:
1. `/usr/src/linux/scripts/checkpatch.pl` (Gentoo, follows current kernel)
2. `tools/checkpatch.pl` (frozen local copy, optional)

Bypass (use sparingly, never on push):

```sh
git commit --no-verify
```

### Audit rationale

A layered static-then-dynamic validation pipeline is the recommended
pattern for safety-critical software (DO-178C 6.4.3, IEC 61508-3
7.4.6). Static checks catch class-of-bug issues cheaply; dynamic
checks catch behavioural regressions. The combination provides
defence in depth against both code drift and runtime regressions.

---

## checkpatch.pl

All kernel source files verified with `checkpatch.pl --strict --file`
before each submission:

```sh
for f in alloc.c super.c edac.c dir.c namei.c inode.c ftrfs.h; do
    ~/git/linux/scripts/checkpatch.pl --no-tree --strict --file \
        fs/ftrfs/$f
done
```

Expected for all files:

```
total: 0 errors, 0 warnings, 0 checks, N lines checked
<file> has no obvious style problems and is ready for submission.
```

---

## RS FEC Parity Validation

The parity bytes written by `mkfs.ftrfs` must match exactly what
`lib/reed_solomon` expects with `init_rs(8, 0x187, fcr=0, prim=1, nroots=16)`.

Validated 2026-04-17:
```
Parité écrite:   ['0x1a', '0xfe', '0x1e', '0xd6', '0x6', '0x1e', '0x68', '0xac',
                  '0x56', '0x69', '0x72', '0xa7', '0x8b', '0xb7', '0x9f', '0x46']
Parité attendue: ['0x1a', '0xfe', '0x1e', '0xd6', '0x6', '0x1e', '0x68', '0xac',
                  '0x56', '0x69', '0x72', '0xa7', '0x8b', '0xb7', '0x9f', '0x46']
Match: True
```

---

## Planned: xfstests

A Yocto recipe for xfstests is planned. Target tests:

- `generic/001` — basic open/read/write/close
- `generic/002` — file creation and removal
- `generic/010` — hard links
- `generic/098` — rename
- `generic/257` — filesystem info (statfs)

These will replace the current manual functional test sequence and provide
reproducible results for the kernel submission cover letter.

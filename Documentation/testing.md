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

Note: QEMU TCG (software emulation) dominates latency. Results reflect
the emulated cortex-a57 environment, not bare metal.

Note: The shell in the Yocto image is BusyBox `/bin/sh`. Bash-specific
syntax (`for i in $(seq ...)`) is not supported. Use explicit background
jobs with `wait` instead.

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

# FTRFS Testing

## Test Environment

- **Kernel**: Linux 7.0.0 (final)
- **Architecture**: arm64 (QEMU cortex-a57, KVM/TCG)
- **Build system**: Yocto Styhead (5.1)
- **Yocto layer**: https://github.com/roastercode/yocto-hardened/tree/arm64-ftrfs
- **Cluster**: Slurm 25.11.4, 1 master + 3 compute nodes
- **FTRFS partition**: 64 MiB (`/dev/vdb`) per node

---

## Functional Test Sequence

The following sequence is run after each build on the arm64 master node:

```sh
sudo modprobe ftrfs
sudo mkfs.ftrfs /dev/vdb
sudo mkdir -p /var/tmp/ftrfs
sudo mount -t ftrfs /dev/vdb /var/tmp/ftrfs

# write
sudo bash -c 'echo test > /var/tmp/ftrfs/hello.txt'
cat /var/tmp/ftrfs/hello.txt              # expected: test

# directory operations
sudo mkdir /var/tmp/ftrfs/testdir
sudo mv /var/tmp/ftrfs/hello.txt /var/tmp/ftrfs/testdir/
sudo rm /var/tmp/ftrfs/testdir/hello.txt
sudo rmdir /var/tmp/ftrfs/testdir
ls /var/tmp/ftrfs/                        # expected: empty

sudo umount /var/tmp/ftrfs
sudo rmmod ftrfs
dmesg | tail -10                          # expected: 0 BUG/WARN/Oops
```

### Expected dmesg output

```
ftrfs: loading out-of-tree module taints kernel.
ftrfs: module loaded (FTRFS Fault-Tolerant Radiation-Robust FS)
ftrfs: bitmaps initialized (16378 data blocks, 16378 free; 64 inodes, 63 free)
ftrfs: mounted (blocks=16384 free=16378 inodes=64)
ftrfs: module unloaded
```

---

## Slurm HPC Benchmark

Run from the master node after the functional test sequence:

```sh
# Start cluster services
sudo /etc/init.d/munge start
sudo -u slurm slurmctld
# (on each compute node)
sudo slurmd

# Job submission latency (3 runs)
time srun --nodes=1 hostname

# 3-node parallel job
time srun --nodes=3 --ntasks=3 hostname

# 9-job throughput
time for i in $(seq 1 9); do srun --nodes=1 hostname & done; wait
```

### Results (April 2026, kernel 7.0, arm64 KVM)

| Test                         | Run 1  | Run 2  | Run 3  |
|------------------------------|--------|--------|--------|
| Job submission latency       | 0.378s | 0.256s | 0.266s |
| 3-node parallel              | 0.336s | —      | —      |
| 9-job throughput             | 0.052s | —      | —      |

Note: QEMU TCG (software emulation, no hardware KVM) dominates latency.
Results reflect the emulated cortex-a57 environment, not bare metal.

---

## checkpatch.pl

All kernel source files are verified with `checkpatch.pl --strict --file`
before each submission:

```sh
for f in alloc.c super.c edac.c dir.c namei.c inode.c ftrfs.h; do
    ~/git/linux/scripts/checkpatch.pl --no-tree --strict --file \
        fs/ftrfs/$f
done
```

Expected result for all files:

```
total: 0 errors, 0 warnings, 0 checks, N lines checked
<file> has no obvious style problems and is ready for submission.
```

---

## Planned: xfstests

A Yocto recipe for xfstests is planned. The target tests are:

- `generic/001` — basic open/read/write/close
- `generic/002` — file creation and removal
- `generic/010` — hard links
- `generic/098` — rename
- `generic/257` — filesystem info (statfs)

These will replace the current manual functional test sequence and provide
reproducible results that can be cited in the kernel submission cover letter.

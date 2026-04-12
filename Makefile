# SPDX-License-Identifier: GPL-2.0-only
#
# FTRFS — Fault-Tolerant Radiation-Robust Filesystem
# Out-of-tree kernel module
#

obj-m += ftrfs.o

ftrfs-y := super.o \
            inode.o \
            dir.o   \
            file.o  \
            edac.o

# Kernel source tree — override with: make KERNELDIR=/path/to/kernel
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

help:
	@echo "Targets:"
	@echo "  all          - build ftrfs.ko"
	@echo "  clean        - clean build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  KERNELDIR    - kernel build dir (default: running kernel)"
	@echo ""
	@echo "Cross-compile for arm64 (Yocto):"
	@echo "  make KERNELDIR=<yocto-build>/tmp/work/qemuarm64-*/linux-mainline/*/build \\"
	@echo "       ARCH=arm64 \\"
	@echo "       CROSS_COMPILE=aarch64-poky-linux-"

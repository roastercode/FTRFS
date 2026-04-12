# SPDX-License-Identifier: GPL-2.0-only
#
# FTRFS — Fault-Tolerant Radiation-Robust Filesystem
#

# In-tree build (when located at fs/ftrfs/ in the kernel source)
obj-$(CONFIG_FTRFS_FS) += ftrfs.o

ftrfs-y := super.o \
            inode.o \
            dir.o   \
            file.o  \
            edac.o  \
            alloc.o

ftrfs-$(CONFIG_FTRFS_FS_XATTR) += xattr.o

# Out-of-tree build support
ifneq ($(KERNELRELEASE),)
else

ifneq ($(KERNEL_SRC),)
  KERNELDIR := $(KERNEL_SRC)
else
  KERNELDIR ?= /lib/modules/$(shell uname -r)/build
endif

ifneq ($(O),)
  KBUILD_OUTPUT := O=$(O)
else
  KBUILD_OUTPUT :=
endif

PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) $(KBUILD_OUTPUT) M=$(PWD) \
		CONFIG_FTRFS_FS=m CONFIG_FTRFS_FS_XATTR=n CONFIG_FTRFS_FS_SECURITY=n \
		modules

clean:
	$(MAKE) -C $(KERNELDIR) $(KBUILD_OUTPUT) M=$(PWD) clean

modules_install:
	$(MAKE) -C $(KERNELDIR) $(KBUILD_OUTPUT) M=$(PWD) modules_install

help:
	@echo "Targets:"
	@echo "  all              - build ftrfs.ko (out-of-tree)"
	@echo "  clean            - clean build artifacts"
	@echo "  modules_install  - install ftrfs.ko"
	@echo ""
	@echo "Variables:"
	@echo "  KERNELDIR        - kernel build dir (default: running kernel)"
	@echo "  KERNEL_SRC       - Yocto kernel source (overrides KERNELDIR)"
	@echo ""
	@echo "In-tree: place at fs/ftrfs/, add to fs/Kconfig and fs/Makefile"

endif

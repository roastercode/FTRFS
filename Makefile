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

# Kernel source tree
# Priority: KERNEL_SRC (Yocto) > KERNELDIR (manual) > running kernel
ifneq ($(KERNEL_SRC),)
  KERNELDIR := $(KERNEL_SRC)
else
  KERNELDIR ?= /lib/modules/$(shell uname -r)/build
endif

# Build output dir: O= provided by Yocto (kernel-build-artifacts)
ifneq ($(O),)
  KBUILD_OUTPUT := O=$(O)
else
  KBUILD_OUTPUT :=
endif

PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) $(KBUILD_OUTPUT) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) $(KBUILD_OUTPUT) M=$(PWD) clean

modules_install:
	$(MAKE) -C $(KERNELDIR) $(KBUILD_OUTPUT) M=$(PWD) modules_install

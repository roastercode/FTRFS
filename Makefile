# SPDX-License-Identifier: GPL-2.0-only
#
# FTRFS — Fault-Tolerant Radiation-Robust Filesystem
#

obj-$(CONFIG_FTRFS_FS) += ftrfs.o

ftrfs-y := super.o \
            inode.o \
            dir.o   \
            file.o  \
            edac.o  \
            alloc.o \
            namei.o


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
		CONFIG_FTRFS_FS=m \
		modules

clean:
	$(MAKE) -C $(KERNELDIR) $(KBUILD_OUTPUT) M=$(PWD) clean

modules_install:
	$(MAKE) -C $(KERNELDIR) $(KBUILD_OUTPUT) M=$(PWD) modules_install

endif

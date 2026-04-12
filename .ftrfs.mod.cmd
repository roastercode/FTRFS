savedcmd_ftrfs.mod := printf '%s\n'   super.o inode.o dir.o file.o edac.o | awk '!x[$$0]++ { print("./"$$0) }' > ftrfs.mod

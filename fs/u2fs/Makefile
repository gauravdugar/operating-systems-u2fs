U2FS_VERSION="0.1"

EXTRA_CFLAGS += -DU2FS_VERSION=\"$(U2FS_VERSION)\"

obj-$(CONFIG_U2_FS) += u2fs.o

u2fs-y := dentry.o file.o inode.o main.o super.o lookup.o mmap.o whiteout.o

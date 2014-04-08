/*
 * Copyright (c) 1998-2011 Erez Zadok
 * Copyright (c) 2009	   Shrikar Archak
 * Copyright (c) 2003-2011 Stony Brook University
 * Copyright (c) 2003-2011 The Research Foundation of SUNY
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _U2FS_H_
#define _U2FS_H_

#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/seq_file.h>
#include <linux/statfs.h>
#include <linux/fs_stack.h>
#include <linux/magic.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched.h>

/* the file system name */
#define U2FS_NAME "u2fs"

/* u2fs root inode number */
#define U2FS_ROOT_INO     1

/* useful for tracking code reachability */
#define UDBG printk(KERN_DEFAULT "DBG:%s:%s:%d\n", __FILE__, __func__, __LINE__)

/* operations vectors defined in specific files */
extern const struct file_operations u2fs_main_fops;
extern const struct file_operations u2fs_dir_fops;
extern const struct inode_operations u2fs_main_iops;
extern const struct inode_operations u2fs_dir_iops;
extern const struct inode_operations u2fs_symlink_iops;
extern const struct super_operations u2fs_sops;
extern const struct dentry_operations u2fs_dops;
extern const struct address_space_operations u2fs_aops, u2fs_dummy_aops;
extern const struct vm_operations_struct u2fs_vm_ops;

extern int u2fs_init_inode_cache(void);
extern void u2fs_destroy_inode_cache(void);
extern int u2fs_init_dentry_cache(void);
extern void u2fs_destroy_dentry_cache(void);
extern int new_dentry_private_data(struct dentry *dentry);
extern void free_dentry_private_data(struct dentry *dentry);
extern struct dentry *u2fs_lookup(struct inode *dir, struct dentry *dentry,
				    struct nameidata *nd);
extern struct inode *u2fs_iget(struct super_block *sb,
				 struct inode *lower_inode);
extern int u2fs_interpose(struct dentry *dentry, struct super_block *sb,
			    struct path *left_path);

/* file private data */
struct u2fs_file_info {
	struct file *lower_file;
	const struct vm_operations_struct *lower_vm_ops;
};

/* u2fs inode data in memory */
struct u2fs_inode_info {
	struct inode *lower_inode;
	struct inode vfs_inode;
};

/* u2fs dentry data in memory */
struct u2fs_dentry_info {
	spinlock_t lock;	/* protects left_path */
	struct path left_path;
	struct path right_path;
};

/* u2fs super-block data in memory */
struct u2fs_sb_info {
	char *dev_name;		/* to identify different unions in pr_debug */
	struct super_block *left_sb;
	struct super_block *right_sb;
};

/*
 * inode to private data
 *
 * Since we use containers and the struct inode is _inside_ the
 * u2fs_inode_info structure, U2FS_I will always (given a non-NULL
 * inode pointer), return a valid non-NULL pointer.
 */
static inline struct u2fs_inode_info *U2FS_I(const struct inode *inode)
{
	return container_of(inode, struct u2fs_inode_info, vfs_inode);
}

/* dentry to private data */
#define U2FS_D(dent) ((struct u2fs_dentry_info *)(dent)->d_fsdata)

/* superblock to private data */
#define U2FS_SB(super) ((struct u2fs_sb_info *)(super)->s_fs_info)

/* file to private Data */
#define U2FS_F(file) ((struct u2fs_file_info *)((file)->private_data))

/* file to lower file */
static inline struct file *u2fs_lower_file(const struct file *f)
{
	return U2FS_F(f)->lower_file;
}

static inline void u2fs_set_lower_file(struct file *f, struct file *val)
{
	U2FS_F(f)->lower_file = val;
}

/* inode to lower inode. */
static inline struct inode *u2fs_lower_inode(const struct inode *i)
{
	return U2FS_I(i)->lower_inode;
}

static inline void u2fs_set_lower_inode(struct inode *i, struct inode *val)
{
	U2FS_I(i)->lower_inode = val;
}

/* superblock to lower superblock */
static inline struct super_block *u2fs_lower_super(
	const struct super_block *sb)
{
	return U2FS_SB(sb)->left_sb;
}

static inline void u2fs_set_left_super(struct super_block *sb,
					  struct super_block *val)
{
	U2FS_SB(sb)->left_sb = val;
}

static inline void u2fs_set_right_super(struct super_block *sb,
					  struct super_block *val)
{
	U2FS_SB(sb)->right_sb = val;
}

/* path based (dentry/mnt) macros */
static inline void pathcpy(struct path *dst, const struct path *src)
{
	dst->dentry = src->dentry;
	dst->mnt = src->mnt;
}

/* Returns struct path.  Caller must path_put it. */
static inline void u2fs_get_left_path(const struct dentry *dent,
					 struct path *left_path)
{
	spin_lock(&U2FS_D(dent)->lock);
	pathcpy(left_path, &U2FS_D(dent)->left_path);
	path_get(left_path);
	spin_unlock(&U2FS_D(dent)->lock);
	return;
}

/* Valid for both left and right paths */
static inline void u2fs_put_path(const struct dentry *dent,
					 struct path *path)
{
	path_put(path);
	return;
}

static inline void u2fs_set_left_path(const struct dentry *dent,
					 struct path *left_path)
{
	spin_lock(&U2FS_D(dent)->lock);
	pathcpy(&U2FS_D(dent)->left_path, left_path);
	spin_unlock(&U2FS_D(dent)->lock);
	return;
}

static inline void u2fs_set_right_path(const struct dentry *dent,
					 struct path *right_path)
{
	spin_lock(&U2FS_D(dent)->lock);
	pathcpy(&U2FS_D(dent)->right_path, right_path);
	spin_unlock(&U2FS_D(dent)->lock);
	return;
}

static inline void u2fs_reset_left_path(const struct dentry *dent)
{
	spin_lock(&U2FS_D(dent)->lock);
	U2FS_D(dent)->left_path.dentry = NULL;
	U2FS_D(dent)->left_path.mnt = NULL;
	spin_unlock(&U2FS_D(dent)->lock);
	return;
}

static inline void u2fs_put_reset_left_path(const struct dentry *dent)
{
	struct path left_path;
	spin_lock(&U2FS_D(dent)->lock);
	pathcpy(&left_path, &U2FS_D(dent)->left_path);
	U2FS_D(dent)->left_path.dentry = NULL;
	U2FS_D(dent)->left_path.mnt = NULL;
	spin_unlock(&U2FS_D(dent)->lock);
	path_put(&left_path);
	return;
}

/* locking helpers */
static inline struct dentry *lock_parent(struct dentry *dentry)
{
	struct dentry *dir = dget_parent(dentry);
	mutex_lock_nested(&dir->d_inode->i_mutex, I_MUTEX_PARENT);
	return dir;
}

static inline void unlock_dir(struct dentry *dir)
{
	mutex_unlock(&dir->d_inode->i_mutex);
	dput(dir);
}
#endif	/* not _U2FS_H_ */

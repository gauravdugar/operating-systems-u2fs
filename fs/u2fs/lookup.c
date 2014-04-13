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

#include "u2fs.h"

/*
 * Lookup one path component @name relative to a <base,mnt> path pair.
 * Behaves nearly the same as lookup_one_len (i.e., return negative dentry
 * on ENOENT), but uses the @mnt passed, so it can cross bind mounts and
 * other lower mounts properly.  If @new_mnt is non-null, will fill in the
 * new mnt there.  Caller is responsible to dput/mntput/path_put returned
 * @dentry and @new_mnt.
 *
 * Taken from unionfs/lookup.c
 */
struct dentry *__lookup_one(struct dentry *base, struct vfsmount *mnt,
		const char *name, struct vfsmount **new_mnt)
{
	struct dentry *dentry = NULL;
	struct path lower_path = {NULL, NULL};
	int err;

	/* we use flags=0 to get basic lookup */
	err = vfs_path_lookup(base, mnt, name, 0, &lower_path);

	switch (err) {
	case 0: /* no error */
		dentry = lower_path.dentry;
		if (new_mnt)
			*new_mnt = lower_path.mnt;
			/* rc already inc'ed */
		break;
	case -ENOENT:
		/*
		 * We don't consider ENOENT an error, and we want
		 * to return a negative dentry (ala lookup_one_len).
		 * As we know there was no inode for this name
		 * before (-ENOENT), then it's safe to call
		 * lookup_one_len (which doesn't take a vfsmount).
		 */
		dentry = lookup_lck_len(name, base, strlen(name));
		if (new_mnt)
			*new_mnt = mntget(lower_path.mnt);
		break;
	default: /* all other real errors */
		dentry = ERR_PTR(err);
		break;
	}
	return dentry;
}

/* The dentry cache is just so we have properly sized dentries */
static struct kmem_cache *u2fs_dentry_cachep;

int u2fs_init_dentry_cache(void)
{
	u2fs_dentry_cachep =
		kmem_cache_create("u2fs_dentry",
				sizeof(struct u2fs_dentry_info),
				0, SLAB_RECLAIM_ACCOUNT, NULL);

	return u2fs_dentry_cachep ? 0 : -ENOMEM;
}

void u2fs_destroy_dentry_cache(void)
{
	if (u2fs_dentry_cachep)
		kmem_cache_destroy(u2fs_dentry_cachep);
}

void free_dentry_private_data(struct dentry *dentry)
{
	if (!dentry || !dentry->d_fsdata)
		return;
	kmem_cache_free(u2fs_dentry_cachep, dentry->d_fsdata);
	dentry->d_fsdata = NULL;
}

/* allocate new dentry private data */
int new_dentry_private_data(struct dentry *dentry)
{
	struct u2fs_dentry_info *info = U2FS_D(dentry);

	/* use zalloc to init dentry_info.lower_path */
	info = kmem_cache_zalloc(u2fs_dentry_cachep, GFP_ATOMIC);
	if (!info)
		return -ENOMEM;

	spin_lock_init(&info->lock);
	dentry->d_fsdata = info;

	return 0;
}

static int u2fs_inode_test(struct inode *inode, void *candidate_lower_inode)
{
	struct inode *current_lower_inode = u2fs_lower_inode(inode);
	if (current_lower_inode == (struct inode *)candidate_lower_inode)
		return 1; /* found a match */
	else
		return 0; /* no match */
}

static int u2fs_inode_set(struct inode *inode, void *lower_inode)
{
	/* we do actual inode initialization in u2fs_iget */
	return 0;
}

struct inode *u2fs_iget(struct super_block *sb, struct inode *lower_inode)
{
	struct u2fs_inode_info *info;
	struct inode *inode; /* the new inode to return */
	int err;

	inode = iget5_locked(sb, /* our superblock */
			/*
			 * hashval: we use inode number, but we can
			 * also use "(unsigned long)lower_inode"
			 * instead.
			 */
			lower_inode->i_ino, /* hashval */
			u2fs_inode_test,	/* inode comparison function */
			u2fs_inode_set, /* inode init function */
			lower_inode); /* data passed to test+set fxns */
	if (!inode) {
		err = -EACCES;
		iput(lower_inode);
		return ERR_PTR(err);
	}
	/* if found a cached inode, then just return it */
	if (!(inode->i_state & I_NEW))
		return inode;

	/* initialize new inode */
	info = U2FS_I(inode);

	inode->i_ino = lower_inode->i_ino;
	if (!igrab(lower_inode)) {
		err = -ESTALE;
		return ERR_PTR(err);
	}
	u2fs_set_lower_inode(inode, lower_inode);

	inode->i_version++;

	/* use different set of inode ops for symlinks & directories */
	if (S_ISDIR(lower_inode->i_mode))
		inode->i_op = &u2fs_dir_iops;
	else if (S_ISLNK(lower_inode->i_mode))
		inode->i_op = &u2fs_symlink_iops;
	else
		inode->i_op = &u2fs_main_iops;

	/* use different set of file ops for directories */
	if (S_ISDIR(lower_inode->i_mode))
		inode->i_fop = &u2fs_dir_fops;
	else
		inode->i_fop = &u2fs_main_fops;

	inode->i_mapping->a_ops = &u2fs_aops;

	inode->i_atime.tv_sec = 0;
	inode->i_atime.tv_nsec = 0;
	inode->i_mtime.tv_sec = 0;
	inode->i_mtime.tv_nsec = 0;
	inode->i_ctime.tv_sec = 0;
	inode->i_ctime.tv_nsec = 0;

	/* properly initialize special inodes */
	if (S_ISBLK(lower_inode->i_mode) || S_ISCHR(lower_inode->i_mode) ||
			S_ISFIFO(lower_inode->i_mode) ||
			S_ISSOCK(lower_inode->i_mode))
		init_special_inode(inode, lower_inode->i_mode,
				lower_inode->i_rdev);

	/* all well, copy inode attributes */
	fsstack_copy_attr_all(inode, lower_inode);
	fsstack_copy_inode_size(inode, lower_inode);

	unlock_new_inode(inode);
	return inode;
}

/*
 * Connect a u2fs inode dentry/inode with several lower ones.  This is
 * the classic stackable file system "vnode interposition" action.
 *
 * @dentry: u2fs's dentry which interposes on lower one
 * @sb: u2fs's super_block
 * @left_path: the lower path (caller does path_get/put)
 */
int u2fs_interpose(struct dentry *dentry, struct super_block *sb)
{
	int err = 0;
	struct inode *inode;
	struct inode *lower_inode = NULL;
	struct super_block *left_sb;
	struct dentry *lower_dentry = NULL;
	int i = 0;

	for (i = 0; i < 2; i++) {
		lower_dentry = u2fs_get_lower_dentry(dentry, i);
		if (lower_dentry == NULL)
			continue;
		if (lower_dentry->d_inode == NULL)
			continue;
		lower_inode = lower_dentry->d_inode;
		break;
	}
	left_sb = u2fs_lower_super(sb);

	/* check that the lower file system didn't cross a mount point */
	if (lower_inode->i_sb != left_sb) {
		err = -EXDEV;
		goto out;
	}

	/*
	 * We allocate our new inode below by calling u2fs_iget,
	 * which will initialize some of the new inode's fields
	 */

	/* inherit lower inode number for u2fs's inode */
	inode = u2fs_iget(sb, lower_inode);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out;
	}
	d_add(dentry, inode);

out:
	return err;
}

/*
 * Main driver function for u2fs's lookup.
 *
 * Returns: NULL (ok), ERR_PTR if an error occurred.
 * Fills in lower_parent_path with <dentry,mnt> on success.
 *
 * Taken from unionfs/lookup.c : unionfs_lookup_full() and Modified.
 */
static struct dentry *__u2fs_lookup(struct dentry *dentry, int flags)
{
	int err = 0;
	struct vfsmount *lower_dir_mnt;
	struct dentry *lower_dir_dentry = NULL;
	struct dentry *lower_dentry = NULL;
	const char *name;
	struct path left_path;
	struct qstr this;
	struct vfsmount *lower_mnt;
	struct dentry *parent;
	struct path *parent_path;
	int i = 0;
	struct dentry *wh_dentry = NULL;
	bool valid = false;

	parent = dget_parent(dentry);

	/* must initialize dentry operations */
	d_set_d_op(dentry, &u2fs_dops);

	if (IS_ROOT(dentry))
		goto out;

	name = dentry->d_name.name;

	for (i = 0; i < 2; i++) {
		parent_path = u2fs_get_path(parent, i);
		/* now start the actual lookup procedure */
		lower_dir_dentry = parent_path->dentry;
		lower_dir_mnt = parent_path->mnt;
		lower_mnt = NULL;

		/* if the lower dentry's parent does not exist, skip this */
		if (!lower_dir_dentry || !lower_dir_dentry->d_inode)
			continue;

		/* also skip it if the parent isn't a directory. */
		if (!S_ISDIR(lower_dir_dentry->d_inode->i_mode))
			continue;

		/* Checking Whiteout */
		if (i == 0) {
			wh_dentry = lookup_whiteout(name, lower_dir_dentry);
			if (IS_ERR(wh_dentry)) {
				err = PTR_ERR(wh_dentry);
				u2fs_put_reset_all_path(dentry);
				goto out;
			}
			if (wh_dentry->d_inode) {
				dput(wh_dentry);
				break;
			}
			dput(wh_dentry);
		}

		lower_dentry = __lookup_one(lower_dir_dentry, lower_dir_mnt,
				name, &lower_mnt);
		if (IS_ERR(lower_dentry)) {
			err = PTR_ERR(lower_dentry);
			goto out;
		}

		u2fs_set_lower_dentry(dentry, i, lower_dentry);
		if (!lower_mnt)
			lower_mnt = u2fs_mntget(dentry->d_sb->s_root, i);
		u2fs_set_lower_mnt(dentry, i, lower_mnt);

		/*
		 * We always store the lower dentries above even if
		 * the whole u2fs dentry is negative (i.e., no lower inodes).
		 */
		if (!lower_dentry->d_inode)
			continue;
		valid = true;
	}

	/* Handle negative dentries. */
	if (valid) {
		err = u2fs_interpose(dentry, dentry->d_sb);
		if (err)
			u2fs_put_reset_all_path(dentry);
		if (err && err != -ENOENT) /* Negative dentry */
			goto out;
		goto out;
	}

	/* instatiate a new negative dentry */
	this.name = name;
	this.len = strlen(name);
	this.hash = full_name_hash(this.name, this.len);
	parent_path = u2fs_get_path(parent, 0);
	lower_dir_dentry = parent_path->dentry;
	lower_dentry = d_lookup(lower_dir_dentry, &this);

	if (lower_dentry)
		goto setup_lower;

	if (!lower_dir_dentry) {
		err = -EPERM;
		goto out;
	}
	lower_dentry = d_alloc(lower_dir_dentry, &this);
	if (!lower_dentry) {
		err = -ENOMEM;
		goto out;
	}
	d_add(lower_dentry, NULL); /* instantiate and hash */

setup_lower:
	left_path.dentry = lower_dentry;
	left_path.mnt = mntget(lower_dir_mnt);
	u2fs_set_path(dentry, &left_path, 0);

	/*
	 * If the intent is to create a file, then don't return an error, so
	 * the VFS will continue the process of making this negative dentry
	 * into a positive one.
	 */
	if (flags & (LOOKUP_CREATE|LOOKUP_RENAME_TARGET))
		err = 0;

out:
	/* Updating the access time of dir */
	fsstack_copy_attr_atime(parent->d_inode,
		u2fs_lower_inode(parent->d_inode));
	/* Reference count decrement */
	dput(parent);
	return ERR_PTR(err);
}

/*
 * Taken and modified from unionfs code.
 */
struct dentry *u2fs_lookup(struct inode *dir, struct dentry *dentry,
		struct nameidata *nd)
{
	struct dentry *ret;
	int err = 0;

	BUG_ON(!nd);

	/* allocate dentry private data.  We free it in ->d_release */
	err = new_dentry_private_data(dentry);
	if (err) {
		ret = ERR_PTR(err);
		goto out;
	}
	ret = __u2fs_lookup(dentry, nd->flags);
	if (IS_ERR(ret))
		goto out;
	if (ret)
		dentry = ret;
	if (dentry->d_inode)
		fsstack_copy_attr_times(dentry->d_inode,
				u2fs_lower_inode(dentry->d_inode));
out:
	return ret;
}

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

static int u2fs_create(struct inode *dir, struct dentry *dentry,
		int mode, struct nameidata *nd)
{
	int err = 0;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path *left_path, saved_path;

	left_path = u2fs_get_path(dentry, 0);
	lower_dentry = left_path->dentry;

	err = mnt_want_write(left_path->mnt);
	if (err)
		goto out;
	if (lower_dentry->d_parent->d_inode)
		err = check_unlink_whiteout(dentry, lower_dentry);
	if (err > 0)
		err = 0;
	if (err && err != -EROFS)
		goto out;

	if (!lower_dentry->d_parent->d_inode) {
		err = -EPERM;
		goto out;
	}

	lower_parent_dentry = u2fs_lock_parent(lower_dentry);

	pathcpy(&saved_path, &nd->path);
	pathcpy(&nd->path, left_path);
	err = vfs_create(lower_parent_dentry->d_inode, lower_dentry, mode, nd);
	pathcpy(&nd->path, &saved_path);
	if (err)
		goto out_unlock;
	err = u2fs_interpose(dentry, dir->i_sb);
	if (err)
		goto out_unlock;
	fsstack_copy_attr_times(dir, u2fs_lower_inode(dir));
	fsstack_copy_inode_size(dir, lower_parent_dentry->d_inode);

out_unlock:
	unlock_dir(lower_parent_dentry);
out:
	mnt_drop_write(left_path->mnt);
	return err;
}

static int u2fs_link(struct dentry *old_dentry, struct inode *dir,
		struct dentry *new_dentry)
{
	struct dentry *lower_old_dentry;
	struct dentry *lower_new_dentry;
	struct dentry *lower_dir_dentry;
	u64 file_size_save;
	int err;
	struct path *lower_old_path, *lower_new_path;

	file_size_save = i_size_read(old_dentry->d_inode);
	lower_old_path = u2fs_get_path(old_dentry, 0);
	if (!lower_old_path || !lower_old_path->dentry ||
			!lower_old_path->dentry->d_inode)
		return -EPERM;
	lower_new_path = u2fs_get_path(new_dentry, 0);

	lower_old_dentry = lower_old_path->dentry;
	lower_new_dentry = lower_new_path->dentry;

	if (!lower_new_dentry || !lower_new_dentry->d_parent)
		return -EPERM;

	if (!lower_new_dentry->d_parent->d_inode)
		return -EPERM;

	lower_dir_dentry = lock_parent(lower_new_dentry);

	err = mnt_want_write(lower_new_path->mnt);
	if (err)
		goto out_unlock;

	err = vfs_link(lower_old_dentry, lower_dir_dentry->d_inode,
			lower_new_dentry);
	if (err || !lower_new_dentry->d_inode)
		goto out;
	err = u2fs_interpose(new_dentry, dir->i_sb);
	if (err)
		goto out;
	fsstack_copy_attr_times(dir, lower_new_dentry->d_inode);
	fsstack_copy_inode_size(dir, lower_new_dentry->d_inode);
	set_nlink(old_dentry->d_inode,
			u2fs_lower_inode(old_dentry->d_inode)->i_nlink);
	i_size_write(new_dentry->d_inode, file_size_save);
out:
	mnt_drop_write(lower_new_path->mnt);
out_unlock:
	unlock_dir(lower_dir_dentry);
	return err;
}

static int u2fs_unlink(struct inode *dir, struct dentry *dentry)
{
	int err;
	struct dentry *lower_dentry = NULL, *temp_lower_dentry;
	struct dentry *lower_dir_dentry;
	struct vfsmount *mount = NULL, *temp_mount = NULL;
	bool right = false, left = false;
	int i = 0;
	struct dentry *parent;

	parent = u2fs_lock_parent(dentry);

	for (i = 1; i >= 0; i--) {
		temp_lower_dentry = u2fs_get_lower_dentry(dentry, i);
		temp_mount = u2fs_get_lower_mnt(dentry, i);
		if (temp_lower_dentry && temp_lower_dentry->d_inode) {
			if (i == 1)
				right = true;
			if (i == 0)
				left = true;
			lower_dentry = temp_lower_dentry;
			mount = temp_mount;
		}
	}
	if (!lower_dentry || !lower_dentry->d_inode) {
		err = -ENOENT;
		goto out_err;
	}

	dget(lower_dentry);

	if (left)
		lower_dir_dentry = u2fs_get_lower_dentry(parent, 0);
	else
		lower_dir_dentry = u2fs_get_lower_dentry(parent, 1);

	err = mnt_want_write(mount);
	if (err)
		goto out_unlock;

	if (left) {
		if (S_ISDIR(lower_dentry->d_inode->i_mode))
			err = vfs_rmdir(lower_dir_dentry->d_inode,
					lower_dentry);
		else
			err = vfs_unlink(lower_dir_dentry->d_inode,
					lower_dentry);
	}

	if (err)
		goto out;
	if (right) {
		temp_lower_dentry = u2fs_get_lower_dentry(dentry, 0);
		if (!temp_lower_dentry || !temp_lower_dentry->d_parent
			|| !temp_lower_dentry->d_parent->d_inode)
			err = -EPERM;
		else
			err = create_whiteout(dentry);
	}
	if (err == 0)
		inode_dec_link_count(dentry->d_inode);

	d_drop(dentry); /* this is needed, else LTP fails (VFS won't do it) */
out:
	mnt_drop_write(mount);

out_unlock:
	dput(lower_dentry);

out_err:
	u2fs_unlock_parent(dentry, parent);
	return err;
}

static int u2fs_symlink(struct inode *dir, struct dentry *dentry,
		const char *symname)
{
	int err = 0;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;

	lower_dentry = u2fs_get_lower_dentry(dentry, 0);

	if (lower_dentry->d_parent->d_inode)
		err = check_unlink_whiteout(dentry, lower_dentry);
	if (err > 0)    /* ignore if whiteout found and removed */
		err = 0;
	if (err && err != -EROFS)
		goto out;

	if (!lower_dentry->d_parent->d_inode)
		return -ENOENT;

	lower_parent_dentry = lock_parent(lower_dentry);

	if (IS_ERR(lower_parent_dentry)) {
		err = PTR_ERR(lower_parent_dentry);
		goto out_unlock;
	}
	err = vfs_symlink(lower_parent_dentry->d_inode, lower_dentry, symname);
	err = u2fs_interpose(dentry, dir->i_sb);
	if (err)
		goto out_unlock;
	fsstack_copy_attr_times(dir, u2fs_lower_inode(dir));
	fsstack_copy_inode_size(dir, lower_parent_dentry->d_inode);
	set_nlink(dir, u2fs_lower_inode(dir)->i_nlink);

out_unlock:
	unlock_dir(lower_parent_dentry);
out:
	return err;
}

static int u2fs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	int err = 0;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path *left_path;

	left_path = u2fs_get_path(dentry, 0);
	if (!left_path || !left_path->dentry)
		return -EPERM;
	lower_dentry = left_path->dentry;

	if (!lower_dentry->d_parent || !lower_dentry->d_parent->d_inode)
		return -EPERM;

	lower_parent_dentry = lock_parent(lower_dentry);

	err = mnt_want_write(left_path->mnt);
	if (err)
		goto out_unlock;
	err = vfs_mkdir(lower_parent_dentry->d_inode, lower_dentry, mode);
	if (err)
		goto out;

	err = u2fs_interpose(dentry, dir->i_sb);
	if (err)
		goto out;

	fsstack_copy_attr_times(dir, u2fs_lower_inode(dir));
	fsstack_copy_inode_size(dir, lower_parent_dentry->d_inode);
	/* update number of links on parent directory */
	set_nlink(dir, u2fs_lower_inode(dir)->i_nlink);

out:
	mnt_drop_write(left_path->mnt);
out_unlock:
	unlock_dir(lower_parent_dentry);
	return err;
}

/*
   static int u2fs_rmdir(struct inode *dir, struct dentry *dentry)
   {
   struct dentry *lower_dentry;
   struct dentry *lower_dir_dentry;
   int err;
   struct path *left_path;

   left_path = u2fs_get_path(dentry, 0);
   lower_dentry = left_path->dentry;
   lower_dir_dentry = lock_parent(lower_dentry);

   err = mnt_want_write(left_path->mnt);
   if (err)
   goto out_unlock;
   err = vfs_rmdir(lower_dir_dentry->d_inode, lower_dentry);
   if (err)
   goto out;

   d_drop(dentry);
   if (dentry->d_inode)
   clear_nlink(dentry->d_inode);
   fsstack_copy_attr_times(dir, lower_dir_dentry->d_inode);
   fsstack_copy_inode_size(dir, lower_dir_dentry->d_inode);
   set_nlink(dir, lower_dir_dentry->d_inode->i_nlink);

out:
mnt_drop_write(left_path->mnt);
out_unlock:
unlock_dir(lower_dir_dentry);
return err;
}
 */

static int u2fs_mknod(struct inode *dir, struct dentry *dentry, int mode,
		dev_t dev)
{
	int err = 0;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path *left_path;

	left_path = u2fs_get_path(dentry, 0);
	lower_dentry = left_path->dentry;
	lower_parent_dentry = lock_parent(lower_dentry);

	err = mnt_want_write(left_path->mnt);
	if (err)
		goto out_unlock;
	err = vfs_mknod(lower_parent_dentry->d_inode, lower_dentry, mode, dev);
	if (err)
		goto out;
	err = u2fs_interpose(dentry, dir->i_sb);
	if (err)
		goto out;
	fsstack_copy_attr_times(dir, u2fs_lower_inode(dir));
	fsstack_copy_inode_size(dir, lower_parent_dentry->d_inode);

out:
	mnt_drop_write(left_path->mnt);
out_unlock:
	unlock_dir(lower_parent_dentry);
	return err;
}

/*
 * The locking rules in u2fs_rename are complex.  We could use a simpler
 * superblock-level name-space lock for renames and copy-ups.
 */
static int u2fs_rename(struct inode *old_dir, struct dentry *old_dentry,
		struct inode *new_dir, struct dentry *new_dentry)
{
	int err = 0;
	struct dentry *lower_old_dentry = NULL;
	struct dentry *lower_new_dentry = NULL;
	struct dentry *lower_old_dir_dentry = NULL;
	struct dentry *lower_new_dir_dentry = NULL;
	struct dentry *trap = NULL;
	struct path *lower_old_path, *lower_new_path;

	lower_old_path = u2fs_get_path(old_dentry, 0);
	lower_new_path = u2fs_get_path(new_dentry, 0);
	if (!lower_old_path || !lower_new_path || !lower_old_path->dentry
			|| !lower_new_path->dentry)
		return -EPERM;
	lower_old_dentry = lower_old_path->dentry;
	lower_new_dentry = lower_new_path->dentry;
	if (!lower_old_dentry->d_inode || !lower_new_dentry->d_parent->d_inode)
		return -EPERM;
	lower_old_dir_dentry = dget_parent(lower_old_dentry);
	lower_new_dir_dentry = dget_parent(lower_new_dentry);

	trap = lock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	/* source should not be ancestor of target */
	if (trap == lower_old_dentry) {
		err = -EINVAL;
		goto out;
	}
	/* target should not be ancestor of source */
	if (trap == lower_new_dentry) {
		err = -ENOTEMPTY;
		goto out;
	}

	err = mnt_want_write(lower_old_path->mnt);
	if (err)
		goto out;
	err = mnt_want_write(lower_new_path->mnt);
	if (err)
		goto out_drop_old_write;

	err = vfs_rename(lower_old_dir_dentry->d_inode, lower_old_dentry,
			lower_new_dir_dentry->d_inode, lower_new_dentry);
	if (err)
		goto out_err;

	fsstack_copy_attr_all(new_dir, lower_new_dir_dentry->d_inode);
	fsstack_copy_inode_size(new_dir, lower_new_dir_dentry->d_inode);
	if (new_dir != old_dir) {
		fsstack_copy_attr_all(old_dir,
				lower_old_dir_dentry->d_inode);
		fsstack_copy_inode_size(old_dir,
				lower_old_dir_dentry->d_inode);
	}

out_err:
	mnt_drop_write(lower_new_path->mnt);
out_drop_old_write:
	mnt_drop_write(lower_old_path->mnt);
out:
	unlock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	dput(lower_old_dir_dentry);
	dput(lower_new_dir_dentry);
	return err;
}

static int u2fs_readlink(struct dentry *dentry, char __user *buf, int bufsiz)
{
	int err;
	struct dentry *lower_dentry;
	struct path *left_path;

	left_path = u2fs_get_path(dentry, 0);
	if (!left_path || !left_path->dentry)
		return -ENOENT;
	lower_dentry = left_path->dentry;

	if (!lower_dentry->d_inode) {
		left_path = u2fs_get_path(dentry, 1);
		if (!left_path || !left_path->dentry)
			return -ENOENT;
		lower_dentry = left_path->dentry;
	}

	if (!lower_dentry->d_inode->i_op ||
			!lower_dentry->d_inode->i_op->readlink) {
		err = -EINVAL;
		goto out;
	}

	err = lower_dentry->d_inode->i_op->readlink(lower_dentry,
			buf, bufsiz);
	if (err < 0)
		goto out;
	fsstack_copy_attr_atime(dentry->d_inode, lower_dentry->d_inode);

out:
	return err;
}

static void *u2fs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	char *buf;
	int len = PAGE_SIZE, err;
	mm_segment_t old_fs;

	/* This is freed by the put_link method assuming a successful call. */
	buf = kmalloc(len, GFP_KERNEL);
	if (!buf) {
		buf = ERR_PTR(-ENOMEM);
		goto out;
	}

	/* read the symlink, and then we will follow it */
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	err = u2fs_readlink(dentry, buf, len);
	set_fs(old_fs);
	if (err < 0) {
		kfree(buf);
		buf = ERR_PTR(err);
	} else {
		buf[err] = '\0';
	}
out:
	nd_set_link(nd, buf);
	return NULL;
}

/* this @nd *IS* still used */
static void u2fs_put_link(struct dentry *dentry, struct nameidata *nd,
		void *cookie)
{
	char *buf = nd_get_link(nd);
	if (!IS_ERR(buf))	/* free the char* */
		kfree(buf);
}

static int u2fs_permission(struct inode *inode, int mask)
{
	struct inode *lower_inode;
	int err;

	lower_inode = u2fs_lower_inode(inode);
	err = inode_permission(lower_inode, mask);
	return err;
}

static int u2fs_setattr(struct dentry *dentry, struct iattr *ia)
{
	int err = 0;
	struct dentry *lower_dentry;
	struct inode *inode;
	struct inode *lower_inode;
	struct iattr lower_ia;

	inode = dentry->d_inode;

	/*
	 * Check if user has permission to change inode.  We don't check if
	 * this user can change the lower inode: that should happen when
	 * calling notify_change on the lower inode.
	 */
	err = inode_change_ok(inode, ia);
	if (err)
		goto out_err;

	lower_dentry = u2fs_get_lower_dentry(dentry, 0);
	if (!lower_dentry || !lower_dentry->d_inode) {
		err = -EPERM;
		goto out_err;
	}
	lower_inode = lower_dentry->d_inode;

	/* prepare our own lower struct iattr (with the lower file) */
	memcpy(&lower_ia, ia, sizeof(lower_ia));
	if (ia->ia_valid & ATTR_FILE)
		lower_ia.ia_file = u2fs_lower_file(ia->ia_file, 0);

	/*
	 * If shrinking, first truncate upper level to cancel writing dirty
	 * pages beyond the new eof; and also if its' maxbytes is more
	 * limiting (fail with -EFBIG before making any change to the lower
	 * level).  There is no need to vmtruncate the upper level
	 * afterwards in the other cases: we fsstack_copy_inode_size from
	 * the lower level.
	 */
	if (ia->ia_valid & ATTR_SIZE) {
		err = inode_newsize_ok(inode, ia->ia_size);
		if (err)
			goto out_err;
		truncate_setsize(inode, ia->ia_size);
	}

	/*
	 * mode change is for clearing setuid/setgid bits. Allow lower fs
	 * to interpret this in its own way.
	 */
	if (lower_ia.ia_valid & (ATTR_KILL_SUID | ATTR_KILL_SGID))
		lower_ia.ia_valid &= ~ATTR_MODE;

	/* notify the (possibly copied-up) lower inode */
	/*
	 * Note: we use lower_dentry->d_inode, because lower_inode may be
	 * unlinked (no inode->i_sb and i_ino==0.  This happens if someone
	 * tries to open(), unlink(), then ftruncate() a file.
	 */
	mutex_lock(&lower_dentry->d_inode->i_mutex);
	err = notify_change(lower_dentry, &lower_ia); /* note: lower_ia */
	mutex_unlock(&lower_dentry->d_inode->i_mutex);
	if (err)
		goto out_err;

	/* get attributes from the lower inode */
	fsstack_copy_attr_all(inode, lower_inode);
	/*
	 * Not running fsstack_copy_inode_size(inode, lower_inode), because
	 * VFS should update our inode size, and notify_change on
	 * lower_inode should update its size.
	 */

out_err:
	return err;
}

const struct inode_operations u2fs_symlink_iops = {
	.readlink	= u2fs_readlink,
	.permission	= u2fs_permission,
	.follow_link	= u2fs_follow_link,
	.setattr	= u2fs_setattr,
	.put_link	= u2fs_put_link,
};

const struct inode_operations u2fs_dir_iops = {
	.create		= u2fs_create,
	.lookup		= u2fs_lookup,
	.link		= u2fs_link,
	.unlink		= u2fs_unlink,
	.symlink	= u2fs_symlink,
	.mkdir		= u2fs_mkdir,
	.rmdir		= u2fs_unlink,
	.mknod		= u2fs_mknod,
	.rename		= u2fs_rename,
	.permission	= u2fs_permission,
	.setattr	= u2fs_setattr,
};

const struct inode_operations u2fs_main_iops = {
	.permission	= u2fs_permission,
	.setattr	= u2fs_setattr,
};

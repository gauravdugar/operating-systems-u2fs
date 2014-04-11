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
#include <linux/module.h>

/*
 * Check for any overlapping of branch in these dentries.
 * Taken from unionfs/main.c
 */
static int is_branch_overlap(struct dentry *dent1, struct dentry *dent2)
{
	struct dentry *dent = NULL;

	dent = dent1;
	while ((dent != dent2) && (dent->d_parent != dent))
		dent = dent->d_parent;

	if (dent == dent2)
		return 1;

	dent = dent2;
	while ((dent != dent1) && (dent->d_parent != dent))
		dent = dent->d_parent;

	return (dent == dent1);
}

/*
 * Make sure the branch we just looked up (nd) makes sense:
 *
 * 1) we're not trying to stack unionfs on top of unionfs
 * 2) it exists
 * 3) is a directory
 *
 * Taken from unionfs/main.c
 */
int check_branch(const struct path *path)
{
	if (!strcmp(path->dentry->d_sb->s_type->name, U2FS_NAME))
		return -EINVAL;
	if (!path->dentry->d_inode)
		return -ENOENT;
	if (!S_ISDIR(path->dentry->d_inode->i_mode))
		return -ENOTDIR;
	return 0;
}

/*
 *
 *
 * Taken from unionfs/main.c and modified
 */
static int u2fs_get_kern_path(char *name, struct path *path)
{
	int err = kern_path(name, LOOKUP_FOLLOW, path);
	if (err) {
		printk(KERN_ERR "u2fs: error accessing "
				"lower directory '%s' (error %d)\n",
				name, err);
		return err;
	}

	err = check_branch(path);
	if (err) {
		printk(KERN_ERR "u2fs: lower directory "
				"'%s' is not a valid branch\n", name);
		path_put(path);
		return err;
	}
	return 0;
}

/*
 * T"rse mount options.  See the manual page for usage instructions.
 *
 * Returns the dentry object of the lower-level (lower) directory;
 * We want to mount our stackable file system on top of that lower directory.
 *
 * Taken from unionfs/main.c and modified
 */
static struct u2fs_dentry_info *u2fs_parse_options(
		struct super_block *sb,
		char *options)
{
	struct u2fs_dentry_info *root_info;
	char *optname, *ldir = NULL, *rdir = NULL;
	int err = 0;
	int ldirsfound = 0;
	int rdirsfound = 0;
	struct path lpath, rpath;

	/* allocate private data area */
	err = -ENOMEM;
	root_info =
		kzalloc(sizeof(struct u2fs_dentry_info), GFP_KERNEL);
	if (unlikely(!root_info))
		goto out_error;
	printk("arg: %s",options);
	while ((optname = strsep(&options, ",")) != NULL) {
		char *optarg;

		if (!optname || !*optname)
			continue;

		optarg = strchr(optname, '=');
		if (optarg)
			*optarg++ = '\0';

		/*
		 * All of our options take an argument now. Insert ones that
		 * don't, above this check.
		 */
		if (!optarg) {
			printk(KERN_ERR "u2fs: %s requires an argument\n",
					optname);
			err = -EINVAL;
			goto out_error;
		}

		if (!strcmp("ldir", optname)) {
			if (++ldirsfound > 1) {
				printk(KERN_ERR
						"u2fs: multiple ldirs specified\n");
				err = -EINVAL;
				goto out_error;
			}
			ldir = optarg;
			continue;
		}

		if (!strcmp("rdir", optname)) {
			if (++rdirsfound > 1) {
				printk(KERN_ERR
						"u2fs: multiple rdirs specified\n");
				err = -EINVAL;
				goto out_error;
			}
			rdir = optarg;
			continue;
		}

		err = -EINVAL;
		printk(KERN_ERR "u2fs: unrecognized option '%s'\n", optname);
		goto out_error;
	}
	if (ldirsfound != 1 || rdirsfound != 1) {
		printk(KERN_ERR "u2fs: ldirs and rdirs option required\n");
		err = -EINVAL;
		goto out_error;
	}

	err = u2fs_get_kern_path(ldir, &lpath);
	if(err) {
		goto out_error;
	}

	err = u2fs_get_kern_path(rdir, &rpath);
	if(err) {
		path_put(&lpath);
		goto out_error;
	}

	if (is_branch_overlap(lpath.dentry, rpath.dentry)) {
		printk(KERN_ERR "u2fs: Directories overlap\n");
		err = -EINVAL;
		path_put(&lpath);
		path_put(&rpath);
		goto out_error;
	}

	root_info->left_path.dentry = lpath.dentry;
	root_info->left_path.mnt = lpath.mnt;
	root_info->right_path.dentry = rpath.dentry;
	root_info->right_path.mnt = rpath.mnt;
	goto out;

out_error:
	kfree(root_info);
	root_info = ERR_PTR(err);
out:
	return root_info;
}

/*
 * There is no need to lock the u2fs_super_info's rwsem as there is no
 * way anyone can have a reference to the superblock at this point in time.
 *
 * Modified with code snippets from fs/unionfs/main.c
 */
static int u2fs_read_super(struct super_block *sb, void *raw_data, int silent)
{
	int err = 0;
	struct super_block *left_sb = NULL;
	struct super_block *right_sb = NULL;
	struct u2fs_dentry_info *root_info;
	struct inode *inode;
	char *dev_name = (char *) raw_data;

	if (!dev_name) {
		printk(KERN_ERR
				"u2fs: read_super: missing dev_name argument\n");
		err = -EINVAL;
		goto out;
	}

	/* parse lower path */
	root_info = u2fs_parse_options(sb, dev_name);
	if (IS_ERR(root_info)) {
		printk(KERN_ERR
				"u2fs: read_super: error while parsing options "
				"(err = %ld)\n", PTR_ERR(root_info));
		err = PTR_ERR(root_info);
		root_info = NULL;
		goto out_sput;
	}

	/* allocate superblock private data */
	sb->s_fs_info = kzalloc(sizeof(struct u2fs_sb_info), GFP_KERNEL);
	if (!U2FS_SB(sb)) {
		printk(KERN_CRIT "u2fs: read_super: out of memory\n");
		err = -ENOMEM;
		goto out;	//TO_CHECK
	}

	/* set the lower superblock field of left superblock */
	left_sb = root_info->left_path.dentry->d_sb;
	atomic_inc(&left_sb->s_active);
	u2fs_set_left_super(sb, left_sb);

	/* set the lower superblock field of right superblock */
	right_sb = root_info->right_path.dentry->d_sb;
	atomic_inc(&right_sb->s_active);
	u2fs_set_right_super(sb, right_sb);

	/* max Bytes is the maximum bytes from highest priority branch */
	sb->s_maxbytes = left_sb->s_maxbytes;

	/*
	 * Our c/m/atime granularity is 1 ns because we may stack on file
	 * systems whose granularity is as good.
	 */
	sb->s_time_gran = 1;

	sb->s_op = &u2fs_sops;

	/* get a new inode and allocate our root dentry */
	inode = u2fs_iget(sb, root_info->left_path.dentry->d_inode);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out_pput;	// TO_CHECK
	}
	sb->s_root = d_alloc_root(inode);
	if (!sb->s_root) {
		err = -ENOMEM;
		goto out_iput;
	}
	d_set_d_op(sb->s_root, &u2fs_dops);

	/* link the upper and lower dentries */
	sb->s_root->d_fsdata = NULL;
	err = new_dentry_private_data(sb->s_root);
	if (err)
		goto out_freeroot;

	/* if get here: cannot have error */

	/* set the lower dentries for s_root */
	u2fs_set_path(sb->s_root, &(root_info->left_path), 0);
	u2fs_set_path(sb->s_root, &(root_info->right_path), 1);

	/*
	 * No need to call interpose because we already have a positive
	 * dentry, which was instantiated by d_alloc_root.  Just need to
	 * d_rehash it.
	 */
	d_rehash(sb->s_root);
	if (!silent)
		printk(KERN_INFO
				"u2fs: mounted on top of left %s\nright %s",
				left_sb->s_type->name, right_sb->s_type->name);
	goto out; /* all is well */

	/* no longer needed: free_dentry_private_data(sb->s_root); */
out_freeroot:
	dput(sb->s_root);
out_iput:
	iput(inode);
out_sput:
	kfree(U2FS_SB(sb));
	sb->s_fs_info = NULL;
out_pput:	// TO_CHECK
	atomic_dec(&left_sb->s_active);
	atomic_dec(&right_sb->s_active);
	path_put(&(root_info->left_path));
	path_put(&(root_info->right_path));
out:
	return err;
}

static struct dentry *u2fs_mount(struct file_system_type *fs_type,
		int flags, const char *dev_name,
		void *raw_data)
{
	struct dentry *dentry;

	dentry = mount_nodev(fs_type, flags, raw_data, u2fs_read_super);
	printk("iaddr: %p",dentry);
	if (!IS_ERR(dentry)) {
		printk("dev_name : %s", dev_name);
		U2FS_SB(dentry->d_sb)->dev_name =
			kstrdup(dev_name, GFP_KERNEL);
	}
	return dentry;
}

static struct file_system_type u2fs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= U2FS_NAME,
	.mount		= u2fs_mount,
	.kill_sb	= generic_shutdown_super,
	.fs_flags	= FS_REVAL_DOT,
};

static int __init init_u2fs_fs(void)
{
	int err;

	pr_info("Registering u2fs " U2FS_VERSION "\n");

	err = u2fs_init_inode_cache();
	if (err)
		goto out;
	err = u2fs_init_dentry_cache();
	if (err)
		goto out;
	err = register_filesystem(&u2fs_fs_type);
out:
	if (err) {
		u2fs_destroy_inode_cache();
		u2fs_destroy_dentry_cache();
	}
	return err;
}

static void __exit exit_u2fs_fs(void)
{
	u2fs_destroy_inode_cache();
	u2fs_destroy_dentry_cache();
	unregister_filesystem(&u2fs_fs_type);
	pr_info("Completed u2fs module unload\n");
}

MODULE_AUTHOR("Erez Zadok, Filesystems and Storage Lab, Stony Brook University"
		" (http://www.fsl.cs.sunysb.edu/)");
MODULE_DESCRIPTION("U2fs " U2FS_VERSION
		" (http://u2fs.filesystems.org/)");
MODULE_LICENSE("GPL");

module_init(init_u2fs_fs);
module_exit(exit_u2fs_fs);

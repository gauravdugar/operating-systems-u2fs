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
 * There is no need to lock the u2fs_super_info's rwsem as there is no
 * way anyone can have a reference to the superblock at this point in time.
 */
static int u2fs_read_super(struct super_block *sb, void *raw_data, int silent)
{
	int err = 0;
	struct super_block *lower_sb;
	struct path lower_path;
	char *dev_name = (char *) raw_data;
	struct inode *inode;

	printk("\nRaw Data = %s\nsilent = %d\n", dev_name, silent);

	if (!dev_name) {
		printk(KERN_ERR
				"u2fs: read_super: missing dev_name argument\n");
		err = -EINVAL;
		goto out;
	}

	/* parse lower path */
	err = kern_path(dev_name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
			&lower_path);
	if (err) {
		printk(KERN_ERR	"u2fs: error accessing "
				"lower directory '%s'\n", dev_name);
		goto out;
	}

	/* allocate superblock private data */
	sb->s_fs_info = kzalloc(sizeof(struct u2fs_sb_info), GFP_KERNEL);
	if (!U2FS_SB(sb)) {
		printk(KERN_CRIT "u2fs: read_super: out of memory\n");
		err = -ENOMEM;
		goto out_free;
	}

	/* set the lower superblock field of upper superblock */
	lower_sb = lower_path.dentry->d_sb;
	atomic_inc(&lower_sb->s_active);
	u2fs_set_lower_super(sb, lower_sb);

	/* inherit maxbytes from lower file system */
	sb->s_maxbytes = lower_sb->s_maxbytes;

	/*
	 * Our c/m/atime granularity is 1 ns because we may stack on file
	 * systems whose granularity is as good.
	 */
	sb->s_time_gran = 1;

	sb->s_op = &u2fs_sops;

	/* get a new inode and allocate our root dentry */
	inode = u2fs_iget(sb, lower_path.dentry->d_inode);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out_sput;
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
	u2fs_set_lower_path(sb->s_root, &lower_path);

	/*
	 * No need to call interpose because we already have a positive
	 * dentry, which was instantiated by d_alloc_root.  Just need to
	 * d_rehash it.
	 */
	d_rehash(sb->s_root);
	if (!silent)
		printk(KERN_INFO
				"u2fs: mounted on top of %s type %s\n",
				dev_name, lower_sb->s_type->name);
	goto out; /* all is well */

	/* no longer needed: free_dentry_private_data(sb->s_root); */
out_freeroot:
	dput(sb->s_root);
out_iput:
	iput(inode);
out_sput:
	/* drop refs we took earlier */
	atomic_dec(&lower_sb->s_active);
	kfree(U2FS_SB(sb));
	sb->s_fs_info = NULL;
out_free:
	path_put(&lower_path);

out:
	return err;
}

/* GD */
/*
 * Parse mount options.  See the manual page for usage instructions.
 *
 * Returns the dentry object of the lower-level (lower) directory;
 * We want to mount our stackable file system on top of that lower directory.
 */
static struct unionfs_dentry_info *unionfs_parse_options(
		struct super_block *sb,
		char *options)
{
	struct unionfs_dentry_info *lower_root_info;
	char *optname;
	int err = 0;
	int bindex;
	int dirsfound = 0;

	/* allocate private data area */
	err = -ENOMEM;
	lower_root_info =
		kzalloc(sizeof(struct unionfs_dentry_info), GFP_KERNEL);
	if (unlikely(!lower_root_info))
		goto out_error;
	lower_root_info->bstart = -1;
	lower_root_info->bend = -1;
	lower_root_info->bopaque = -1;

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
			printk(KERN_ERR "unionfs: %s requires an argument\n",
					optname);
			err = -EINVAL;
			goto out_error;
		}

		if (!strcmp("dirs", optname)) {
			if (++dirsfound > 1) {
				printk(KERN_ERR
						"unionfs: multiple dirs specified\n");
				err = -EINVAL;
				goto out_error;
			}
			err = parse_dirs_option(sb, lower_root_info, optarg);
			if (err)
				goto out_error;
			continue;
		}

		err = -EINVAL;
		printk(KERN_ERR
				"unionfs: unrecognized option '%s'\n", optname);
		goto out_error;
	}
	if (dirsfound != 1) {
		printk(KERN_ERR "unionfs: dirs option required\n");
		err = -EINVAL;
		goto out_error;
	}
	goto out;

out_error:
	if (lower_root_info && lower_root_info->lower_paths) {
		for (bindex = lower_root_info->bstart;
				bindex >= 0 && bindex <= lower_root_info->bend;
				bindex++)
			path_put(&lower_root_info->lower_paths[bindex]);
	}

	kfree(lower_root_info->lower_paths);
	kfree(lower_root_info);

	kfree(UNIONFS_SB(sb)->data);
	UNIONFS_SB(sb)->data = NULL;

	lower_root_info = ERR_PTR(err);
out:
	return lower_root_info;
}
/* ~GD */

struct dentry *u2fs_mount(struct file_system_type *fs_type, int flags,
		const char *dev_name, void *raw_data)
{
	void *lower_path_name = (void *) dev_name;

	return mount_nodev(fs_type, flags, lower_path_name,
			u2fs_read_super);
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

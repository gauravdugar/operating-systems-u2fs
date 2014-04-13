/* This file contains ths file has functions copied from readstat.c
 * It has functions to support u2fs_filldir() in file.c routines for
 * maintaining readdir state.
 */
#include "u2fs.h"

static struct kmem_cache *u2fs_filldir_cachep;

int u2fs_init_filldir_cache(void)
{
	u2fs_filldir_cachep =
		kmem_cache_create("u2fs_filldir",
				sizeof(struct filldir_node), 0,
				SLAB_RECLAIM_ACCOUNT, NULL);

	if (u2fs_filldir_cachep)
		return 0;
	return -ENOMEM;
}

void u2fs_destroy_filldir_cache(void)
{
	if (u2fs_filldir_cachep)
		kmem_cache_destroy(u2fs_filldir_cachep);
}

static void free_filldir_node(struct filldir_node *node)
{
	if (node->namelen >= DNAME_INLINE_LEN)
		kfree(node->name);
	kmem_cache_free(u2fs_filldir_cachep, node);
}

void free_filldirs(struct list_head *heads, int size)
{
	struct filldir_node *tmp;
	int i;

	for (i = 0; i < size; i++) {
		struct list_head *head = &(heads[i]);
		struct list_head *pos, *n;

		/* traverse the list and deallocate space */
		list_for_each_safe(pos, n, head) {
			tmp = list_entry(pos, struct filldir_node, file_list);
			list_del(&tmp->file_list);
			free_filldir_node(tmp);
		}
	}
}

struct filldir_node *find_filldir_node(struct list_head *heads,
		const char *name, int namelen, int size)
{
	int index;
	unsigned int hash;
	struct list_head *head;
	struct list_head *pos;
	struct filldir_node *cursor = NULL;
	int found = 0;

	BUG_ON(namelen <= 0);

	hash = full_name_hash(name, namelen);
	index = hash % size;

	head = &(heads[index]);
	list_for_each(pos, head) {
		cursor = list_entry(pos, struct filldir_node, file_list);

		if (cursor->namelen == namelen && cursor->hash == hash &&
				!strncmp(cursor->name, name, namelen)) {
			/*
			 * a duplicate exists, and hence no need to create
			 * entry to the list
			 */
			found = 1;

			/*
			 * if a duplicate is found in this branch, and is
			 * not due to the caller looking for an entry to
			 * whiteout, then the file system may be corrupted.
			 */
			break;
		}
	}

	if (!found)
		cursor = NULL;

	return cursor;
}

int add_filldir_node(struct list_head *heads, const char *name,
		int namelen, int size, int whiteout)
{
	struct filldir_node *new;
	unsigned int hash;
	int index;
	int err = 0;
	struct list_head *head;

	BUG_ON(namelen <= 0);

	hash = full_name_hash(name, namelen);
	index = hash % size;
	head = &(heads[index]);

	UDBG;
	new = kmem_cache_alloc(u2fs_filldir_cachep, GFP_KERNEL);
	if (unlikely(!new)) {
		err = -ENOMEM;
		goto out;
	}

	INIT_LIST_HEAD(&new->file_list);
	UDBG;
	new->namelen = namelen;
	new->hash = hash;
	new->whiteout = whiteout;

	if (namelen < DNAME_INLINE_LEN) {
		UDBG;
		new->name = new->iname;
	} else {
		UDBG;
		new->name = kmalloc(namelen + 1, GFP_KERNEL);
		if (unlikely(!new->name)) {
			kmem_cache_free(u2fs_filldir_cachep, new);
			new = NULL;
			goto out;
		}
	}

	UDBG;
	memcpy(new->name, name, namelen);
	UDBG;
	new->name[namelen] = '\0';
	UDBG;
	list_add(&(new->file_list), head);
	UDBG;
out:
	return err;
}

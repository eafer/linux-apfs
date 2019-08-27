// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/apfs/btree.c
 *
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/buffer_head.h>
#include <linux/slab.h>
#include "apfs.h"
#include "btree.h"
#include "key.h"
#include "message.h"
#include "node.h"
#include "super.h"
#include "transaction.h"

/**
 * apfs_child_from_query - Read the child id found by a successful nonleaf query
 * @query:	the query that found the record
 * @child:	Return parameter.  The child id found.
 *
 * Reads the child id in the nonleaf node record into @child and performs a
 * basic sanity check as a protection against crafted filesystems.  Returns 0
 * on success or -EFSCORRUPTED otherwise.
 */
static int apfs_child_from_query(struct apfs_query *query, u64 *child)
{
	char *raw = query->node->object.bh->b_data;

	if (query->len != 8) /* The data on a nonleaf node is the child id */
		return -EFSCORRUPTED;

	*child = le64_to_cpup((__le64 *)(raw + query->off));
	return 0;
}

/**
 * apfs_omap_lookup_block - Find the block number of a b-tree node from its id
 * @sb:		filesystem superblock
 * @tbl:	Root of the object map to be searched
 * @id:		id of the node
 * @block:	on return, the found block number
 * @write:	get write access to the object?
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_omap_lookup_block(struct super_block *sb, struct apfs_node *tbl,
			   u64 id, u64 *block, bool write)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_query *query;
	struct apfs_key key;
	int ret = 0;

	query = apfs_alloc_query(tbl, NULL /* parent */);
	if (!query)
		return -ENOMEM;

	apfs_init_omap_key(id, sbi->s_xid, &key);
	query->key = &key;
	query->flags |= APFS_QUERY_OMAP;

	ret = apfs_btree_query(sb, &query);
	if (ret)
		goto fail;

	ret = apfs_bno_from_query(query, block);
	if (ret) {
		apfs_alert(sb, "bad object map leaf block: 0x%llx",
			   query->node->object.block_nr);
		goto fail;
	}

	if (write) {
		struct apfs_node *node = query->node;
		struct buffer_head *node_bh = node->object.bh;
		struct apfs_btree_node_phys *node_raw;
		struct apfs_omap_key *key;
		struct apfs_omap_val *val;
		struct buffer_head *new_bh;

		/* TODO: update parent nodes */
		ASSERT(apfs_node_is_root(node) && apfs_node_is_leaf(node));

		node_raw = (void *)node_bh->b_data;
		ASSERT(sbi->s_xid == le64_to_cpu(node_raw->btn_o.o_xid));

		new_bh = apfs_read_object_block(sb, *block, write);
		if (IS_ERR(new_bh)) {
			ret = PTR_ERR(new_bh);
			goto fail;
		}

		key = (void *)node_raw + query->key_off;
		key->ok_xid = cpu_to_le64(sbi->s_xid); /* TODO: snapshots? */
		val = (void *)node_raw + query->off;
		val->ov_paddr = cpu_to_le64(new_bh->b_blocknr);
		*block = new_bh->b_blocknr;
		brelse(new_bh);

		set_buffer_csum(node_bh);
		mark_buffer_dirty(node_bh);
	}

fail:
	apfs_free_query(sb, query);
	return ret;
}

/**
 * apfs_alloc_query - Allocates a query structure
 * @node:	node to be searched
 * @parent:	query for the parent node
 *
 * Callers other than apfs_btree_query() should set @parent to NULL, and @node
 * to the root of the b-tree. They should also initialize most of the query
 * fields themselves; when @parent is not NULL the query will inherit them.
 *
 * Returns the allocated query, or NULL in case of failure.
 */
struct apfs_query *apfs_alloc_query(struct apfs_node *node,
				    struct apfs_query *parent)
{
	struct apfs_query *query;

	query = kmalloc(sizeof(*query), GFP_KERNEL);
	if (!query)
		return NULL;

	/* To be released by free_query. */
	apfs_node_get(node);
	query->node = node;
	query->key = parent ? parent->key : NULL;
	query->flags = parent ?
		parent->flags & ~(APFS_QUERY_DONE | APFS_QUERY_NEXT) : 0;
	query->parent = parent;
	/* Start the search with the last record and go backwards */
	query->index = node->records;
	query->depth = parent ? parent->depth + 1 : 0;

	return query;
}

/**
 * apfs_free_query - Free a query structure
 * @sb:		filesystem superblock
 * @query:	query to free
 *
 * Also frees the ancestor queries, if they are kept.
 */
void apfs_free_query(struct super_block *sb, struct apfs_query *query)
{
	while (query) {
		struct apfs_query *parent = query->parent;

		apfs_node_put(query->node);
		kfree(query);
		query = parent;
	}
}

/**
 * apfs_btree_query - Execute a query on a b-tree
 * @sb:		filesystem superblock
 * @query:	the query to execute
 *
 * Searches the b-tree starting at @query->index in @query->node, looking for
 * the record corresponding to @query->key.
 *
 * Returns 0 in case of success and sets the @query->len, @query->off and
 * @query->index fields to the results of the query. @query->node will now
 * point to the leaf node holding the record.
 *
 * In case of failure returns an appropriate error code.
 */
int apfs_btree_query(struct super_block *sb, struct apfs_query **query)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_node *node;
	struct apfs_query *parent;
	u64 child_id, child_blk;
	int err;

next_node:
	if ((*query)->depth >= 12) {
		/*
		 * We need a maximum depth for the tree so we can't loop
		 * forever if the filesystem is damaged. 12 should be more
		 * than enough to map every block.
		 */
		apfs_alert(sb, "b-tree is corrupted");
		return -EFSCORRUPTED;
	}

	err = apfs_node_query(sb, *query);
	if (err == -EAGAIN) {
		if (!(*query)->parent) /* We are at the root of the tree */
			return -ENODATA;

		/* Move back up one level and continue the query */
		parent = (*query)->parent;
		(*query)->parent = NULL; /* Don't free the parent */
		apfs_free_query(sb, *query);
		*query = parent;
		goto next_node;
	}
	if (err)
		return err;
	if (apfs_node_is_leaf((*query)->node)) /* All done */
		return 0;

	err = apfs_child_from_query(*query, &child_id);
	if (err) {
		apfs_alert(sb, "bad index block: 0x%llx",
			   (*query)->node->object.block_nr);
		return err;
	}

	/*
	 * The omap maps a node id into a block number. The nodes
	 * of the omap itself do not need this translation.
	 */
	if ((*query)->flags & APFS_QUERY_OMAP) {
		child_blk = child_id;
	} else {
		/*
		 * we are always performing lookup from omap root. Might
		 * need improvement in the future.
		 */
		err = apfs_omap_lookup_block(sb, sbi->s_omap_root, child_id,
					     &child_blk, false /* write */);
		if (err)
			return err;
	}

	/* Now go a level deeper and search the child */
	node = apfs_read_node(sb, child_blk);
	if (IS_ERR(node))
		return PTR_ERR(node);

	if (node->object.oid != child_id)
		apfs_debug(sb, "corrupt b-tree");

	if ((*query)->flags & APFS_QUERY_MULTIPLE) {
		/*
		 * We are looking for multiple entries, so we must remember
		 * the parent node and index to continue the search later.
		 */
		*query = apfs_alloc_query(node, *query);
		apfs_node_put(node);
	} else {
		/* Reuse the same query structure to search the child */
		apfs_node_put((*query)->node);
		(*query)->node = node;
		(*query)->index = node->records;
		(*query)->depth++;
	}
	goto next_node;
}

/**
 * apfs_omap_read_node - Find and read a node from a b-tree
 * @id:		id for the seeked node
 *
 * Returns NULL is case of failure, otherwise a pointer to the resulting
 * apfs_node structure.
 */
struct apfs_node *apfs_omap_read_node(struct super_block *sb, u64 id)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_node *result;
	u64 block;
	int err;

	err = apfs_omap_lookup_block(sb, sbi->s_omap_root, id, &block,
				     false /* write */);
	if (err)
		return ERR_PTR(err);

	result = apfs_read_node(sb, block);
	if (IS_ERR(result))
		return result;

	if (result->object.oid != id)
		apfs_debug(sb, "corrupt b-tree");

	return result;
}

/* Constants used in managing the size of a node's table of contents */
#define BTREE_TOC_ENTRY_INCREMENT	8
#define BTREE_TOC_ENTRY_MAX_UNUSED	(2 * BTREE_TOC_ENTRY_INCREMENT)

/**
 * apfs_btree_insert - Insert a new record into a b-tree
 * @query:	query run to search for the record
 * @key:	on-disk record key
 * @key_len:	length of @key
 * @val:	on-disk record value (NULL for ghost records)
 * @val_len:	length of @val (0 for ghost records)
 *
 * The new record is placed right before the one found by @query.  Returns 0 on
 * success, or a negative error code in case of failure.
 */
int apfs_btree_insert(struct apfs_query *query, void *key, int key_len,
		      void *val, int val_len)
{
	struct apfs_node *node = query->node;
	struct super_block *sb = node->object.sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_btree_node_phys *node_raw;
	struct apfs_btree_info *info;
	int toc_entry_size;

	/*
	 * This function is a very rough first draft; all we need is to add
	 * a few records to an empty tree.
	 */
	ASSERT(apfs_node_is_root(node) && apfs_node_is_leaf(node));

	node_raw = (void *)query->node->object.bh->b_data;
	ASSERT(sbi->s_xid == le64_to_cpu(node_raw->btn_o.o_xid));

	/* TODO: support record fragmentation */
	if (node->free + key_len + val_len > node->data)
		return -ENOSPC;

	if (apfs_node_has_fixed_kv_size(node))
		toc_entry_size = sizeof(struct apfs_kvoff);
	else
		toc_entry_size = sizeof(struct apfs_kvloc);

	/* Expand the table of contents if necessary */
	if (sizeof(*node_raw) +
	    (node->records + 1) * toc_entry_size > node->key) {
		int new_key_base = node->key;
		int new_free_base = node->free;
		int inc;

		inc = BTREE_TOC_ENTRY_INCREMENT * toc_entry_size;

		new_key_base += inc;
		new_free_base += inc;
		if (new_free_base + key_len + val_len > node->data)
			return -ENOSPC;
		memmove((void *)node_raw + new_key_base,
			(void *)node_raw + node->key, node->free - node->key);

		node->key = new_key_base;
		node->free = new_free_base;
		le16_add_cpu(&node_raw->btn_table_space.len, inc);
		le16_add_cpu(&node_raw->btn_free_space.len, -inc);
	}

	query->index++; /* The query returned the record right before @key */

	/* Insert the new entry in the table of contents */
	if (apfs_node_has_fixed_kv_size(node)) {
		struct apfs_kvoff *toc_entry;

		toc_entry = (struct apfs_kvoff *)node_raw->btn_data +
								query->index;
		memmove(toc_entry + 1, toc_entry,
			(node->records - query->index) * sizeof(*toc_entry));

		if (!val) /* Ghost record */
			toc_entry->v = cpu_to_le16(APFS_BTOFF_INVALID);
		else
			toc_entry->v = cpu_to_le16(val_len);
		toc_entry->k = cpu_to_le16(node->free - node->key);
	} else {
		struct apfs_kvloc *toc_entry;

		toc_entry = (struct apfs_kvloc *)node_raw->btn_data +
								query->index;
		memmove(toc_entry + 1, toc_entry,
			(node->records - query->index) * sizeof(*toc_entry));

		toc_entry->v.off = cpu_to_le16(sb->s_blocksize - node->data -
					       sizeof(*info) + val_len);
		toc_entry->v.len = cpu_to_le16(val_len);
		toc_entry->k.off = cpu_to_le16(node->free - node->key);
		toc_entry->k.len = cpu_to_le16(key_len);
	}

	/* Write the record key to the end of the key area */
	memcpy((void *)node_raw + node->free, key, key_len);
	node->free += key_len;
	le16_add_cpu(&node_raw->btn_free_space.off, key_len);
	le16_add_cpu(&node_raw->btn_free_space.len, -key_len);

	if (val) {
		/* Write the record value to the beginning of the value area */
		memcpy((void *)node_raw + node->data - val_len, val, val_len);
		node->data -= val_len;
		le16_add_cpu(&node_raw->btn_free_space.len, -val_len);
	}

	info = (void *)node_raw + sb->s_blocksize - sizeof(*info);
	le64_add_cpu(&info->bt_key_count, 1);
	node_raw->btn_nkeys = cpu_to_le32(++node->records);

	if (key_len > le32_to_cpu(info->bt_longest_key))
		info->bt_longest_key = cpu_to_le32(key_len);
	if (val_len > le32_to_cpu(info->bt_longest_val))
		info->bt_longest_val = cpu_to_le32(val_len);

	apfs_obj_set_csum(sb, &node_raw->btn_o);
	mark_buffer_dirty(node->object.bh);
	return 0;
}

/**
 * apfs_btree_remove - Remove a record from a b-tree
 * @query:	exact query that found the record
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
int apfs_btree_remove(struct apfs_query *query)
{
	struct apfs_node *node = query->node;
	struct super_block *sb = node->object.sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_btree_node_phys *node_raw;
	struct apfs_nloc *free_head;
	struct apfs_btree_info *info;
	int later_entries = node->records - query->index - 1;

	/* This function is just a first draft that works with single nodes */
	ASSERT(apfs_node_is_root(node) && apfs_node_is_leaf(node));

	node_raw = (void *)query->node->object.bh->b_data;
	ASSERT(sbi->s_xid == le64_to_cpu(node_raw->btn_o.o_xid));

	/* Remove the entry from the table of contents */
	if (apfs_node_has_fixed_kv_size(node)) {
		struct apfs_kvoff *toc_entry;

		toc_entry = (struct apfs_kvoff *)node_raw->btn_data +
								query->index;
		memmove(toc_entry, toc_entry + 1,
			later_entries * sizeof(*toc_entry));
	} else {
		struct apfs_kvloc *toc_entry;

		toc_entry = (struct apfs_kvloc *)node_raw->btn_data +
								query->index;
		memmove(toc_entry, toc_entry + 1,
			later_entries * sizeof(*toc_entry));
	}

	info = (void *)node_raw + sb->s_blocksize - sizeof(*info);
	le64_add_cpu(&info->bt_key_count, -1);
	node_raw->btn_nkeys = cpu_to_le32(--node->records);

	/*
	 * TODO: move the edges of the key and value areas, if necessary; add
	 * the freed space to the linked list.
	 */
	free_head = &node_raw->btn_key_free_list;
	le16_add_cpu(&free_head->len, query->key_len);
	free_head = &node_raw->btn_val_free_list;
	le16_add_cpu(&free_head->len, query->len);

	apfs_obj_set_csum(sb, &node_raw->btn_o);
	mark_buffer_dirty(node->object.bh);
	return 0;
}

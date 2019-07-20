/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  linux/fs/apfs/spaceman.h
 *
 * Copyright (C) 2019 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#ifndef _APFS_SPACEMAN_H
#define _APFS_SPACEMAN_H

#include <linux/types.h>

/*
 * On-disk allocation info for a chunk of blocks
 */
struct apfs_chunk_info {
	__le64	ci_xid;
	__le64	ci_addr;
	__le32	ci_block_count;
	__le32	ci_free_count;
	__le64	ci_bitmap_addr;
} __packed;

/* Constants for the chunk info block */
#define	APFS_CI_COUNT_MASK		0x000FFFFF
#define	APFS_CI_COUNT_RESERVED_MASK	0xFFF00000

/*
 * Structure of a block with an array of chunk allocation info structures
 */
struct apfs_chunk_info_block {
	struct apfs_obj_phys	cib_o;
	__le32			cib_index;
	__le32			cib_chunk_info_count;
	struct apfs_chunk_info	cib_chunk_info[];
} __packed;

/*
 * Structure of a block with an array of addresses to chunk information blocks
 */
struct apfs_cib_addr_block {
	struct apfs_obj_phys	cab_o;
	__le32			cab_index;
	__le32			cab_cib_count;
	__le64			cab_cib_addr[];
} __packed;

/*
 * On-disk structure for a free queue
 */
struct apfs_spaceman_free_queue {
	__le64	sfq_count;
	__le64	sfq_tree_oid;
	__le64	sfq_oldest_xid;
	__le16	sfq_tree_node_limit;
	__le16	sfq_pad16;
	__le32	sfq_pad32;
	__le64	sfq_reserved;
} __packed;

/* Indexes for a free queue array */
enum {
	APFS_SFQ_IP	= 0,
	APFS_SFQ_MAIN	= 1,
	APFS_SFQ_TIER2	= 2,
	APFS_SFQ_COUNT	= 3
};

/*
 * On-disk structure for device allocation information
 */
struct apfs_spaceman_device {
	__le64	sm_block_count;
	__le64	sm_chunk_count;
	__le32	sm_cib_count;
	__le32	sm_cab_count;
	__le64	sm_free_count;
	__le32	sm_addr_offset;
	__le32	sm_reserved;
	__le64	sm_reserved2;
} __packed;

/* Indexes for a device array */
enum {
	APFS_SD_MAIN	= 0,
	APFS_SD_TIER2	= 1,
	APFS_SD_COUNT	= 2
};

/*
 * On-disk structure to describe allocation zone boundaries
 */
struct apfs_spaceman_allocation_zone_boundaries {
	__le64	saz_zone_start;
	__le64	saz_zone_end;
} __packed;

/* Allocation zone constants */
#define	APFS_SM_ALLOCZONE_INVALID_END_BOUNDARY		0
#define	APFS_SM_ALLOCZONE_NUM_PREVIOUS_BOUNDARIES	7

struct apfs_spaceman_allocation_zone_info_phys {
	struct apfs_spaceman_allocation_zone_boundaries	saz_current_boundaries;
	struct apfs_spaceman_allocation_zone_boundaries
	     saz_previous_boundaries[APFS_SM_ALLOCZONE_NUM_PREVIOUS_BOUNDARIES];

	__le16	saz_zone_id;
	__le16	saz_previous_boundary_index;
	__le32	saz_reserved;
} __packed;

/* Datazone constants */
#define	APFS_SM_DATAZONE_ALLOCZONE_COUNT	8

struct apfs_spaceman_datazone_info_phys {
	struct apfs_spaceman_allocation_zone_info_phys
	  sdz_allocation_zones[APFS_SD_COUNT][APFS_SM_DATAZONE_ALLOCZONE_COUNT];
} __packed;

/* Internal-pool bitmap constants */
#define	APFS_SPACEMAN_IP_BM_TX_MULTIPLIER	16
#define	APFS_SPACEMAN_IP_BM_INDEX_INVALID	0xFFFF
#define	APFS_SPACEMAN_IP_BM_BLOCK_COUNT_MAX	0xFFFE

/* Space manager flags */
#define	APFS_SM_FLAG_VERSIONED		0x00000001
#define	APFS_SM_FLAGS_VALID_MASK	APFS_SM_FLAG_VERSIONED

/*
 * On-disk structure for the space manager
 */
struct apfs_spaceman_phys {
	struct apfs_obj_phys			sm_o;
	__le32					sm_block_size;
	__le32					sm_blocks_per_chunk;
	__le32					sm_chunks_per_cib;
	__le32					sm_cibs_per_cab;
	struct apfs_spaceman_device		sm_dev[APFS_SD_COUNT];
	__le32					sm_flags;
	__le32					sm_ip_bm_tx_multiplier;
	__le64					sm_ip_block_count;
	__le32					sm_ip_bm_size_in_blocks;
	__le32					sm_ip_bm_block_count;
	__le64					sm_ip_bm_base;
	__le64					sm_ip_base;
	__le64					sm_fs_reserve_block_count;
	__le64					sm_fs_reserve_alloc_count;
	struct apfs_spaceman_free_queue		sm_fq[APFS_SFQ_COUNT];
	__le16					sm_ip_bm_free_head;
	__le16					sm_ip_bm_free_tail;
	__le32					sm_ip_bm_xid_offset;
	__le32					sm_ip_bitmap_offset;
	__le32					sm_ip_bm_free_next_offset;
	__le32					sm_version;
	__le32					sm_struct_size;
	struct apfs_spaceman_datazone_info_phys	sm_datazone;
} __packed;

/*
 * Space manager data in memory.
 */
struct apfs_spaceman {
	struct apfs_spaceman_phys *sm_raw; /* On-disk spaceman structure */
	struct buffer_head	  *sm_bh;  /* Buffer head for @sm_raw */

	int sm_struct_size;		/* Actual size of @sm_raw */
	u32 sm_blocks_per_chunk;	/* Blocks covered by a bitmap block */
	u32 sm_chunks_per_cib;		/* Chunk count in a chunk-info block */
	u64 sm_block_count;		/* Block count for the container */
	u64 sm_chunk_count;		/* Number of bitmap blocks */
	u32 sm_cib_count;		/* Number of chunk-info blocks */
	u64 sm_free_count;		/* Number of free blocks */
	u32 sm_addr_offset;		/* Offset of cib addresses in @sm_raw */
};

extern int apfs_read_spaceman(struct super_block *sb);
extern int apfs_free_queue_insert(struct super_block *sb, u64 bno);
extern int apfs_spaceman_allocate_block(struct super_block *sb, u64 *bno);

#endif	/* _APFS_SPACEMAN_H */

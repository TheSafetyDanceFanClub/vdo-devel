/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_SLAB_H
#define VDO_SLAB_H

#include <linux/list.h>

#include "permassert.h"

#include "admin-state.h"
#include "encodings.h"
#include "recovery-journal.h"
#include "types.h"

enum slab_rebuild_status {
	VDO_SLAB_REBUILT,
	VDO_SLAB_REPLAYING,
	VDO_SLAB_REQUIRES_SCRUBBING,
	VDO_SLAB_REQUIRES_HIGH_PRIORITY_SCRUBBING,
	VDO_SLAB_REBUILDING,
};

/*
 * This is the type declaration for the vdo_slab type. A vdo_slab currently consists of a run of
 * 2^23 data blocks, but that will soon change to dedicate a small number of those blocks for
 * metadata storage for the reference counts and slab journal for the slab.
 */
struct vdo_slab {
	/* A list entry to queue this slab in a block_allocator list */
	struct list_head allocq_entry;

	/* The struct block_allocator that owns this slab */
	struct block_allocator *allocator;

	/* The reference counts for the data blocks in this slab */
	struct ref_counts *reference_counts;
	/* The journal for this slab */
	struct slab_journal *journal;

	/* The slab number of this slab */
	slab_count_t slab_number;
	/* The offset in the allocator partition of the first block in this slab */
	physical_block_number_t start;
	/* The offset of the first block past the end of this slab */
	physical_block_number_t end;
	/* The starting translated PBN of the slab journal */
	physical_block_number_t journal_origin;
	/* The starting translated PBN of the reference counts */
	physical_block_number_t ref_counts_origin;

	/* The administrative state of the slab */
	struct admin_state state;
	/* The status of the slab */
	enum slab_rebuild_status status;
	/* Whether the slab was ever queued for scrubbing */
	bool was_queued_for_scrubbing;

	/* The priority at which this slab has been queued for allocation */
	u8 priority;
};

struct reference_updater;

int __must_check vdo_make_slab(physical_block_number_t slab_origin,
			       struct block_allocator *allocator,
			       physical_block_number_t translation,
			       struct recovery_journal *recovery_journal,
			       slab_count_t slab_number,
			       bool is_new,
			       struct vdo_slab **slab_ptr);

int __must_check vdo_allocate_ref_counts_for_slab(struct vdo_slab *slab);

void vdo_free_slab(struct vdo_slab *slab);

int __must_check
vdo_slab_block_number_from_pbn(struct vdo_slab *slab,
			       physical_block_number_t physical_block_number,
			       slab_block_number *slab_block_number_ptr);

bool __must_check vdo_is_slab_open(struct vdo_slab *slab);

void vdo_check_if_slab_drained(struct vdo_slab *slab);

#endif /* VDO_SLAB_H */

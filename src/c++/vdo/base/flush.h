/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_FLUSH_H
#define VDO_FLUSH_H

#include "types.h"
#include "vio.h"
#include "wait-queue.h"
#include "work-queue.h"

/* A marker for tracking which journal entries are affected by a flush request. */
struct vdo_flush {
	/* The completion for enqueueing this flush request. */
	struct vdo_completion completion;
	/* The flush bios covered by this request */
	struct bio_list bios;
#ifdef VDO_INTERNAL
	/* Time when the earlier bio arrived */
	u64 arrival_jiffies;
#endif /* VDO_INTERNAL */
	/* The wait queue entry for this flush */
	struct waiter waiter;
	/* Which flush this struct represents */
	sequence_number_t flush_generation;
};

struct flusher;

int __must_check vdo_make_flusher(struct vdo *vdo);

void vdo_free_flusher(struct flusher *flusher);

thread_id_t __must_check vdo_get_flusher_thread_id(struct flusher *flusher);

void vdo_complete_flushes(struct flusher *flusher);

void vdo_dump_flusher(const struct flusher *flusher);

void vdo_launch_flush(struct vdo *vdo, struct bio *bio);

void vdo_drain_flusher(struct flusher *flusher, struct vdo_completion *completion);

void vdo_resume_flusher(struct flusher *flusher, struct vdo_completion *parent);

#endif /* VDO_FLUSH_H */

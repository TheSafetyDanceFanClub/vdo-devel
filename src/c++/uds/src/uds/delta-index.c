// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */
#include "delta-index.h"

#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/compiler.h>
#include <linux/limits.h>
#include <linux/log2.h>

#include "buffer.h"
#include "config.h"
#include "cpu.h"
#include "errors.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "string-utils.h"
#include "time-utils.h"
#include "uds.h"

/*
 * The entries in a delta index could be stored in a single delta list, but for efficiency it uses
 * multiple delta lists. These lists are stored in a single chunk of memory managed by the
 * delta_zone structure. The delta_zone can move the data around within its memory, so it never
 * keeps any reference pointers, only bit offsets into the memory.
 *
 * The delta lists are stored as bit streams. These bit streams are stored in little endian order,
 * and all offsets into delta_memory are bit offsets.
 *
 * All entries are stored as a fixed length payload (the value) followed by a variable length key
 * (the delta), and always strictly in little endian order.
 *
 * A collision entry is used when two block names have the same delta list address. A collision
 * entry is encoded with DELTA == 0, and has 256 extension bits containing the full block name.
 *
 * The DELTA == 0 encoding usually indicates a collision with the preceding entry, but for the
 * first entry in any delta list there is no preceding entry, so the DELTA == 0 encoding at the
 * beginning of a delta list indicates a normal entry.
 *
 * The Huffman code is driven by 3 parameters:
 *
 *  MINBITS   The number of bits in the smallest code
 *  BASE      The number of values coded using a code of length MINBITS
 *  INCR      The number of values coded by using one additional bit
 *
 * These parameters are related by this equation:
 *
 *	BASE + INCR == 1 << MINBITS
 *
 * When an index is created, it needs to know the mean delta. From the mean delta, these three
 * parameters can be computed. The math for the Huffman code of an exponential distribution says
 * that
 *
 *	INCR = log(2) * MEAN_DELTA
 *
 * Then use the smallest MINBITS value so that
 *
 *	(1 << MINBITS) > INCR
 *
 * And then
 *
 *	BASE = (1 << MINBITS) - INCR
 *
 * Now the index can generate a code such that
 *
 * - The first BASE values code using MINBITS bits.
 * - The next INCR values code using MINBITS+1 bits.
 * - The next INCR values code using MINBITS+2 bits.
 * - The next INCR values code using MINBITS+3 bits.
 * - (and so on).
 *
 * ENCODE(DELTA):
 *
 *   if (DELTA < BASE)
 *	 put DELTA in MINBITS bits;
 *   else {
 *	 T1 = (DELTA - BASE) % INCR + BASE;
 *	 T2 = (DELTA - BASE) / INCR;
 *	 put T1 in MINBITS bits;
 *	 put 0 in T2 bits;
 *	 put 1 in 1 bit;
 *   }
 *
 * DECODE(BIT_STREAM):
 *
 *   T1 = next MINBITS bits of stream;
 *   if (T1 < BASE)
 *	 DELTA = T1;
 *   else {
 *	 Scan bits in the stream until reading a 1,
 *	   setting T2 to the number of 0 bits read;
 *	 DELTA = T2 * INCR + T1;
 *   }
 *
 * Within the delta list encoding, bits and bytes are numbered in little endian order. Within a
 * byte, bit 0 is the least significant bit (0x1), and bit 7 is the most significant bit (0x80).
 * Within a bit stream, bit 7 is the most signficant bit of byte 0, and bit 8 is the least
 * significant bit of byte 1. Within a byte array, a byte's number corresponds to its index in the
 * array.
 *
 * The delta_zone structure manages the memory that stores delta lists. Because the volume index
 * can contain a million delta lists or more, we want to be efficient with the delta list header
 * size.
 *
 * The delta list information is encoded into 16 bytes per list. The volume index delta list memory
 * can easily exceed 4 gigabits, so a 64 bit value is needed to address the memory. The volume
 * index delta lists average around 6 kilobits, so the size of a delta list can be stored in a
 * 16-bit value.
 *
 * The bit field utilities used on the delta lists assume that it is possible to read a few bytes
 * beyond the end of the bit field, so a delta_zone memory allocation is guarded by two invalid
 * delta lists to prevent reading outside the delta_zone memory. The valid delta lists are numbered
 * 1 to N, and the guard lists are numbered 0 and N+1.
 *
 * The functions to decode the bit streams include a step that skips over bits set to 0 until the
 * first 1 bit is found. A corrupted delta list could cause this step to run off the end of the
 * delta_zone memory. As extra protection against this happening, the guard list at the end is set
 * to all ones.
 *
 * The delta_index supports two different forms. The mutable form is created by
 * initialize_delta_index(), and is used for the volume index and for open chapter indexes. The
 * immutable form is created by initialize_delta_index_page(), and is used for closed (and cached)
 * chapter index pages. The immutable form does not allocate delta list headers or temporary
 * offsets, and thus is somewhat more memory efficient.
 *
 * This implementation assumes that the native machine is little endian, and that performance is
 * very important.
 */

/* This is the number of bits in a u32. */
enum {
	U32_BITS = sizeof(u32) * BITS_PER_BYTE,
};

/*
 * This is the largest field size supported by get_field() and set_field(). Any field that is
 * larger is not guaranteed to fit in a single byte-aligned u32.
 */
enum {
	MAX_FIELD_BITS = (sizeof(u32) - 1) * BITS_PER_BYTE + 1,
};

/*
 * This is the largest field size supported by get_big_field() and set_big_field(). Any field that
 * is larger is not guaranteed to fit in a single byte-aligned u64.
 */
enum {
	MAX_BIG_FIELD_BITS = (sizeof(u64) - 1) * BITS_PER_BYTE + 1,
};

/*
 * This is the number of guard bytes needed at the end of the memory byte array when using the bit
 * utilities. 3 bytes are needed when get_field() and set_field() access a field, because they will
 * access some extra bytes past the end of the field. 7 bytes are needed when get_big_field() and
 * set_big_field() access a big field, for the same reason. Note that move_bits() calls
 * get_big_field() and set_big_field(). The definition is written to make it clear how it is
 * derived.
 */
enum {
	POST_FIELD_GUARD_BYTES = sizeof(u64) - 1,
};

/* The number of guard bits that are needed in the tail guard list */
enum {
	GUARD_BITS = POST_FIELD_GUARD_BYTES * BITS_PER_BYTE
};

/*
 * The maximum size of a single delta list (in bytes). We count guard bytes in this value because a
 * buffer of this size can be used with move_bits().
 */
enum {
	DELTA_LIST_MAX_BYTE_COUNT =
		((U16_MAX + BITS_PER_BYTE) / BITS_PER_BYTE + POST_FIELD_GUARD_BYTES)
};

/* The number of extra bytes and bits needed to store a collision entry */
enum {
	COLLISION_BYTES = UDS_RECORD_NAME_SIZE,
	COLLISION_BITS = COLLISION_BYTES * BITS_PER_BYTE
};

/*
 * Immutable delta lists are packed into pages containing a header that encodes the delta list
 * information into 19 bits per list (64KB bit offset).
 */

enum { IMMUTABLE_HEADER_SIZE = 19 };

/*
 * Constants and structures for the saved delta index. "DI" is for delta_index, and -##### is a
 * number to increment when the format of the data changes.
 */

enum {
	MAGIC_SIZE = 8,
};

static const char DELTA_INDEX_MAGIC[] = "DI-00002";

struct delta_index_header {
	char magic[MAGIC_SIZE];
	u32 zone_number;
	u32 zone_count;
	u32 first_list;
	u32 list_count;
	u64 record_count;
	u64 collision_count;
};

/*
 * Header data used for immutable delta index pages. This data is followed by the delta list offset
 * table.
 */
struct delta_page_header {
	/* Externally-defined nonce */
	u64 nonce;
	/* The virtual chapter number */
	u64 virtual_chapter_number;
	/* Index of the first delta list on the page */
	u16 first_list;
	/* Number of delta lists on the page */
	u16 list_count;
} __packed;

static inline u64 get_delta_list_byte_start(const struct delta_list *delta_list)
{
	return delta_list->start / BITS_PER_BYTE;
}

static inline u16 get_delta_list_byte_size(const struct delta_list *delta_list)
{
	unsigned int bit_offset = delta_list->start % BITS_PER_BYTE;

	return BITS_TO_BYTES(bit_offset + delta_list->size);
}

static void rebalance_delta_zone(const struct delta_zone *delta_zone,
				 unsigned int first,
				 unsigned int last)
{
	struct delta_list *delta_list;
	u64 new_start;

	if (first == last) {
		/* Only one list is moving, and we know there is space. */
		delta_list = &delta_zone->delta_lists[first];
		new_start = delta_zone->new_offsets[first];
		if (delta_list->start != new_start) {
			u64 source;
			u64 destination;

			source = get_delta_list_byte_start(delta_list);
			delta_list->start = new_start;
			destination = get_delta_list_byte_start(delta_list);
			memmove(delta_zone->memory + destination,
				delta_zone->memory + source,
				get_delta_list_byte_size(delta_list));
		}
	} else {
		/*
		 * There is more than one list. Divide the problem in half, and use recursive calls
		 * to process each half. Note that after this computation, first <= middle, and
		 * middle < last.
		 */
		unsigned int middle = (first + last) / 2;

		delta_list = &delta_zone->delta_lists[middle];
		new_start = delta_zone->new_offsets[middle];

		/*
		 * The direction that our middle list is moving determines which half of the
		 * problem must be processed first.
		 */
		if (new_start > delta_list->start) {
			rebalance_delta_zone(delta_zone, middle + 1, last);
			rebalance_delta_zone(delta_zone, first, middle);
		} else {
			rebalance_delta_zone(delta_zone, first, middle);
			rebalance_delta_zone(delta_zone, middle + 1, last);
		}
	}
}

static inline size_t get_zone_memory_size(unsigned int zone_count, size_t memory_size)
{
	/* Round up so that each zone is a multiple of 64K in size. */
	enum {
		ALLOC_BOUNDARY = 64 * 1024,
	};

	return (memory_size / zone_count + ALLOC_BOUNDARY - 1) & -ALLOC_BOUNDARY;
}

void reset_delta_index(const struct delta_index *delta_index)
{
	unsigned int z;

	/*
	 * Initialize all delta lists to be empty. We keep 2 extra delta list descriptors, one
	 * before the first real entry and one after so that we don't need to bounds check the
	 * array access when calculating preceeding and following gap sizes.
	 */
	for (z = 0; z < delta_index->zone_count; z++) {
		u64 list_bits;
		u64 spacing;
		u64 offset;
		unsigned int i;
		struct delta_zone *zone = &delta_index->delta_zones[z];
		struct delta_list *delta_lists = zone->delta_lists;

		/* Zeroing the delta list headers initializes the head guard list correctly. */
		memset(delta_lists, 0, (zone->list_count + 2) * sizeof(struct delta_list));

		/* Set all the bits in the end guard list. */
		list_bits = (u64) zone->size * BITS_PER_BYTE - GUARD_BITS;
		delta_lists[zone->list_count + 1].start = list_bits;
		delta_lists[zone->list_count + 1].size = GUARD_BITS;
		memset(zone->memory + (list_bits / BITS_PER_BYTE), ~0, POST_FIELD_GUARD_BYTES);

		/* Evenly space out the real delta lists by setting regular offsets. */
		spacing = list_bits / zone->list_count;
		offset = spacing / 2;
		for (i = 1; i <= zone->list_count; i++) {
			delta_lists[i].start = offset;
			offset += spacing;
		}

		/* Update the statistics. */
		zone->discard_count += zone->record_count;
		zone->record_count = 0;
		zone->collision_count = 0;
	}
}

/* Compute the Huffman coding parameters for the given mean delta. */
static void compute_coding_constants(unsigned int mean_delta,
				     unsigned short *min_bits,
				     unsigned int *min_keys,
				     unsigned int *incr_keys)
{
	/*
	 * We want to compute the rounded value of log(2) * mean_delta. Since we cannot always use
	 * floating point, use a really good integer approximation.
	 */
	*incr_keys = (836158UL * mean_delta + 603160UL) / 1206321UL;
	*min_bits = bits_per(*incr_keys + 1);
	*min_keys = (1 << *min_bits) - *incr_keys;
}

void uninitialize_delta_index(struct delta_index *delta_index)
{
	unsigned int z;

	if (delta_index->delta_zones == NULL)
		return;

	for (z = 0; z < delta_index->zone_count; z++) {
		UDS_FREE(UDS_FORGET(delta_index->delta_zones[z].new_offsets));
		UDS_FREE(UDS_FORGET(delta_index->delta_zones[z].delta_lists));
		UDS_FREE(UDS_FORGET(delta_index->delta_zones[z].memory));
	}

	UDS_FREE(delta_index->delta_zones);
	memset(delta_index, 0, sizeof(struct delta_index));
}

static int initialize_delta_zone(struct delta_zone *delta_zone,
				 size_t size,
				 unsigned int first_list,
				 unsigned int list_count,
				 unsigned int mean_delta,
				 unsigned int payload_bits,
				 u8 tag)
{
	int result;

	result = UDS_ALLOCATE(size, u8, "delta list", &delta_zone->memory);
	if (result != UDS_SUCCESS)
		return result;

	result = UDS_ALLOCATE(list_count + 2, u64, "delta list temp", &delta_zone->new_offsets);
	if (result != UDS_SUCCESS)
		return result;

	/* Allocate the delta lists. */
	result = UDS_ALLOCATE(list_count + 2,
			      struct delta_list,
			      "delta lists",
			      &delta_zone->delta_lists);
	if (result != UDS_SUCCESS)
		return result;

	compute_coding_constants(mean_delta,
				 &delta_zone->min_bits,
				 &delta_zone->min_keys,
				 &delta_zone->incr_keys);
	delta_zone->value_bits = payload_bits;
	delta_zone->buffered_writer = NULL;
	delta_zone->size = size;
	delta_zone->rebalance_time = 0;
	delta_zone->rebalance_count = 0;
	delta_zone->record_count = 0;
	delta_zone->collision_count = 0;
	delta_zone->discard_count = 0;
	delta_zone->overflow_count = 0;
	delta_zone->first_list = first_list;
	delta_zone->list_count = list_count;
	delta_zone->tag = tag;

	return UDS_SUCCESS;
}

int initialize_delta_index(struct delta_index *delta_index,
			   unsigned int zone_count,
			   unsigned int list_count,
			   unsigned int mean_delta,
			   unsigned int payload_bits,
			   size_t memory_size,
			   u8 tag)
{
	int result;
	unsigned int z;
	size_t zone_memory;

	result = UDS_ALLOCATE(zone_count,
			      struct delta_zone,
			      "Delta Index Zones",
			      &delta_index->delta_zones);
	if (result != UDS_SUCCESS)
		return result;

	delta_index->zone_count = zone_count;
	delta_index->list_count = list_count;
	delta_index->lists_per_zone = DIV_ROUND_UP(list_count, zone_count);
	delta_index->mutable = true;
	delta_index->tag = tag;

	for (z = 0; z < zone_count; z++) {
		unsigned int lists_in_zone = delta_index->lists_per_zone;
		unsigned int first_list_in_zone = z * lists_in_zone;

		if (z == zone_count - 1) {
			/*
			 * The last zone gets fewer lists if zone_count doesn't evenly divide
			 * list_count. We'll have an underflow if the assertion below doesn't hold.
			 */
			if (delta_index->list_count <= first_list_in_zone) {
				uninitialize_delta_index(delta_index);
				return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
							      "%u delta lists not enough for %u zones",
							      list_count,
							      zone_count);
			}
			lists_in_zone = delta_index->list_count - first_list_in_zone;
		}

		zone_memory = get_zone_memory_size(zone_count, memory_size);
		result = initialize_delta_zone(&delta_index->delta_zones[z],
					       zone_memory,
					       first_list_in_zone,
					       lists_in_zone,
					       mean_delta,
					       payload_bits,
					       tag);
		if (result != UDS_SUCCESS) {
			uninitialize_delta_index(delta_index);
			return result;
		}
	}

	reset_delta_index(delta_index);
	return UDS_SUCCESS;
}

/* Read a bit field from an arbitrary bit boundary. */
static inline unsigned int get_field(const u8 *memory, u64 offset, int size)
{
	const void *addr = memory + offset / BITS_PER_BYTE;

	return (get_unaligned_le32(addr) >> (offset % BITS_PER_BYTE)) & ((1 << size) - 1);
}

/* Write a bit field to an arbitrary bit boundary. */
static inline void set_field(unsigned int value, u8 *memory, u64 offset, int size)
{
	void *addr = memory + offset / BITS_PER_BYTE;
	int shift = offset % BITS_PER_BYTE;
	u32 data = get_unaligned_le32(addr);

	data &= ~(((1 << size) - 1) << shift);
	data |= value << shift;
	put_unaligned_le32(data, addr);
}

/* Get the bit offset to the immutable delta list header. */
static inline unsigned int get_immutable_header_offset(unsigned int list_number)
{
	return sizeof(struct delta_page_header) * BITS_PER_BYTE +
		list_number * IMMUTABLE_HEADER_SIZE;
}

/* Get the bit offset to the start of the immutable delta list bit stream. */
static inline unsigned int get_immutable_start(const u8 *memory, unsigned int list_number)
{
	return get_field(memory, get_immutable_header_offset(list_number), IMMUTABLE_HEADER_SIZE);
}

/* Set the bit offset to the start of the immutable delta list bit stream. */
static inline void set_immutable_start(u8 *memory, unsigned int list_number, unsigned int start)
{
	set_field(start, memory, get_immutable_header_offset(list_number), IMMUTABLE_HEADER_SIZE);
}

static bool verify_delta_index_page(u64 nonce,
				    u16 list_count,
				    u64 expected_nonce,
				    u8 *memory,
				    size_t memory_size)
{
	unsigned int i;

	/*
	 * Verify the nonce. A mismatch can happen here during rebuild if we haven't written the
	 * entire volume at least once.
	 */
	if (nonce != expected_nonce)
		return false;

	/* Verify that the number of delta lists can fit in the page. */
	if (list_count > ((memory_size - sizeof(struct delta_page_header)) *
			  BITS_PER_BYTE / IMMUTABLE_HEADER_SIZE))
		return false;

	/*
	 * Verify that the first delta list is immediately after the last delta
	 * list header.
	 */
	if (get_immutable_start(memory, 0) != get_immutable_header_offset(list_count + 1))
		return false;

	/* Verify that the lists are in the correct order. */
	for (i = 0; i < list_count; i++)
		if (get_immutable_start(memory, i) > get_immutable_start(memory, i + 1))
			return false;

	/*
	 * Verify that the last list ends on the page, and that there is room
	 * for the post-field guard bits.
	 */
	if (get_immutable_start(memory, list_count) >
	    (memory_size - POST_FIELD_GUARD_BYTES) * BITS_PER_BYTE)
		return false;

	/* Verify that the guard bytes are correctly set to all ones. */
	for (i = 0; i < POST_FIELD_GUARD_BYTES; i++) {
		if (memory[memory_size - POST_FIELD_GUARD_BYTES + i] != (u8) ~0)
			return false;
	}

	/* All verifications passed. */
	return true;
}

/* Initialize a delta index page to refer to a supplied page. */
int initialize_delta_index_page(struct delta_index_page *delta_index_page,
				u64 expected_nonce,
				unsigned int mean_delta,
				unsigned int payload_bits,
				u8 *memory,
				size_t memory_size)
{
	u64 nonce;
	u64 vcn;
	u64 first_list;
	u64 list_count;
	struct delta_page_header *header = (struct delta_page_header *) memory;
	struct delta_zone *delta_zone = &delta_index_page->delta_zone;
	const u8 *nonce_addr = (const u8 *) &header->nonce;
	const u8 *vcn_addr = (const u8 *) &header->virtual_chapter_number;
	const u8 *first_list_addr = (const u8 *) &header->first_list;
	const u8 *list_count_addr = (const u8 *) &header->list_count;

	/* First assume that the header is little endian. */
	nonce = get_unaligned_le64(nonce_addr);
	vcn = get_unaligned_le64(vcn_addr);
	first_list = get_unaligned_le16(first_list_addr);
	list_count = get_unaligned_le16(list_count_addr);
	if (!verify_delta_index_page(nonce, list_count, expected_nonce, memory, memory_size)) {
		/* If that fails, try big endian. */
		nonce = get_unaligned_be64(nonce_addr);
		vcn = get_unaligned_be64(vcn_addr);
		first_list = get_unaligned_be16(first_list_addr);
		list_count = get_unaligned_be16(list_count_addr);
		if (!verify_delta_index_page(nonce,
					     list_count,
					     expected_nonce,
					     memory,
					     memory_size))
			/*
			 * Both attempts failed. Do not log this as an error, because it can happen
			 * during a rebuild if we haven't written the entire volume at least once.
			 */
			return UDS_CORRUPT_DATA;
	}

	delta_index_page->delta_index.delta_zones = delta_zone;
	delta_index_page->delta_index.zone_count = 1;
	delta_index_page->delta_index.list_count = list_count;
	delta_index_page->delta_index.lists_per_zone = list_count;
	delta_index_page->delta_index.mutable = false;
	delta_index_page->delta_index.tag = 'p';
	delta_index_page->virtual_chapter_number = vcn;
	delta_index_page->lowest_list_number = first_list;
	delta_index_page->highest_list_number = first_list + list_count - 1;

	compute_coding_constants(mean_delta,
				 &delta_zone->min_bits,
				 &delta_zone->min_keys,
				 &delta_zone->incr_keys);
	delta_zone->value_bits = payload_bits;
	delta_zone->memory = memory;
	delta_zone->delta_lists = NULL;
	delta_zone->new_offsets = NULL;
	delta_zone->buffered_writer = NULL;
	delta_zone->size = memory_size;
	delta_zone->rebalance_time = 0;
	delta_zone->rebalance_count = 0;
	delta_zone->record_count = 0;
	delta_zone->collision_count = 0;
	delta_zone->discard_count = 0;
	delta_zone->overflow_count = 0;
	delta_zone->first_list = 0;
	delta_zone->list_count = list_count;
	delta_zone->tag = 'p';

	return UDS_SUCCESS;
}

/* Read a large bit field from an arbitrary bit boundary. */
static inline u64 get_big_field(const u8 *memory, u64 offset, int size)
{
	const void *addr = memory + offset / BITS_PER_BYTE;

	return (get_unaligned_le64(addr) >> (offset % BITS_PER_BYTE)) & ((1UL << size) - 1);
}

/* Write a large bit field to an arbitrary bit boundary. */
static inline void set_big_field(u64 value, u8 *memory, u64 offset, int size)
{
	void *addr = memory + offset / BITS_PER_BYTE;
	int shift = offset % BITS_PER_BYTE;
	u64 data = get_unaligned_le64(addr);

	data &= ~(((1UL << size) - 1) << shift);
	data |= value << shift;
	put_unaligned_le64(data, addr);
}

/* Set a sequence of bits to all zeros. */
static inline void set_zero(u8 *memory, u64 offset, int size)
{
	if (size > 0) {
		u8 *addr = memory + offset / BITS_PER_BYTE;
		int shift = offset % BITS_PER_BYTE;
		int count = size + shift > BITS_PER_BYTE ? BITS_PER_BYTE - shift : size;

		*addr++ &= ~(((1 << count) - 1) << shift);
		for (size -= count; size > BITS_PER_BYTE; size -= BITS_PER_BYTE)
			*addr++ = 0;

		if (size > 0)
			*addr &= 0xFF << size;
	}
}

/*
 * Move several bits from a higher to a lower address, moving the lower addressed bits first. The
 * size and memory offsets are measured in bits.
 */
static void move_bits_down(const u8 *from, u64 from_offset, u8 *to, u64 to_offset, int size)
{
	const u8 *source;
	u8 *destination;
	int offset;
	int count;
	u64 field;

	/* Start by moving one field that ends on a to int boundary. */
	count = (MAX_BIG_FIELD_BITS - ((to_offset + MAX_BIG_FIELD_BITS) % U32_BITS));
	field = get_big_field(from, from_offset, count);
	set_big_field(field, to, to_offset, count);
	from_offset += count;
	to_offset += count;
	size -= count;

	/* Now do the main loop to copy 32 bit chunks that are int-aligned at the destination. */
	offset = from_offset % U32_BITS;
	source = from + (from_offset - offset) / BITS_PER_BYTE;
	destination = to + to_offset / BITS_PER_BYTE;
	while (size > MAX_BIG_FIELD_BITS) {
		put_unaligned_le32(get_unaligned_le64(source) >> offset, destination);
		source += sizeof(u32);
		destination += sizeof(u32);
		from_offset += U32_BITS;
		to_offset += U32_BITS;
		size -= U32_BITS;
	}

	/* Finish up by moving any remaining bits. */
	if (size > 0) {
		field = get_big_field(from, from_offset, size);
		set_big_field(field, to, to_offset, size);
	}
}

/*
 * Move several bits from a lower to a higher address, moving the higher addressed bits first. The
 * size and memory offsets are measured in bits.
 */
static void move_bits_up(const u8 *from, u64 from_offset, u8 *to, u64 to_offset, int size)
{
	const u8 *source;
	u8 *destination;
	int offset;
	int count;
	u64 field;

	/* Start by moving one field that begins on a destination int boundary. */
	count = (to_offset + size) % U32_BITS;
	if (count > 0) {
		size -= count;
		field = get_big_field(from, from_offset + size, count);
		set_big_field(field, to, to_offset + size, count);
	}

	/* Now do the main loop to copy 32 bit chunks that are int-aligned at the destination. */
	offset = (from_offset + size) % U32_BITS;
	source = from + (from_offset + size - offset) / BITS_PER_BYTE;
	destination = to + (to_offset + size) / BITS_PER_BYTE;
	while (size > MAX_BIG_FIELD_BITS) {
		source -= sizeof(u32);
		destination -= sizeof(u32);
		size -= U32_BITS;
		put_unaligned_le32(get_unaligned_le64(source) >> offset, destination);
	}

	/* Finish up by moving any remaining bits. */
	if (size > 0) {
		field = get_big_field(from, from_offset, size);
		set_big_field(field, to, to_offset, size);
	}
}

/*
 * Move bits from one field to another. When the fields overlap, behave as if we first move all the
 * bits from the source to a temporary value, and then move all the bits from the temporary value
 * to the destination. The size and memory offsets are measured in bits.
 */
EXTERNAL_STATIC void move_bits(const u8 *from, u64 from_offset, u8 *to, u64 to_offset, int size)
{
	u64 field;

	/* A small move doesn't require special handling. */
	if (size <= MAX_BIG_FIELD_BITS) {
		if (size > 0) {
			field = get_big_field(from, from_offset, size);
			set_big_field(field, to, to_offset, size);
		}

		return;
	}

	if (from_offset > to_offset)
		move_bits_down(from, from_offset, to, to_offset, size);
	else
		move_bits_up(from, from_offset, to, to_offset, size);
}

/*
 * Pack delta lists from a mutable delta index into an immutable delta index page. A range of delta
 * lists (starting with a specified list index) is copied from the mutable delta index into a
 * memory page used in the immutable index. The number of lists copied onto the page is returned in
 * list_count.
 */
int pack_delta_index_page(const struct delta_index *delta_index,
			  u64 header_nonce,
			  u8 *memory,
			  size_t memory_size,
			  u64 virtual_chapter_number,
			  unsigned int first_list,
			  unsigned int *list_count)
{
	const struct delta_zone *delta_zone;
	struct delta_list *delta_lists;
	unsigned int max_lists;
	unsigned int n_lists = 0;
	unsigned int offset;
	unsigned int i;
	int free_bits;
	int bits;
	struct delta_page_header *header;

	delta_zone = &delta_index->delta_zones[0];
	delta_lists = &delta_zone->delta_lists[first_list + 1];
	max_lists = delta_index->list_count - first_list;

	/*
	 * Compute how many lists will fit on the page. Subtract the size of the fixed header, one
	 * delta list offset, and the guard bytes from the page size to determine how much space is
	 * available for delta lists.
	 */
	free_bits = memory_size * BITS_PER_BYTE;
	free_bits -= get_immutable_header_offset(1);
	free_bits -= POST_FIELD_GUARD_BYTES * BITS_PER_BYTE;
	if (free_bits < IMMUTABLE_HEADER_SIZE)
		/* This page is too small to store any delta lists. */
		return uds_log_error_strerror(UDS_OVERFLOW,
					      "Chapter Index Page of %zu bytes is too small",
					      memory_size);

	while (n_lists < max_lists) {
		/* Each list requires a delta list offset and the list data. */
		bits = IMMUTABLE_HEADER_SIZE + delta_lists[n_lists].size;
		if (bits > free_bits)
			break;

		n_lists++;
		free_bits -= bits;
	}

	*list_count = n_lists;

	header = (struct delta_page_header *) memory;
	put_unaligned_le64(header_nonce, (u8 *) &header->nonce);
	put_unaligned_le64(virtual_chapter_number, (u8 *) &header->virtual_chapter_number);
	put_unaligned_le16(first_list, (u8 *) &header->first_list);
	put_unaligned_le16(n_lists, (u8 *) &header->list_count);

	/* Construct the delta list offset table. */
	offset = get_immutable_header_offset(n_lists + 1);
	set_immutable_start(memory, 0, offset);
	for (i = 0; i < n_lists; i++) {
		offset += delta_lists[i].size;
		set_immutable_start(memory, i + 1, offset);
	}

	/* Copy the delta list data onto the memory page. */
	for (i = 0; i < n_lists; i++)
		move_bits(delta_zone->memory,
			  delta_lists[i].start,
			  memory,
			  get_immutable_start(memory, i),
			  delta_lists[i].size);

	/* Set all the bits in the guard bytes. */
	memset(memory + memory_size - POST_FIELD_GUARD_BYTES, ~0, POST_FIELD_GUARD_BYTES);
	return UDS_SUCCESS;
}

#ifdef TEST_INTERNAL
/*
 * Given a delta index page written in little endian order, convert it into a delta index page
 * written in big endian order. Used to test old formats.
 */
void swap_delta_index_page_endianness(u8 *memory)
{
	struct delta_page_header *header = (struct delta_page_header *) memory;
	const u8 *nonce_addr = (const u8 *) &header->nonce;
	const u8 *vcn_addr = (const u8 *) &header->virtual_chapter_number;
	const u8 *first_list_addr = (const u8 *) &header->first_list;
	const u8 *list_count_addr = (const u8 *) &header->list_count;
	u64 nonce = get_unaligned_le64(nonce_addr);
	u64 vcn = get_unaligned_le64(vcn_addr);
	u16 first_list = get_unaligned_le16(first_list_addr);
	u16 list_count = get_unaligned_le16(list_count_addr);

	put_unaligned_be64(nonce, (u8 *) &header->nonce);
	put_unaligned_be64(vcn, (u8 *) &header->virtual_chapter_number);
	put_unaligned_be16(first_list, (u8 *) &header->first_list);
	put_unaligned_be16(list_count, (u8 *) &header->list_count);
}

#endif /* TEST_INTERNAL */
static int __must_check
decode_delta_index_header(struct buffer *buffer, struct delta_index_header *header)
{
	int result;

	result = get_bytes_from_buffer(buffer, MAGIC_SIZE, &header->magic);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u32_le_from_buffer(buffer, &header->zone_number);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u32_le_from_buffer(buffer, &header->zone_count);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u32_le_from_buffer(buffer, &header->first_list);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u32_le_from_buffer(buffer, &header->list_count);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &header->record_count);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &header->collision_count);
	if (result != UDS_SUCCESS)
		return result;

	return ASSERT(content_length(buffer) == 0,
		      "%zu bytes decoded of %zu expected",
		      buffer_length(buffer) - content_length(buffer),
		      buffer_length(buffer));
}

static int __must_check
read_delta_index_header(struct buffered_reader *reader, struct delta_index_header *header)
{
	int result;
	struct buffer *buffer;

	result = make_buffer(sizeof(*header), &buffer);
	if (result != UDS_SUCCESS)
		return result;

	result = read_from_buffered_reader(reader,
					   get_buffer_contents(buffer),
					   buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return uds_log_warning_strerror(result,
						"failed to read delta index header");
	}

	result = reset_buffer_end(buffer, buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = decode_delta_index_header(buffer, header);
	free_buffer(UDS_FORGET(buffer));
	return result;
}

/* Compute the new offsets of the delta lists. */
static void compute_new_list_offsets(struct delta_zone *delta_zone,
				     unsigned int growing_index,
				     size_t growing_size,
				     size_t used_space)
{
	size_t spacing;
	unsigned int i;
	struct delta_list *delta_lists = delta_zone->delta_lists;
	unsigned int tail_guard_index = delta_zone->list_count + 1;

	spacing = (delta_zone->size - used_space) / delta_zone->list_count;
	delta_zone->new_offsets[0] = 0;
	for (i = 0; i <= delta_zone->list_count; i++) {
		delta_zone->new_offsets[i + 1] =
			(delta_zone->new_offsets[i] +
			 get_delta_list_byte_size(&delta_lists[i]) + spacing);
		delta_zone->new_offsets[i] *= BITS_PER_BYTE;
		delta_zone->new_offsets[i] += delta_lists[i].start % BITS_PER_BYTE;
		if (i == 0)
			delta_zone->new_offsets[i + 1] -= spacing / 2;
		if (i + 1 == growing_index)
			delta_zone->new_offsets[i + 1] += growing_size;
	}

	delta_zone->new_offsets[tail_guard_index] =
		(delta_zone->size * BITS_PER_BYTE - delta_lists[tail_guard_index].size);
}

static void rebalance_lists(struct delta_zone *delta_zone)
{
	struct delta_list *delta_lists;
	unsigned int i;
	size_t used_space = 0;

	/* Extend and balance memory to receive the delta lists */
	delta_lists = delta_zone->delta_lists;
	for (i = 0; i <= delta_zone->list_count + 1; i++)
		used_space += get_delta_list_byte_size(&delta_lists[i]);

	compute_new_list_offsets(delta_zone, 0, 0, used_space);
	for (i = 1; i <= delta_zone->list_count + 1; i++)
		delta_lists[i].start = delta_zone->new_offsets[i];
}

/* Start restoring a delta index from multiple input streams. */
int start_restoring_delta_index(struct delta_index *delta_index,
				struct buffered_reader **buffered_readers,
				unsigned int reader_count)
{
	int result;
	unsigned int zone_count = reader_count;
	unsigned long record_count = 0;
	unsigned long collision_count = 0;
	unsigned int first_list[MAX_ZONES];
	unsigned int list_count[MAX_ZONES];
	unsigned int z;
	unsigned int list_next = 0;
	const struct delta_zone *delta_zone;

	/* Read and validate each header. */
	for (z = 0; z < zone_count; z++) {
		struct delta_index_header header;

		result = read_delta_index_header(buffered_readers[z], &header);
		if (result != UDS_SUCCESS)
			return uds_log_warning_strerror(result,
							"failed to read delta index header");

		if (memcmp(header.magic, DELTA_INDEX_MAGIC, MAGIC_SIZE) != 0)
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"delta index file has bad magic number");

		if (zone_count != header.zone_count)
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"delta index files contain mismatched zone counts (%u,%u)",
							zone_count,
							header.zone_count);

		if (header.zone_number >= zone_count)
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"delta index files contains zone %u of %u zones",
							header.zone_number,
							zone_count);
		if (header.zone_number != z)
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"delta index zone %u found in slot %u",
							header.zone_number,
							z);

		first_list[z] = header.first_list;
		list_count[z] = header.list_count;
		record_count += header.record_count;
		collision_count += header.collision_count;

		if (first_list[z] != list_next)
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"delta index file for zone %u starts with list %u instead of list %u",
							z,
							first_list[z],
							list_next);

		list_next += list_count[z];
	}

	if (list_next != delta_index->list_count)
		return uds_log_warning_strerror(UDS_CORRUPT_DATA,
						"delta index files contain %u delta lists instead of %u delta lists",
						list_next,
						delta_index->list_count);

	if (collision_count > record_count)
		return uds_log_warning_strerror(UDS_CORRUPT_DATA,
						"delta index files contain %ld collisions and %ld records",
						collision_count,
						record_count);

	reset_delta_index(delta_index);
	delta_index->delta_zones[0].record_count = record_count;
	delta_index->delta_zones[0].collision_count = collision_count;

	/* Read the delta lists and distribute them to the proper zones. */
	for (z = 0; z < zone_count; z++) {
		unsigned int i;

		delta_index->load_lists[z] = 0;
		for (i = 0; i < list_count[z]; i++) {
			u16 delta_list_size;
			unsigned int list_number;
			unsigned int zone_number;
			u8 size_data[sizeof(u16)];

			result = read_from_buffered_reader(buffered_readers[z],
							   size_data,
							   sizeof(size_data));
			if (result != UDS_SUCCESS)
				return uds_log_warning_strerror(result,
								"failed to read delta index size");

			delta_list_size = get_unaligned_le16(size_data);
			if (delta_list_size > 0)
				delta_index->load_lists[z] += 1;

			list_number = first_list[z] + i;
			zone_number = list_number / delta_index->lists_per_zone;
			delta_zone = &delta_index->delta_zones[zone_number];
			list_number -= delta_zone->first_list;
			delta_zone->delta_lists[list_number + 1].size = delta_list_size;
		}
	}

	/* Prepare each zone to start receiving the delta list data. */
	for (z = 0; z < delta_index->zone_count; z++)
		rebalance_lists(&delta_index->delta_zones[z]);
	return UDS_SUCCESS;
}

static int restore_delta_list_to_zone(struct delta_zone *delta_zone,
				      const struct delta_list_save_info *save_info,
				      const u8 *data)
{
	struct delta_list *delta_list;
	unsigned int bit_count;
	unsigned int byte_count;
	unsigned int list_number = save_info->index - delta_zone->first_list;

	if (list_number >= delta_zone->list_count)
		return uds_log_warning_strerror(UDS_CORRUPT_DATA,
						"invalid delta list number %u not in range [%u,%u)",
						save_info->index,
						delta_zone->first_list,
						delta_zone->first_list +
						delta_zone->list_count);

	delta_list = &delta_zone->delta_lists[list_number + 1];
	if (delta_list->size == 0)
		return uds_log_warning_strerror(UDS_CORRUPT_DATA,
						"unexpected delta list number %u",
						save_info->index);

	bit_count = (unsigned int) save_info->bit_offset + delta_list->size;
	byte_count = BITS_TO_BYTES(bit_count);
	if (save_info->byte_count != byte_count)
		return uds_log_warning_strerror(UDS_CORRUPT_DATA,
						"unexpected delta list size %u != %u",
						save_info->byte_count,
						byte_count);

	move_bits(data,
		  save_info->bit_offset,
		  delta_zone->memory,
		  delta_list->start,
		  delta_list->size);
	return UDS_SUCCESS;
}

static int restore_delta_list_data(struct delta_index *delta_index,
				   unsigned int load_zone,
				   struct buffered_reader *buffered_reader,
				   u8 *data)
{
	int result;
	struct delta_list_save_info save_info;
	u8 buffer[sizeof(struct delta_list_save_info)];
	unsigned int new_zone;

	result = read_from_buffered_reader(buffered_reader, buffer, sizeof(buffer));
	if (result != UDS_SUCCESS)
		return uds_log_warning_strerror(result, "failed to read delta list data");

	save_info = (struct delta_list_save_info) {
		.tag = buffer[0],
		.bit_offset = buffer[1],
		.byte_count = get_unaligned_le16(&buffer[2]),
		.index = get_unaligned_le32(&buffer[4]),
	};

	if ((save_info.bit_offset >= BITS_PER_BYTE) ||
	    (save_info.byte_count > DELTA_LIST_MAX_BYTE_COUNT))
		return uds_log_warning_strerror(UDS_CORRUPT_DATA, "corrupt delta list data");

	/* Make sure the data is intended for this delta index. */
	if (save_info.tag != delta_index->tag)
		return UDS_CORRUPT_DATA;

	if (save_info.index >= delta_index->list_count)
		return uds_log_warning_strerror(UDS_CORRUPT_DATA,
						"invalid delta list number %u of %u",
						save_info.index,
						delta_index->list_count);

	result = read_from_buffered_reader(buffered_reader, data, save_info.byte_count);
	if (result != UDS_SUCCESS)
		return uds_log_warning_strerror(result,
						"failed to read delta list data");

	delta_index->load_lists[load_zone] -= 1;
	new_zone = save_info.index / delta_index->lists_per_zone;
	return restore_delta_list_to_zone(&delta_index->delta_zones[new_zone], &save_info, data);
}

/* Restore delta lists from saved data. */
int finish_restoring_delta_index(struct delta_index *delta_index,
				 struct buffered_reader **buffered_readers,
				 unsigned int reader_count)
{
	int result;
	int saved_result = UDS_SUCCESS;
	unsigned int z;
	u8 *data;

	result = UDS_ALLOCATE(DELTA_LIST_MAX_BYTE_COUNT, u8, __func__, &data);
	if (result != UDS_SUCCESS)
		return result;

	for (z = 0; z < reader_count; z++) {
		while (delta_index->load_lists[z] > 0) {
			result = restore_delta_list_data(delta_index,
							 z,
							 buffered_readers[z],
							 data);
			if (result != UDS_SUCCESS) {
				saved_result = result;
				break;
			}
		}
	}

	UDS_FREE(data);
	return saved_result;
}

int check_guard_delta_lists(struct buffered_reader **buffered_readers, unsigned int reader_count)
{
	int result;
	unsigned int z;
	u8 buffer[sizeof(struct delta_list_save_info)];

	for (z = 0; z < reader_count; z++) {
		result = read_from_buffered_reader(buffered_readers[z], buffer, sizeof(buffer));
		if (result != UDS_SUCCESS)
			return result;

		if (buffer[0] != 'z')
			return UDS_CORRUPT_DATA;
	}

	return UDS_SUCCESS;
}

static int __must_check
encode_delta_index_header(struct buffer *buffer, struct delta_index_header *header)
{
	int result;

	result = put_bytes(buffer, MAGIC_SIZE, DELTA_INDEX_MAGIC);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u32_le_into_buffer(buffer, header->zone_number);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u32_le_into_buffer(buffer, header->zone_count);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u32_le_into_buffer(buffer, header->first_list);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u32_le_into_buffer(buffer, header->list_count);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, header->record_count);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, header->collision_count);
	if (result != UDS_SUCCESS)
		return result;

	return ASSERT(content_length(buffer) == sizeof(*header),
		      "%zu bytes encoded of %zu expected",
		      content_length(buffer),
		      sizeof(*header));
}

static int flush_delta_list(struct delta_zone *zone, unsigned int flush_index)
{
	struct delta_list *delta_list;
	u8 buffer[sizeof(struct delta_list_save_info)];
	int result;

	delta_list = &zone->delta_lists[flush_index + 1];

	buffer[0] = zone->tag;
	buffer[1] = delta_list->start % BITS_PER_BYTE;
	put_unaligned_le16(get_delta_list_byte_size(delta_list), &buffer[2]);
	put_unaligned_le32(zone->first_list + flush_index, &buffer[4]);

	result = write_to_buffered_writer(zone->buffered_writer, buffer, sizeof(buffer));
	if (result != UDS_SUCCESS) {
		uds_log_warning_strerror(result, "failed to write delta list memory");
		return result;
	}

	result = write_to_buffered_writer(zone->buffered_writer,
					  zone->memory + get_delta_list_byte_start(delta_list),
					  get_delta_list_byte_size(delta_list));
	if (result != UDS_SUCCESS)
		uds_log_warning_strerror(result, "failed to write delta list memory");

	return result;
}

/* Start saving a delta index zone to a buffered output stream. */
int start_saving_delta_index(const struct delta_index *delta_index,
			     unsigned int zone_number,
			     struct buffered_writer *buffered_writer)
{
	int result;
	unsigned int i;
	struct buffer *buffer;
	struct delta_zone *delta_zone;
	struct delta_index_header header;

	delta_zone = &delta_index->delta_zones[zone_number];
	memcpy(header.magic, DELTA_INDEX_MAGIC, MAGIC_SIZE);
	header.zone_number = zone_number;
	header.zone_count = delta_index->zone_count;
	header.first_list = delta_zone->first_list;
	header.list_count = delta_zone->list_count;
	header.record_count = delta_zone->record_count;
	header.collision_count = delta_zone->collision_count;

	result = make_buffer(sizeof(struct delta_index_header), &buffer);
	if (result != UDS_SUCCESS)
		return result;

	result = encode_delta_index_header(buffer, &header);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(buffered_writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS)
		return uds_log_warning_strerror(result, "failed to write delta index header");

	for (i = 0; i < delta_zone->list_count; i++) {
		u8 data[sizeof(u16)];
		struct delta_list *delta_list;

		delta_list = &delta_zone->delta_lists[i + 1];
		put_unaligned_le16(delta_list->size, data);
		result = write_to_buffered_writer(buffered_writer, data, sizeof(data));
		if (result != UDS_SUCCESS)
			return uds_log_warning_strerror(result, "failed to write delta list size");
	}

	delta_zone->buffered_writer = buffered_writer;
	return UDS_SUCCESS;
}

int finish_saving_delta_index(const struct delta_index *delta_index, unsigned int zone_number)
{
	int result;
	int first_error = UDS_SUCCESS;
	unsigned int i;
	struct delta_zone *delta_zone;
	struct delta_list *delta_list;

	delta_zone = &delta_index->delta_zones[zone_number];
	for (i = 0; i < delta_zone->list_count; i++) {
		delta_list = &delta_zone->delta_lists[i + 1];
		if (delta_list->size > 0) {
			result = flush_delta_list(delta_zone, i);
			if ((result != UDS_SUCCESS) && (first_error == UDS_SUCCESS))
				first_error = result;
		}
	}

	delta_zone->buffered_writer = NULL;
	return first_error;
}

int write_guard_delta_list(struct buffered_writer *buffered_writer)
{
	int result;
	u8 buffer[sizeof(struct delta_list_save_info)];

	memset(buffer, 0, sizeof(struct delta_list_save_info));
	buffer[0] = 'z';

	result = write_to_buffered_writer(buffered_writer, buffer, sizeof(buffer));
	if (result != UDS_SUCCESS)
		uds_log_warning_strerror(result, "failed to write guard delta list");

	return UDS_SUCCESS;
}

size_t compute_delta_index_save_bytes(unsigned int list_count, size_t memory_size)
{
	/* One zone will use at least as much memory as other zone counts. */
	return sizeof(struct delta_index_header) +
	       list_count * (sizeof(struct delta_list_save_info) + 1) +
	       get_zone_memory_size(1, memory_size);
}

static int assert_not_at_end(const struct delta_index_entry *delta_entry)
{
	int result = ASSERT(!delta_entry->at_end,
			    "operation is invalid because the list entry is at the end of the delta list");
	if (result != UDS_SUCCESS)
		result = UDS_BAD_STATE;

	return result;
}

/*
 * Prepare to search for an entry in the specified delta list.
 *
 * This is always the first function to be called when dealing with delta index entries. It is
 * always followed by calls to next_delta_index_entry() to iterate through a delta list. The fields
 * of the delta_index_entry argument will be set up for iteration, but will not contain an entry
 * from the list.
 */
int start_delta_index_search(const struct delta_index *delta_index,
			     unsigned int list_number,
			     unsigned int key,
			     struct delta_index_entry *delta_entry)
{
	int result;
	unsigned int zone_number;
	struct delta_zone *delta_zone;
	struct delta_list *delta_list;

	result = ASSERT((list_number < delta_index->list_count),
			"Delta list number (%u) is out of range (%u)",
			list_number,
			delta_index->list_count);
	if (result != UDS_SUCCESS)
		return UDS_CORRUPT_DATA;

	zone_number = list_number / delta_index->lists_per_zone;
	delta_zone = &delta_index->delta_zones[zone_number];
	list_number -= delta_zone->first_list;
	result = ASSERT((list_number < delta_zone->list_count),
			"Delta list number (%u) is out of range (%u) for zone (%u)",
			list_number,
			delta_zone->list_count,
			zone_number);
	if (result != UDS_SUCCESS)
		return UDS_CORRUPT_DATA;

	if (delta_index->mutable) {
		delta_list = &delta_zone->delta_lists[list_number + 1];
	} else {
		unsigned int end_offset;

		/*
		 * Translate the immutable delta list header into a temporary
		 * full delta list header.
		 */
		delta_list = &delta_entry->temp_delta_list;
		delta_list->start = get_immutable_start(delta_zone->memory, list_number);
		end_offset = get_immutable_start(delta_zone->memory, list_number + 1);
		delta_list->size = end_offset - delta_list->start;
		delta_list->save_key = 0;
		delta_list->save_offset = 0;
	}

	if (key > delta_list->save_key) {
		delta_entry->key = delta_list->save_key;
		delta_entry->offset = delta_list->save_offset;
	} else {
		delta_entry->key = 0;
		delta_entry->offset = 0;
		if (key == 0)
			/*
			 * This usually means we're about to walk the entire delta list, so get all
			 * of it into the CPU cache.
			 */
			prefetch_range(&delta_zone->memory[delta_list->start / BITS_PER_BYTE],
				       delta_list->size / BITS_PER_BYTE,
				       false);
	}

	delta_entry->at_end = false;
	delta_entry->delta_zone = delta_zone;
	delta_entry->delta_list = delta_list;
	delta_entry->entry_bits = 0;
	delta_entry->is_collision = false;
	delta_entry->list_number = list_number;
	delta_entry->list_overflow = false;
	delta_entry->value_bits = delta_zone->value_bits;
	return UDS_SUCCESS;
}

static inline u64 get_delta_entry_offset(const struct delta_index_entry *delta_entry)
{
	return delta_entry->delta_list->start + delta_entry->offset;
}

/*
 * Decode a delta index entry delta value. The delta_index_entry basically describes the previous
 * list entry, and has had its offset field changed to point to the subsequent entry. We decode the
 * bit stream and update the delta_list_entry to describe the entry.
 */
static inline void decode_delta(struct delta_index_entry *delta_entry)
{
	int key_bits;
	unsigned int delta;
	const struct delta_zone *delta_zone = delta_entry->delta_zone;
	const u8 *memory = delta_zone->memory;
	u64 delta_offset = get_delta_entry_offset(delta_entry) + delta_entry->value_bits;
	const u8 *addr = memory + delta_offset / BITS_PER_BYTE;
	int offset = delta_offset % BITS_PER_BYTE;
	u32 data = get_unaligned_le32(addr) >> offset;

	addr += sizeof(u32);
	key_bits = delta_zone->min_bits;
	delta = data & ((1 << key_bits) - 1);
	if (delta >= delta_zone->min_keys) {
		data >>= key_bits;
		if (data == 0) {
			key_bits = sizeof(u32) * BITS_PER_BYTE - offset;
			while ((data = get_unaligned_le32(addr)) == 0) {
				addr += sizeof(u32);
				key_bits += sizeof(u32) * BITS_PER_BYTE;
			}
		}
		key_bits += ffs(data);
		delta += ((key_bits - delta_zone->min_bits - 1) * delta_zone->incr_keys);
	}
	delta_entry->delta = delta;
	delta_entry->key += delta;

	/* Check for a collision, a delta of zero after the start. */
	if (unlikely((delta == 0) && (delta_entry->offset > 0))) {
		delta_entry->is_collision = true;
		delta_entry->entry_bits = delta_entry->value_bits + key_bits + COLLISION_BITS;
	} else {
		delta_entry->is_collision = false;
		delta_entry->entry_bits = delta_entry->value_bits + key_bits;
	}
}

noinline int next_delta_index_entry(struct delta_index_entry *delta_entry)
{
	int result;
	const struct delta_list *delta_list;
	unsigned int next_offset;
	unsigned int size;

	result = assert_not_at_end(delta_entry);
	if (result != UDS_SUCCESS)
		return result;

	delta_list = delta_entry->delta_list;
	delta_entry->offset += delta_entry->entry_bits;
	size = delta_list->size;
	if (unlikely(delta_entry->offset >= size)) {
		delta_entry->at_end = true;
		delta_entry->delta = 0;
		delta_entry->is_collision = false;
		result = ASSERT((delta_entry->offset == size),
				"next offset past end of delta list");
		if (result != UDS_SUCCESS)
			result = UDS_CORRUPT_DATA;

		return result;
	}

	decode_delta(delta_entry);

	next_offset = delta_entry->offset + delta_entry->entry_bits;
	if (next_offset > size) {
		/*
		 * This is not an assertion because validate_chapter_index_page() wants to handle
		 * this error.
		 */
		uds_log_warning("Decoded past the end of the delta list");
		return UDS_CORRUPT_DATA;
	}

	return UDS_SUCCESS;
}

int remember_delta_index_offset(const struct delta_index_entry *delta_entry)
{
	int result;
	struct delta_list *delta_list = delta_entry->delta_list;

	result = ASSERT(!delta_entry->is_collision, "entry is not a collision");
	if (result != UDS_SUCCESS)
		return result;

	delta_list->save_key = delta_entry->key - delta_entry->delta;
	delta_list->save_offset = delta_entry->offset;
	return UDS_SUCCESS;
}

static void set_delta(struct delta_index_entry *delta_entry, unsigned int delta)
{
	const struct delta_zone *delta_zone = delta_entry->delta_zone;
	int key_bits = (delta_zone->min_bits +
			((delta_zone->incr_keys -
			  delta_zone->min_keys + delta) /
			 delta_zone->incr_keys));

	delta_entry->delta = delta;
	delta_entry->entry_bits = delta_entry->value_bits + key_bits;
}

static void get_collision_name(const struct delta_index_entry *entry, u8 *name)
{
	u64 offset = get_delta_entry_offset(entry) + entry->entry_bits - COLLISION_BITS;
	const u8 *addr = entry->delta_zone->memory + offset / BITS_PER_BYTE;
	int size = COLLISION_BYTES;
	int shift = offset % BITS_PER_BYTE;

	while (--size >= 0)
		*name++ = get_unaligned_le16(addr++) >> shift;
}

static void set_collision_name(const struct delta_index_entry *entry, const u8 *name)
{
	u64 offset = get_delta_entry_offset(entry) + entry->entry_bits - COLLISION_BITS;
	u8 *addr = entry->delta_zone->memory + offset / BITS_PER_BYTE;
	int size = COLLISION_BYTES;
	int shift = offset % BITS_PER_BYTE;
	u16 mask = ~((u16) 0xFF << shift);
	u16 data;

	while (--size >= 0) {
		data = (get_unaligned_le16(addr) & mask) | (*name++ << shift);
		put_unaligned_le16(data, addr++);
	}
}

int get_delta_index_entry(const struct delta_index *delta_index,
			  unsigned int list_number,
			  unsigned int key,
			  const u8 *name,
			  struct delta_index_entry *delta_entry)
{
	int result;

	result = start_delta_index_search(delta_index, list_number, key, delta_entry);
	if (result != UDS_SUCCESS)
		return result;

	do {
		result = next_delta_index_entry(delta_entry);
		if (result != UDS_SUCCESS)
			return result;
	} while (!delta_entry->at_end && (key > delta_entry->key));

	result = remember_delta_index_offset(delta_entry);
	if (result != UDS_SUCCESS)
		return result;

	if (!delta_entry->at_end && (key == delta_entry->key)) {
		struct delta_index_entry collision_entry = *delta_entry;

		for (;;) {
			u8 full_name[COLLISION_BYTES];

			result = next_delta_index_entry(&collision_entry);
			if (result != UDS_SUCCESS)
				return result;

			if (collision_entry.at_end || !collision_entry.is_collision)
				break;

			get_collision_name(&collision_entry, full_name);
			if (memcmp(full_name, name, COLLISION_BYTES) == 0) {
				*delta_entry = collision_entry;
				break;
			}
		}
	}

	return UDS_SUCCESS;
}

int get_delta_entry_collision(const struct delta_index_entry *delta_entry, u8 *name)
{
	int result;

	result = assert_not_at_end(delta_entry);
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT(delta_entry->is_collision,
			"Cannot get full block name from a non-collision delta index entry");
	if (result != UDS_SUCCESS)
		return UDS_BAD_STATE;

	get_collision_name(delta_entry, name);
	return UDS_SUCCESS;
}

unsigned int get_delta_entry_value(const struct delta_index_entry *delta_entry)
{
	return get_field(delta_entry->delta_zone->memory,
			 get_delta_entry_offset(delta_entry),
			 delta_entry->value_bits);
}

static int assert_mutable_entry(const struct delta_index_entry *delta_entry)
{
	int result = ASSERT((delta_entry->delta_list != &delta_entry->temp_delta_list),
			    "delta index is mutable");
	if (result != UDS_SUCCESS)
		result = UDS_BAD_STATE;

	return result;
}

int set_delta_entry_value(const struct delta_index_entry *delta_entry, unsigned int value)
{
	int result;
	unsigned int value_mask = (1 << delta_entry->value_bits) - 1;

	result = assert_mutable_entry(delta_entry);
	if (result != UDS_SUCCESS)
		return result;

	result = assert_not_at_end(delta_entry);
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT((value & value_mask) == value,
			"Value (%u) being set in a delta index is too large (must fit in %u bits)",
			value,
			delta_entry->value_bits);
	if (result != UDS_SUCCESS)
		return UDS_INVALID_ARGUMENT;

	set_field(value,
		  delta_entry->delta_zone->memory,
		  get_delta_entry_offset(delta_entry),
		  delta_entry->value_bits);
	return UDS_SUCCESS;
}

/*
 * Extend the memory used by the delta lists by adding growing_size bytes before the list indicated
 * by growing_index, then rebalancing the lists in the new chunk.
 */
EXTERNAL_STATIC int
extend_delta_zone(struct delta_zone *delta_zone, unsigned int growing_index, size_t growing_size)
{
	ktime_t start_time;
	ktime_t end_time;
	struct delta_list *delta_lists;
	unsigned int i;
	size_t used_space;


	/* Calculate the amount of space that is or will be in use. */
	start_time = current_time_ns(CLOCK_MONOTONIC);
	delta_lists = delta_zone->delta_lists;
	used_space = growing_size;
	for (i = 0; i <= delta_zone->list_count + 1; i++)
		used_space += get_delta_list_byte_size(&delta_lists[i]);

	if (delta_zone->size < used_space)
		return UDS_OVERFLOW;

	/* Compute the new offsets of the delta lists. */
	compute_new_list_offsets(delta_zone, growing_index, growing_size, used_space);

	/*
	 * When we rebalance the delta list, we will include the end guard list in the rebalancing.
	 * It contains the end guard data, which must be copied.
	 */
	rebalance_delta_zone(delta_zone, 1, delta_zone->list_count + 1);
	end_time = current_time_ns(CLOCK_MONOTONIC);
	delta_zone->rebalance_count++;
	delta_zone->rebalance_time += ktime_sub(end_time, start_time);
	return UDS_SUCCESS;
}

static int insert_bits(struct delta_index_entry *delta_entry, int size)
{
	u64 free_before;
	u64 free_after;
	u64 source;
	u64 destination;
	u32 count;
	bool before_flag;
	u8 *memory;
	struct delta_zone *delta_zone = delta_entry->delta_zone;
	struct delta_list *delta_list = delta_entry->delta_list;
	/* Compute bits in use before and after the inserted bits. */
	u32 total_size = delta_list->size;
	u32 before_size = delta_entry->offset;
	u32 after_size = total_size - delta_entry->offset;

	if ((unsigned int) (total_size + size) > U16_MAX) {
		delta_entry->list_overflow = true;
		delta_zone->overflow_count++;
		return UDS_OVERFLOW;
	}

	/* Compute bits available before and after the delta list. */
	free_before = (delta_list[0].start - (delta_list[-1].start + delta_list[-1].size));
	free_after = (delta_list[1].start - (delta_list[0].start + delta_list[0].size));

	if (((unsigned int) size <= free_before) && ((unsigned int) size <= free_after)) {
		/*
		 * We have enough space to use either before or after the list. Select the smaller
		 * amount of data. If it is exactly the same, try to take from the larger amount of
		 * free space.
		 */
		if (before_size < after_size)
			before_flag = true;
		else if (after_size < before_size)
			before_flag = false;
		else
			before_flag = free_before > free_after;
	} else if ((unsigned int) size <= free_before) {
		/* There is space before but not after. */
		before_flag = true;
	} else if ((unsigned int) size <= free_after) {
		/* There is space after but not before. */
		before_flag = false;
	} else {
		/*
		 * Neither of the surrounding spaces is large enough for this request. Extend
		 * and/or rebalance the delta list memory choosing to move the least amount of
		 * data.
		 */
		int result;
		unsigned int growing_index = delta_entry->list_number + 1;

		before_flag = before_size < after_size;
		if (!before_flag)
			growing_index++;
		result = extend_delta_zone(delta_zone, growing_index, BITS_TO_BYTES(size));
		if (result != UDS_SUCCESS)
			return result;
	}

	delta_list->size += size;
	if (before_flag) {
		source = delta_list->start;
		destination = source - size;
		delta_list->start -= size;
		count = before_size;
	} else {
		source = delta_list->start + delta_entry->offset;
		destination = source + size;
		count = after_size;
	}

	memory = delta_zone->memory;
	move_bits(memory, source, memory, destination, count);
	return UDS_SUCCESS;
}

static void encode_delta(const struct delta_index_entry *delta_entry)
{
	unsigned int temp;
	unsigned int t1;
	unsigned int t2;
	u64 offset;
	const struct delta_zone *delta_zone = delta_entry->delta_zone;
	u8 *memory = delta_zone->memory;

	offset = get_delta_entry_offset(delta_entry) + delta_entry->value_bits;
	if (delta_entry->delta < delta_zone->min_keys) {
		set_field(delta_entry->delta, memory, offset, delta_zone->min_bits);
		return;
	}

	temp = delta_entry->delta - delta_zone->min_keys;
	t1 = (temp % delta_zone->incr_keys) + delta_zone->min_keys;
	t2 = temp / delta_zone->incr_keys;
	set_field(t1, memory, offset, delta_zone->min_bits);
	set_zero(memory, offset + delta_zone->min_bits, t2);
	set_field(1, memory, offset + delta_zone->min_bits + t2, 1);
}

static void encode_entry(const struct delta_index_entry *delta_entry,
			 unsigned int value,
			 const u8 *name)
{
	u8 *memory = delta_entry->delta_zone->memory;
	u64 offset = get_delta_entry_offset(delta_entry);

	set_field(value, memory, offset, delta_entry->value_bits);
	encode_delta(delta_entry);
	if (name != NULL)
		set_collision_name(delta_entry, name);
}

/*
 * Create a new entry in the delta index. If the entry is a collision, the full 256 bit name must
 * be provided.
 */
int put_delta_index_entry(struct delta_index_entry *delta_entry,
			  unsigned int key,
			  unsigned int value,
			  const u8 *name)
{
	int result;
	struct delta_zone *delta_zone;

	result = assert_mutable_entry(delta_entry);
	if (result != UDS_SUCCESS)
		return result;

	if (delta_entry->is_collision)
		/*
		 * The caller wants us to insert a collision entry onto a collision entry. This
		 * happens when we find a collision and attempt to add the name again to the index.
		 * This is normally a fatal error unless we are replaying a closed chapter while we
		 * are rebuilding a volume index.
		 */
		return UDS_DUPLICATE_NAME;

	if (delta_entry->offset < delta_entry->delta_list->save_offset) {
		/*
		 * The saved entry offset is after the new entry and will no longer be valid, so
		 * replace it with the insertion point.
		 */
		result = remember_delta_index_offset(delta_entry);
		if (result != UDS_SUCCESS)
			return result;
	}

	if (name != NULL) {
		/* Insert a collision entry which is placed after this entry. */
		result = assert_not_at_end(delta_entry);
		if (result != UDS_SUCCESS)
			return result;

		result = ASSERT((key == delta_entry->key), "incorrect key for collision entry");
		if (result != UDS_SUCCESS)
			return result;

		delta_entry->offset += delta_entry->entry_bits;
		set_delta(delta_entry, 0);
		delta_entry->is_collision = true;
		delta_entry->entry_bits += COLLISION_BITS;
		result = insert_bits(delta_entry, delta_entry->entry_bits);
	} else if (delta_entry->at_end) {
		/* Insert a new entry at the end of the delta list. */
		result = ASSERT((key >= delta_entry->key), "key past end of list");
		if (result != UDS_SUCCESS)
			return result;

		set_delta(delta_entry, key - delta_entry->key);
		delta_entry->key = key;
		delta_entry->at_end = false;
		result = insert_bits(delta_entry, delta_entry->entry_bits);
	} else {
		int old_entry_size;
		int additional_size;
		struct delta_index_entry next_entry;
		unsigned int next_value;

		/*
		 * Insert a new entry which requires the delta in the following entry to be
		 * updated.
		 */
		result = ASSERT((key < delta_entry->key), "key precedes following entry");
		if (result != UDS_SUCCESS)
			return result;

		result = ASSERT((key >= delta_entry->key - delta_entry->delta),
				"key effects following entry's delta");
		if (result != UDS_SUCCESS)
			return result;

		old_entry_size = delta_entry->entry_bits;
		next_entry = *delta_entry;
		next_value = get_delta_entry_value(&next_entry);
		set_delta(delta_entry, key - (delta_entry->key - delta_entry->delta));
		delta_entry->key = key;
		set_delta(&next_entry, next_entry.key - key);
		next_entry.offset += delta_entry->entry_bits;
		/* The two new entries are always bigger than the single entry being replaced. */
		additional_size = (delta_entry->entry_bits +
				   next_entry.entry_bits - old_entry_size);
		result = insert_bits(delta_entry, additional_size);
		if (result != UDS_SUCCESS)
			return result;

		encode_entry(&next_entry, next_value, NULL);
	}

	if (result != UDS_SUCCESS)
		return result;

	encode_entry(delta_entry, value, name);
	delta_zone = delta_entry->delta_zone;
	delta_zone->record_count++;
	delta_zone->collision_count += delta_entry->is_collision ? 1 : 0;
	return UDS_SUCCESS;
}

static void delete_bits(const struct delta_index_entry *delta_entry, int size)
{
	u64 source;
	u64 destination;
	u32 count;
	bool before_flag;
	struct delta_list *delta_list = delta_entry->delta_list;
	u8 *memory = delta_entry->delta_zone->memory;
	/* Compute bits retained before and after the deleted bits. */
	u32 total_size = delta_list->size;
	u32 before_size = delta_entry->offset;
	u32 after_size = total_size - delta_entry->offset - size;

	/*
	 * Determine whether to add to the available space either before or after the delta list.
	 * We prefer to move the least amount of data. If it is exactly the same, try to add to the
	 * smaller amount of free space.
	 */
	if (before_size < after_size) {
		before_flag = true;
	} else if (after_size < before_size) {
		before_flag = false;
	} else {
		u64 free_before =
			(delta_list[0].start - (delta_list[-1].start + delta_list[-1].size));
		u64 free_after =
			(delta_list[1].start - (delta_list[0].start + delta_list[0].size));

		before_flag = (free_before < free_after);
	}

	delta_list->size -= size;
	if (before_flag) {
		source = delta_list->start;
		destination = source + size;
		delta_list->start += size;
		count = before_size;
	} else {
		destination = delta_list->start + delta_entry->offset;
		source = destination + size;
		count = after_size;
	}

	move_bits(memory, source, memory, destination, count);
}

int remove_delta_index_entry(struct delta_index_entry *delta_entry)
{
	int result;
	struct delta_index_entry next_entry;
	struct delta_zone *delta_zone;
	struct delta_list *delta_list;

	result = assert_mutable_entry(delta_entry);
	if (result != UDS_SUCCESS)
		return result;

	next_entry = *delta_entry;
	result = next_delta_index_entry(&next_entry);
	if (result != UDS_SUCCESS)
		return result;

	delta_zone = delta_entry->delta_zone;

	if (delta_entry->is_collision) {
		/* This is a collision entry, so just remove it. */
		delete_bits(delta_entry, delta_entry->entry_bits);
		next_entry.offset = delta_entry->offset;
		delta_zone->collision_count -= 1;
	} else if (next_entry.at_end) {
		/* This entry is at the end of the list, so just remove it. */
		delete_bits(delta_entry, delta_entry->entry_bits);
		next_entry.key -= delta_entry->delta;
		next_entry.offset = delta_entry->offset;
	} else {
		/* The delta in the next entry needs to be updated. */
		unsigned int next_value = get_delta_entry_value(&next_entry);
		int old_size = delta_entry->entry_bits + next_entry.entry_bits;

		if (next_entry.is_collision) {
			next_entry.is_collision = false;
			delta_zone->collision_count -= 1;
		}

		set_delta(&next_entry, delta_entry->delta + next_entry.delta);
		next_entry.offset = delta_entry->offset;
		/* The one new entry is always smaller than the two entries being replaced. */
		delete_bits(delta_entry, old_size - next_entry.entry_bits);
		encode_entry(&next_entry, next_value, NULL);
	}

	delta_zone->record_count--;
	delta_zone->discard_count++;
	*delta_entry = next_entry;

	delta_list = delta_entry->delta_list;
	if (delta_entry->offset < delta_list->save_offset) {
		/* The saved entry offset is no longer valid. */
		delta_list->save_key = 0;
		delta_list->save_offset = 0;
	}

	return UDS_SUCCESS;
}

static size_t get_delta_zone_allocated(const struct delta_zone *delta_zone)
{
	return delta_zone->size +
	       (delta_zone->list_count + 2) * sizeof(struct delta_list) +
	       (delta_zone->list_count + 2) * sizeof(u64);
}

void get_delta_index_stats(const struct delta_index *delta_index, struct delta_index_stats *stats)
{
	unsigned int z;
	const struct delta_zone *delta_zone;

	memset(stats, 0, sizeof(struct delta_index_stats));
	stats->memory_allocated = delta_index->zone_count * sizeof(struct delta_zone);
	for (z = 0; z < delta_index->zone_count; z++) {
		delta_zone = &delta_index->delta_zones[z];
		stats->memory_allocated += get_delta_zone_allocated(delta_zone);
		stats->rebalance_time += delta_zone->rebalance_time;
		stats->rebalance_count += delta_zone->rebalance_count;
		stats->record_count += delta_zone->record_count;
		stats->collision_count += delta_zone->collision_count;
		stats->discard_count += delta_zone->discard_count;
		stats->overflow_count += delta_zone->overflow_count;
		stats->list_count += delta_zone->list_count;
	}
}

size_t compute_delta_index_size(unsigned long entry_count,
				unsigned int mean_delta,
				unsigned int payload_bits)
{
	unsigned short min_bits;
	unsigned int incr_keys;
	unsigned int min_keys;

	compute_coding_constants(mean_delta, &min_bits, &min_keys, &incr_keys);
	/* On average, each delta is encoded into about min_bits + 1.5 bits. */
	return entry_count * (payload_bits + min_bits + 1) + entry_count / 2;
}

unsigned int get_delta_index_page_count(unsigned int entry_count,
					unsigned int list_count,
					unsigned int mean_delta,
					unsigned int payload_bits,
					size_t bytes_per_page)
{
	unsigned int bits_per_delta_list;
	unsigned int bits_per_page;
	size_t bits_per_index;

	/* Compute the expected number of bits needed for all the entries. */
	bits_per_index = compute_delta_index_size(entry_count, mean_delta, payload_bits);
	bits_per_delta_list = bits_per_index / list_count;

	/* Add in the immutable delta list headers. */
	bits_per_index += list_count * IMMUTABLE_HEADER_SIZE;
	/* Compute the number of usable bits on an immutable index page. */
	bits_per_page = ((bytes_per_page - sizeof(struct delta_page_header)) * BITS_PER_BYTE);
	/*
	 * Reduce the bits per page by one immutable delta list header and one delta list to
	 * account for internal fragmentation.
	 */
	bits_per_page -= IMMUTABLE_HEADER_SIZE + bits_per_delta_list;
	/* Now compute the number of pages needed. */
	return DIV_ROUND_UP(bits_per_index, bits_per_page);
}

void log_delta_index_entry(struct delta_index_entry *delta_entry)
{
	uds_log_ratelimit(uds_log_info,
			  "List 0x%X Key 0x%X Offset 0x%X%s%s List_size 0x%X%s",
			  delta_entry->list_number,
			  delta_entry->key,
			  delta_entry->offset,
			  delta_entry->at_end ? " end" : "",
			  delta_entry->is_collision ? " collision" : "",
			  delta_entry->delta_list->size,
			  delta_entry->list_overflow ? " overflow" : "");
	delta_entry->list_overflow = false;
}

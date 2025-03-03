/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef INDEX_PAGE_MAP_H
#define INDEX_PAGE_MAP_H 1

#include "geometry.h"
#include "io-factory.h"

/*
 * The index maintains a page map which records how the chapter delta lists are distributed among
 * the index pages for each chapter, allowing the volume to be efficient about reading only pages
 * that it knows it will need.
 */

struct index_page_map {
	const struct geometry *geometry;
	u64 last_update;
	unsigned int entries_per_chapter;
	u16 *entries;
};

int __must_check make_index_page_map(const struct geometry *geometry,
				     struct index_page_map **map_ptr);

void free_index_page_map(struct index_page_map *map);

int __must_check read_index_page_map(struct index_page_map *map, struct buffered_reader *reader);

int __must_check write_index_page_map(struct index_page_map *map, struct buffered_writer *writer);

void update_index_page_map(struct index_page_map *map,
			   u64 virtual_chapter_number,
			   unsigned int chapter_number,
			   unsigned int index_page_number,
			   unsigned int delta_list_number);

unsigned int __must_check find_index_page_number(const struct index_page_map *map,
						 const struct uds_record_name *name,
						 unsigned int chapter_number);

void get_list_number_bounds(const struct index_page_map *map,
			    unsigned int chapter_number,
			    unsigned int index_page_number,
			    unsigned int *lowest_list,
			    unsigned int *highest_list);

u64 compute_index_page_map_save_size(const struct geometry *geometry);

#endif /* INDEX_PAGE_MAP_H */

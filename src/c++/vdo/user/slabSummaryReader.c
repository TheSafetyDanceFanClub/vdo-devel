/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#include "slabSummaryReader.h"

#include <err.h>

#include "memory-alloc.h"

#include "encodings.h"
#include "status-codes.h"
#include "types.h"

#include "physicalLayer.h"
#include "userVDO.h"

/**********************************************************************/
int readSlabSummary(UserVDO *vdo, struct slab_summary_entry **entriesPtr)
{
  zone_count_t zones = vdo->states.slab_depot.zone_count;
  if (zones == 0) {
    return VDO_SUCCESS;
  }

  struct slab_summary_entry *entries;
  block_count_t summary_blocks = VDO_SLAB_SUMMARY_BLOCKS_PER_ZONE;
  int result = vdo->layer->allocateIOBuffer(vdo->layer,
                                            summary_blocks * VDO_BLOCK_SIZE,
                                            "slab summary entries",
                                            (char **) &entries);
  if (result != VDO_SUCCESS) {
    warnx("Could not create in-memory slab summary");
    return result;
  }

  struct partition *slab_summary_partition;
  result = vdo_get_fixed_layout_partition(vdo->states.layout,
					  VDO_SLAB_SUMMARY_PARTITION,
					  &slab_summary_partition);
  if (result != VDO_SUCCESS) {
    warnx("Could not find slab summary partition");
    return result;
  }

  physical_block_number_t origin
    = vdo_get_fixed_layout_partition_offset(slab_summary_partition);
  result = vdo->layer->reader(vdo->layer, origin, summary_blocks,
                              (char *) entries);
  if (result != VDO_SUCCESS) {
    warnx("Could not read summary data");
    UDS_FREE(entries);
    return result;
  }

  // If there is more than one zone, read and combine the other zone's data
  // with the data already read from the first zone.
  if (zones > 1) {
    struct slab_summary_entry *buffer;
    result = vdo->layer->allocateIOBuffer(vdo->layer,
                                          summary_blocks * VDO_BLOCK_SIZE,
                                          "slab summary entries",
                                          (char **) &buffer);
    if (result != VDO_SUCCESS) {
      warnx("Could not create slab summary buffer");
      UDS_FREE(entries);
      return result;
    }

    for (zone_count_t zone = 1; zone < zones; zone++) {
      origin += summary_blocks;
      result = vdo->layer->reader(vdo->layer, origin, summary_blocks,
                                  (char *) buffer);
      if (result != VDO_SUCCESS) {
        warnx("Could not read summary data");
        UDS_FREE(buffer);
        UDS_FREE(entries);
        return result;
      }

      for (slab_count_t entry_number = zone; entry_number < MAX_VDO_SLABS;
           entry_number += zones) {
        memcpy(entries + entry_number, buffer + entry_number,
               sizeof(struct slab_summary_entry));
      }
    }

    UDS_FREE(buffer);
  }

  *entriesPtr = entries;
  return VDO_SUCCESS;
}

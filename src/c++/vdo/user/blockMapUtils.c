/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#include "blockMapUtils.h"

#include <err.h>

#include "errors.h"
#include "memory-alloc.h"
#include "string-utils.h"
#include "syscalls.h"

#include "encodings.h"
#include "status-codes.h"
#include "types.h"

#include "physicalLayer.h"
#include "userVDO.h"

/**
 * Read a block map page call the examiner on every defined mapping in it.
 * Also recursively call itself to examine an entire tree.
 *
 * @param vdo       The VDO
 * @param pagePBN   The PBN of the block map page to read
 * @param height    The height of this page in the tree
 * @param examiner  The MappingExaminer to call for each mapped entry
 *
 * @return VDO_SUCCESS or an error
 **/
static int readAndExaminePage(UserVDO                 *vdo,
                              physical_block_number_t  pagePBN,
                              height_t                 height,
                              MappingExaminer         *examiner)
{
  struct block_map_page *page;
  int result = vdo->layer->allocateIOBuffer(vdo->layer, VDO_BLOCK_SIZE,
                                            "block map page", (char **) &page);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = readBlockMapPage(vdo->layer, pagePBN, vdo->states.vdo.nonce, page);
  if (result != VDO_SUCCESS) {
    UDS_FREE(page);
    return result;
  }

  if (!page->header.initialized) {
    UDS_FREE(page);
    return VDO_SUCCESS;
  }

  struct block_map_slot blockMapSlot = {
    .pbn  = pagePBN,
    .slot = 0,
  };
  for (; blockMapSlot.slot < VDO_BLOCK_MAP_ENTRIES_PER_PAGE;
        blockMapSlot.slot++) {
    struct data_location mapped
      = vdo_unpack_block_map_entry(&page->entries[blockMapSlot.slot]);

    result = examiner(blockMapSlot, height, mapped.pbn, mapped.state);
    if (result != VDO_SUCCESS) {
      UDS_FREE(page);
      return result;
    }

    if (!vdo_is_mapped_location(&mapped)) {
      continue;
    }

    if ((height > 0) && isValidDataBlock(vdo, mapped.pbn)) {
      result = readAndExaminePage(vdo, mapped.pbn, height - 1, examiner);
      if (result != VDO_SUCCESS) {
        UDS_FREE(page);
        return result;
      }
    }
  }

  UDS_FREE(page);
  return VDO_SUCCESS;
}

/**********************************************************************/
int examineBlockMapEntries(UserVDO *vdo, MappingExaminer *examiner)
{
  struct block_map_state_2_0 *map = &vdo->states.block_map;
  int result = ASSERT((map->root_origin != 0),
                      "block map root origin must be non-zero");
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ASSERT((map->root_count != 0),
                  "block map root count must be non-zero");
  if (result != VDO_SUCCESS) {
    return result;
  }

  height_t height = VDO_BLOCK_MAP_TREE_HEIGHT - 1;
  for (uint8_t rootIndex = 0; rootIndex < map->root_count; rootIndex++) {
    result = readAndExaminePage(vdo, rootIndex + map->root_origin, height,
                                examiner);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  return VDO_SUCCESS;
}

/**
 * Find and decode a particular slot from a block map page.
 *
 * @param vdo           The VDO
 * @param pbn           The PBN of the block map page to read
 * @param slot          The slot to read from the block map page
 * @param mappedPBNPtr  A pointer to the mapped PBN
 * @param mappedPtr     A pointer to the mapped state
 *
 * @return VDO_SUCCESS or an error
 **/
static int readSlotFromPage(UserVDO                  *vdo,
                            physical_block_number_t   pbn,
                            slot_number_t             slot,
                            physical_block_number_t  *mappedPBNPtr,
                            enum block_mapping_state *mappedStatePtr)
{
  struct block_map_page *page;
  int result = vdo->layer->allocateIOBuffer(vdo->layer, VDO_BLOCK_SIZE,
                                            "page buffer", (char **) &page);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = readBlockMapPage(vdo->layer, pbn, vdo->states.vdo.nonce, page);
  if (result != VDO_SUCCESS) {
    UDS_FREE(page);
    return result;
  }

  struct data_location mapped;
  if (page->header.initialized) {
    mapped = vdo_unpack_block_map_entry(&page->entries[slot]);
  } else {
    mapped = (struct data_location) {
      .state = VDO_MAPPING_STATE_UNMAPPED,
      .pbn   = VDO_ZERO_BLOCK,
    };
  }

  *mappedStatePtr = mapped.state;
  *mappedPBNPtr   = mapped.pbn;

  UDS_FREE(page);
  return VDO_SUCCESS;
}

/**********************************************************************/
int findLBNPage(UserVDO                 *vdo,
                logical_block_number_t   lbn,
                physical_block_number_t *pbnPtr)
{
  if (lbn >= vdo->states.vdo.config.logical_blocks) {
    warnx("VDO has only %llu logical blocks, cannot dump mapping for LBA %llu",
          (unsigned long long) vdo->states.vdo.config.logical_blocks,
          (unsigned long long) lbn);
    return VDO_OUT_OF_RANGE;
  }

  struct block_map_state_2_0 *map = &vdo->states.block_map;
  page_number_t pageNumber = lbn / VDO_BLOCK_MAP_ENTRIES_PER_PAGE;

  // It's in the tree section of the block map.
  slot_number_t slots[VDO_BLOCK_MAP_TREE_HEIGHT];
  slots[0] = lbn % VDO_BLOCK_MAP_ENTRIES_PER_PAGE;
  root_count_t rootIndex = pageNumber % map->root_count;
  pageNumber = pageNumber / map->root_count;
  for (int i = 1; i < VDO_BLOCK_MAP_TREE_HEIGHT; i++) {
    slots[i] = pageNumber % VDO_BLOCK_MAP_ENTRIES_PER_PAGE;
    pageNumber /= VDO_BLOCK_MAP_ENTRIES_PER_PAGE;
  }

  physical_block_number_t pbn = map->root_origin + rootIndex;
  for (int i = VDO_BLOCK_MAP_TREE_HEIGHT - 1; i > 0; i--) {
    enum block_mapping_state state;
    int result = readSlotFromPage(vdo, pbn, slots[i], &pbn, &state);
    if ((result != VDO_SUCCESS) || (pbn == VDO_ZERO_BLOCK)
        || (state == VDO_MAPPING_STATE_UNMAPPED)) {
      *pbnPtr = VDO_ZERO_BLOCK;
      return result;
    }
  }

  *pbnPtr = pbn;
  return VDO_SUCCESS;
}

/**********************************************************************/
int findLBNMapping(UserVDO                  *vdo,
                   logical_block_number_t    lbn,
                   physical_block_number_t  *pbnPtr,
                   enum block_mapping_state *statePtr)
{
  physical_block_number_t pagePBN;
  int result = findLBNPage(vdo, lbn, &pagePBN);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (pagePBN == VDO_ZERO_BLOCK) {
    *pbnPtr   = VDO_ZERO_BLOCK;
    *statePtr = VDO_MAPPING_STATE_UNMAPPED;
    return VDO_SUCCESS;
  }

  slot_number_t slot = lbn % VDO_BLOCK_MAP_ENTRIES_PER_PAGE;
  return readSlotFromPage(vdo, pagePBN, slot, pbnPtr, statePtr);
}

/**********************************************************************/
int readBlockMapPage(PhysicalLayer            *layer,
                     physical_block_number_t   pbn,
                     nonce_t                   nonce,
                     struct block_map_page    *page)
{
  int result = layer->reader(layer, pbn, 1, (char *) page);
  if (result != VDO_SUCCESS) {
    char errBuf[UDS_MAX_ERROR_MESSAGE_SIZE];
    printf("%llu unreadable : %s",
           (unsigned long long) pbn,
           uds_string_error(result, errBuf, UDS_MAX_ERROR_MESSAGE_SIZE));
    return result;
  }

  enum block_map_page_validity validity
    = vdo_validate_block_map_page(page, nonce, pbn);
  if (validity == VDO_BLOCK_MAP_PAGE_VALID) {
    return VDO_SUCCESS;
  }

  if (validity == VDO_BLOCK_MAP_PAGE_BAD) {
    warnx("Expected page %llu but got page %llu",
          (unsigned long long) pbn,
          (unsigned long long) vdo_get_block_map_page_pbn(page));
  }

  page->header.initialized = false;
  return VDO_SUCCESS;
}

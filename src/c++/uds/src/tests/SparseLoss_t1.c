// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

/**
 * SparseLoss_t1 measures the sparse dedupe loss of an index with an arbitrary
 * number of zones and asserts that it is no worse than the loss expected in
 * the single zone case.
 **/

#include "albtest.h"
#include "assertions.h"
#include "config.h"
#include "hash-utils.h"
#include "index.h"
#include "memory-alloc.h"
#include "request-queue.h"
#include "testPrototypes.h"

enum {
  SPARSE_SAMPLE_RATE = 32,
};

static unsigned int numHashesInChapter;

static struct configuration *config;
static struct uds_index     *theIndex;

static uint64_t blockNameCounter = 0;

static uint64_t postsNotFound = 0;

static struct cond_var callbackCond;
static struct mutex    callbackMutex;
static unsigned int    callbackCount = 0;

/**********************************************************************/
static void incrementCallbackCount(void)
{
  uds_lock_mutex(&callbackMutex);
  callbackCount++;
  uds_signal_cond(&callbackCond);
  uds_unlock_mutex(&callbackMutex);
}

/**********************************************************************/
static void waitForCallbacks(void)
{
  uds_lock_mutex(&callbackMutex);
  while (callbackCount > 0) {
    uds_wait_cond(&callbackCond, &callbackMutex);
  }
  uds_unlock_mutex(&callbackMutex);
}

/**
 * The callback updates the outstanding record count and tracks
 * the number of blocks that weren't found.
 **/
static void testCallback(struct uds_request *request)
{
  UDS_ASSERT_SUCCESS(request->status);
  uds_lock_mutex(&callbackMutex);
  callbackCount--;
  if (!request->found) {
    postsNotFound++;
  }
  uds_signal_cond(&callbackCond);
  uds_unlock_mutex(&callbackMutex);
  freeRequest(request);
}

/**********************************************************************/
static void suiteInit(const char *indexName)
{
  blockNameCounter = 0;
  callbackCount = 0;
  postsNotFound = 0;
  UDS_ASSERT_SUCCESS(uds_init_cond(&callbackCond));
  UDS_ASSERT_SUCCESS(uds_init_mutex(&callbackMutex));

  struct uds_parameters params = {
    .memory_size = 1,
    .name = indexName,
  };
  UDS_ASSERT_SUCCESS(make_configuration(&params, &config));

  // Make a small geometry for speed.
  unsigned int chaptersPerVolume = 10240;
  unsigned int sparseChaptersPerVolume = chaptersPerVolume - 1;
  unsigned int idealNumHashesInChapter = 128;
  unsigned int zoneCount = config->zone_count;
  numHashesInChapter = (idealNumHashesInChapter
                        - idealNumHashesInChapter % zoneCount
                        - zoneCount
                        + 1);
  unsigned int pageSize = 4096;
  unsigned int recordsPerPage = pageSize / BYTES_PER_RECORD;
  unsigned int recordPagesPerChapter
    = idealNumHashesInChapter / recordsPerPage;
  resizeSparseConfiguration(config, pageSize, recordPagesPerChapter,
                            chaptersPerVolume, sparseChaptersPerVolume, 
                            SPARSE_SAMPLE_RATE);

  UDS_ASSERT_SUCCESS(make_index(config, UDS_CREATE, NULL, &testCallback,
                                &theIndex));
}

/**********************************************************************/
static void suiteCleaner(void)
{
  free_index(theIndex);
  theIndex = NULL;
  free_configuration(config);
  UDS_ASSERT_SUCCESS(uds_destroy_cond(&callbackCond));
  UDS_ASSERT_SUCCESS(uds_destroy_mutex(&callbackMutex));
}

/**********************************************************************/
static void dispatchRequest(struct uds_request *request)
{
  request->index = theIndex;
  request->unbatched = true;
  incrementCallbackCount();
  enqueue_request(request, STAGE_TRIAGE);
}

/**********************************************************************/
static void nextBlockNameInZone(const struct uds_index *index,
                                unsigned int            zone,
                                struct uds_record_name *name)
{
  unsigned int nameZone = MAX_ZONES;
  while (nameZone != zone) {
    *name = hash_record_name(&blockNameCounter, sizeof(blockNameCounter));
    nameZone = get_volume_index_zone(index->volume_index, name);
    blockNameCounter++;
  }
}

/**********************************************************************/
static void indexOneChapter(void)
{
  unsigned int numAdded;
  for (numAdded = 0; numAdded < numHashesInChapter; ++numAdded) {
    unsigned int zone = numAdded % theIndex->zone_count;
    struct uds_request *request;
    UDS_ALLOCATE(1, struct uds_request, "req", &request);
    request->type = UDS_POST;
    nextBlockNameInZone(theIndex, zone, &request->record_name);
    dispatchRequest(request);
  }
  waitForCallbacks();
}

/**********************************************************************/
static void skipOneChapter(void)
{
  unsigned int numAdded;
  for (numAdded = 0; numAdded < numHashesInChapter; ++numAdded) {
    unsigned int zone = numAdded % theIndex->zone_count;
    struct uds_record_name name;
    nextBlockNameInZone(theIndex, zone, &name);
  }
}

/**********************************************************************/
static void sparseLossTest(void)
{
  unsigned int chaptersIndexed = 128 * theIndex->zone_count;
  unsigned int i;
  for (i =  0; i < chaptersIndexed; ++i) {
    indexOneChapter();
  }

  /*
   * Reset the block counter and reindex the above blocks.
   * The index is all sparse, save for the open chapter, so we expect
   * some loss of dedupe. After enough runs, we expect that we only lose
   * about 31 blocks per chapter indexed.
   */
  blockNameCounter = 0;
  postsNotFound = 0;
  invalidate_sparse_cache(theIndex->volume->sparse_cache);

  // Only re-index every n-th chapter or any sparse loss from
  // a multiple subindex multicore scaling will be concealed.
  unsigned int stride = theIndex->zone_count;
  unsigned int chaptersDeduped = chaptersIndexed / stride;
  for (i = 0; i < chaptersIndexed; ++i) {
    if (i % stride == 0) {
      indexOneChapter();
    } else {
      skipOneChapter();
    }
  }

  struct uds_index_stats counters;
  get_index_stats(theIndex, &counters);
  albPrint("Sparse loss indexing %u chapters of dedupe in a %u-zone config: %llu (%llu discards)",
           chaptersDeduped, theIndex->zone_count,
           (unsigned long long) postsNotFound,
           (unsigned long long) counters.entries_discarded);
  unsigned long expectedLoss = chaptersDeduped * (SPARSE_SAMPLE_RATE - 1);
  CU_ASSERT(postsNotFound < expectedLoss * 5 / 4);
  CU_ASSERT(postsNotFound > expectedLoss * 3 / 4);
}

static const CU_TestInfo sparseTests[] = {
  { "Sparse Loss",      sparseLossTest      },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "SparseLoss_t1",
  .initializerWithIndexName = suiteInit,
  .cleaner                  = suiteCleaner,
  .tests                    = sparseTests,
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}

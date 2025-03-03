// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "config.h"
#include "hash-utils.h"
#include "index-session.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "random.h"
#include "string-utils.h"
#include "testPrototypes.h"
#include "volume.h"
#include "volumeUtils.h"

typedef struct readRequest {
  struct uds_request request;
  uint32_t           physPage;
} ReadRequest;

static struct configuration      *config;
static struct geometry           *geometry;
static struct index_layout       *layout;
static u8                       **pages;
static struct volume             *volume;
static struct uds_index_session  *session;
static unsigned int               numRequestsQueued = 0;
static struct mutex               numRequestsMutex;
static struct cond_var            allDoneCond;
static bool                       keepRunning       = false;

/**********************************************************************/
static void freeReadRequest(struct uds_request *request)
{
  // Release the counted reference to the context that was acquired for the
  // request (and not released) in createRequest().
  ReadRequest *readRequest = container_of(request, ReadRequest, request);
  UDS_FREE(readRequest);
}

/**********************************************************************/
static void verifyPageData(uint32_t            physPage,
                           struct cached_page *cp,
                           size_t              length)
{
  UDS_ASSERT_EQUAL_BYTES(pages[physPage], dm_bufio_get_block_data(cp->buffer),
                         length);
}

/**********************************************************************/
static void retryReadRequest(struct uds_request *request)
{
  freeReadRequest(request);
  uds_lock_mutex(&numRequestsMutex);
  if (--numRequestsQueued == 0) {
    uds_broadcast_cond(&allDoneCond);
  }
  uds_unlock_mutex(&numRequestsMutex);
}

/**********************************************************************/
static void retryReadRequestAndVerify(struct uds_request *request)
{
  ReadRequest *readRequest = container_of(request, ReadRequest, request);
  uint32_t physPage = readRequest->physPage;

  struct cached_page *actual;
  // Make sure the page read is synchronous. We do not need to grab
  // the volume read lock here, because the caller of this function already
  // has it.
  UDS_ASSERT_SUCCESS(get_volume_page_locked(volume, physPage + 1, &actual));
  verifyPageData(physPage, actual, geometry->bytes_per_page);
  retryReadRequest(request);
}

/**********************************************************************/
static void init(request_restarter_t restartRequest, unsigned int zoneCount)
{
  set_request_restarter(restartRequest);

  UDS_ASSERT_SUCCESS(uds_init_mutex(&numRequestsMutex));
  UDS_ASSERT_SUCCESS(uds_init_cond(&allDoneCond));
  UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
  numRequestsQueued = 0;

  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = getTestIndexName(),
    .zone_count = zoneCount,
  };
  UDS_ASSERT_SUCCESS(make_configuration(&params, &config));
  resizeDenseConfiguration(config, 4096, 16, 256);
  UDS_ASSERT_SUCCESS(make_uds_index_layout(config, true, &layout));

  UDS_ASSERT_SUCCESS(make_volume(config, layout, &volume));

  geometry = config->geometry;
  pages = makePageArray(geometry->pages_per_volume, geometry->bytes_per_page);
  writeTestVolumeData(volume, geometry, pages);
}

/**********************************************************************/
static void deinit(void)
{
  set_request_restarter(NULL);
  freePageArray(pages, geometry->pages_per_volume);
  free_volume(volume);
  free_configuration(config);
  free_uds_index_layout(UDS_FORGET(layout));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(session));
  uds_destroy_cond(&allDoneCond);
  uds_destroy_mutex(&numRequestsMutex);
}

/**********************************************************************/
static void computeNameOnPage(struct uds_record_name *name, uint32_t physPage)
{
  struct delta_index_page indexPage;
  uint32_t pageInChapter = physPage % geometry->pages_per_chapter;
  if (pageInChapter >= geometry->index_pages_per_chapter) {
    /* This is a record page so it doesn't matter what record name we use. */
    return;
  }

  UDS_ASSERT_SUCCESS(initialize_chapter_index_page(&indexPage,
                                                   geometry,
                                                   pages[physPage],
                                                   volume->nonce));
  unsigned int listNumber = hash_to_chapter_delta_list(name, geometry);
  while ((listNumber < indexPage.lowest_list_number) ||
         (listNumber > indexPage.highest_list_number)) {
    createRandomBlockName(name);
    listNumber = hash_to_chapter_delta_list(name, geometry);
  }
}

/**********************************************************************/
static struct uds_request *newReadRequest(uint32_t physPage)
{
  ReadRequest *readRequest = NULL;

  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, ReadRequest, __func__, &readRequest));
  readRequest->request.session = session;
  readRequest->physPage = physPage;
  readRequest->request.unbatched = true;
  computeNameOnPage(&readRequest->request.record_name, physPage);
  return &readRequest->request;
}

/**********************************************************************/
static bool waitCondTimeout(struct cond_var *cond, struct mutex *mutex,
                            ktime_t timeout)
{
  int result = uds_timed_wait_cond(cond, mutex, timeout);
  return (result != ETIMEDOUT);
}

/**********************************************************************/
static void testSequentialGet(void)
{
  init(retryReadRequestAndVerify, 1);
  unsigned int chapter, page;
  for (chapter = 0; chapter < geometry->chapters_per_volume; ++chapter) {
    for (page = 0; page < geometry->pages_per_chapter; ++page) {
      uint32_t physPage = chapter * geometry->pages_per_chapter + page;
      struct uds_request *request = newReadRequest(physPage);
      struct cached_page *actual;
      begin_pending_search(volume->page_cache, physPage + 1, 0);
      // Make sure the page read is asynchronous
      int ret = get_volume_page_protected(volume, request, physPage + 1,
                                          &actual);
      if (ret == UDS_SUCCESS) {
        freeReadRequest(request);
        verifyPageData(physPage, actual, geometry->bytes_per_page);
      } else if (ret == UDS_QUEUED) {
        uds_lock_mutex(&numRequestsMutex);
        ++numRequestsQueued;
        uds_unlock_mutex(&numRequestsMutex);
      }
      end_pending_search(volume->page_cache, 0);
    }
  }
  uds_lock_mutex(&numRequestsMutex);
  while (numRequestsQueued > 0) {
    CU_ASSERT_TRUE(waitCondTimeout(&allDoneCond, &numRequestsMutex,
                                   seconds_to_ktime(10)));
  }
  uds_unlock_mutex(&numRequestsMutex);
}

/**********************************************************************/
static void testStumblingGet(void)
{
  init(retryReadRequestAndVerify, 1);
  unsigned int page;
  for (page = 0; page < geometry->pages_per_volume;) {
    struct uds_request *request = newReadRequest(page);
    struct cached_page *actual;
    // Make sure the page read is asynchronous
    begin_pending_search(volume->page_cache, page + 1, 0);
    int ret = get_volume_page_protected(volume, request, page + 1, &actual);
    if (ret == UDS_SUCCESS) {
      freeReadRequest(request);
      verifyPageData(page, actual, geometry->bytes_per_page);
    } else if (ret == UDS_QUEUED) {
      uds_lock_mutex(&numRequestsMutex);
      ++numRequestsQueued;
      uds_unlock_mutex(&numRequestsMutex);
    }
    end_pending_search(volume->page_cache, 0);
    // back one page 25%, same page 25%, forward one page 50%.
    unsigned int action = random() % 4;
    if (action == 0) {
      if (page > 0) {
        --page;
      }
    } else if (action != 1) {
      ++page;
    }
  }
  uds_lock_mutex(&numRequestsMutex);
  while (numRequestsQueued > 0) {
    CU_ASSERT_TRUE(waitCondTimeout(&allDoneCond, &numRequestsMutex,
                                   seconds_to_ktime(10)));
  }
  uds_unlock_mutex(&numRequestsMutex);
}

/**********************************************************************/
static void testFullReadQueue(void)
{
  init(retryReadRequest, 1);

  const unsigned int numRequests = VOLUME_CACHE_MAX_QUEUED_READS;
  struct uds_request **requests;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(numRequests, struct uds_request *, __func__,
                                  &requests));

  volume->reader_state |= READER_STATE_STOP;
  unsigned int i;
  for (i = 0; i < numRequests; i++) {
    requests[i] = newReadRequest(i);
    int result = enqueue_read(volume->page_cache, requests[i], i + 1);
    bool queued = (result == UDS_QUEUED);
    if (i < numRequests - 1) {
      CU_ASSERT_TRUE(queued);

      uds_lock_mutex(&numRequestsMutex);
      ++numRequestsQueued;
      uds_unlock_mutex(&numRequestsMutex);
    } else {
      CU_ASSERT_FALSE(queued);
    }
  }

  volume->reader_state &= ~READER_STATE_STOP;
  uds_lock_mutex(&volume->read_threads_mutex);
  int result = enqueue_page_read(volume, requests[numRequests - 1],
                                 numRequests - 1);
  uds_unlock_mutex(&volume->read_threads_mutex);
  UDS_ASSERT_ERROR(UDS_QUEUED, result);
  uds_lock_mutex(&numRequestsMutex);
  ++numRequestsQueued;
  uds_unlock_mutex(&numRequestsMutex);

  uds_lock_mutex(&numRequestsMutex);
  while (numRequestsQueued > 0) {
    CU_ASSERT_TRUE(waitCondTimeout(&allDoneCond, &numRequestsMutex,
                                   seconds_to_ktime(60)));
  }
  uds_unlock_mutex(&numRequestsMutex);

  UDS_FREE(requests);
}

/**********************************************************************/
static void testInvalidateReadQueue(void)
{
  init(retryReadRequest, 1);

  const unsigned int numRequests = VOLUME_CACHE_MAX_QUEUED_READS;
  struct uds_request **requests;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(numRequests, struct uds_request *, __func__,
                                  &requests));

  // Fill up the read queue by turning on the stop state and enqueuing entries
  volume->reader_state |= READER_STATE_STOP;
  unsigned int i;
  for (i = 0; i < numRequests; i++) {
    requests[i] = newReadRequest(i);
    int result = enqueue_read(volume->page_cache, requests[i], i + 1);
    bool queued = (result == UDS_QUEUED);
    if (i < numRequests - 1) {
      CU_ASSERT_TRUE(queued);

      uds_lock_mutex(&numRequestsMutex);
      ++numRequestsQueued;
      uds_unlock_mutex(&numRequestsMutex);
    } else {
      CU_ASSERT_FALSE(queued);
    }
  }

  // Invalidate all of the reads, so that when they're dequeued, they don't
  // push the synchronized read out of the cache
  for (i = 0; i < geometry->chapters_per_volume; i++) {
    invalidate_page_cache_for_chapter(volume->page_cache, i,
                                      geometry->pages_per_chapter);
  }
  struct cached_page *actual;

  // Synchronously read in physical page 5. We skip entry 0, as that is the
  // configuration page for the volume.
  uds_lock_mutex(&volume->read_threads_mutex);
  UDS_ASSERT_SUCCESS(get_volume_page_locked(volume, 5, &actual));
  CU_ASSERT_PTR_NOT_NULL(actual);
  uds_unlock_mutex(&volume->read_threads_mutex);

  volume->reader_state &= ~READER_STATE_STOP;
  uds_lock_mutex(&volume->read_threads_mutex);
  // Enqueue another request to kick the reader threads awake to start
  // dequeuing the now invalid reads
  int result = enqueue_page_read(volume, requests[numRequests - 1],
                                 numRequests - 1);
  uds_unlock_mutex(&volume->read_threads_mutex);
  UDS_ASSERT_ERROR(UDS_QUEUED, result);
  uds_lock_mutex(&numRequestsMutex);
  ++numRequestsQueued;
  uds_unlock_mutex(&numRequestsMutex);

  uds_lock_mutex(&numRequestsMutex);
  while (numRequestsQueued > 0) {
    CU_ASSERT_TRUE(waitCondTimeout(&allDoneCond, &numRequestsMutex,
                                   seconds_to_ktime(60)));
  }
  uds_unlock_mutex(&numRequestsMutex);

  // Try to get page 5 from the map. It should be there from the sync read
  uds_lock_mutex(&volume->read_threads_mutex);
  get_page_from_cache(volume->page_cache, 5, &actual);
  CU_ASSERT_PTR_NOT_NULL(actual);
  uds_unlock_mutex(&volume->read_threads_mutex);

  UDS_FREE(requests);
}

/**********************************************************************/
static unsigned int randomChapter(void)
{
  return random() % geometry->chapters_per_volume;
}

/**********************************************************************/
static unsigned int randomPage(void)
{
  return random() % geometry->pages_per_chapter;
}

/**********************************************************************/
static void retryReadRequestAndVerifyMT(struct uds_request *request)
{
  ReadRequest *readRequest = container_of(request, ReadRequest, request);
  uint32_t physPage = readRequest->physPage;

  struct cached_page *actual;
  // Make sure the page read is synchronous. We do not need to grab
  // the volume read lock here, because the caller of this function already
  // has it.
  UDS_ASSERT_SUCCESS(get_volume_page_locked(volume, physPage + 1, &actual));
  verifyPageData(physPage, actual, geometry->bytes_per_page);

  if (request->requeued) {
    keepRunning = false;
  }

  retryReadRequest(request);
}

static const unsigned int MAX_REQUESTS  = 102400;
static unsigned int       iterationCounter = 0;

typedef struct {
  unsigned int zoneNumber;
} ThreadArg;

/**********************************************************************/
static void invalidatePageThread(void *arg __attribute__((unused)))
{
  while (keepRunning) {
    uds_lock_mutex(&volume->read_threads_mutex);
    unsigned int chapter = randomChapter();
    unsigned int pageNumber = randomPage();
    unsigned int physicalPage
      = chapter * geometry->pages_per_chapter + pageNumber;

    invalidate_page(volume->page_cache, physicalPage);
    uds_unlock_mutex(&volume->read_threads_mutex);
    cond_resched();

  }
}

/**********************************************************************/
static void indexThreadAsync(void *arg)
{
  ThreadArg *a = (ThreadArg *) arg;
  unsigned int zoneNumber = a->zoneNumber;

  while (iterationCounter < MAX_REQUESTS) {
    uds_signal_cond(&volume->read_threads_cond);

    unsigned int chapter = randomChapter();
    unsigned int pageNumber = randomPage();
    unsigned int physicalPage
      = chapter * geometry->pages_per_chapter + pageNumber;

    // Only one of the async threads needs to keep track of the number of
    // iterations it has run.
    if (zoneNumber == 0) {
      iterationCounter++;
    }

    struct cached_page *entry = NULL;
    struct uds_request *request = newReadRequest(physicalPage);
    request->zone_number = zoneNumber;

    begin_pending_search(volume->page_cache, physicalPage + 1, zoneNumber);

    // Assume we're enqueuing this
    uds_lock_mutex(&numRequestsMutex);
    ++numRequestsQueued;
    uds_unlock_mutex(&numRequestsMutex);

    int result = get_volume_page_protected(volume, request, physicalPage + 1,
                                           &entry);

    if (result == UDS_SUCCESS) {
      freeReadRequest(request);
      verifyPageData(physicalPage, entry, geometry->bytes_per_page);

      // We didn't actually enqueue this particular request, so adjust the count
      // we're waiting on
      uds_lock_mutex(&numRequestsMutex);
      --numRequestsQueued;
      uds_unlock_mutex(&numRequestsMutex);
    } else {
      CU_ASSERT_EQUAL(result, UDS_QUEUED);
    }

    end_pending_search(volume->page_cache, zoneNumber);
    cond_resched();
  }

  uds_signal_cond(&volume->read_threads_cond);

  keepRunning = false;
}

/**********************************************************************/
static void testMultiThreadStress(unsigned int numAsyncIndexThreads)
{
  /*
   * Use three types of threads to try and mess things up as much as possible.
   * - Index threads doing async searches
   * - Regular reader threads reading in entries from disk
   * - A thread which is periodically invalidating chapters
   */

  const unsigned int numZones = numAsyncIndexThreads;
  const unsigned int numThreads = 1 + numZones;

  init(retryReadRequestAndVerifyMT, numZones);
  const unsigned int numRequests = VOLUME_CACHE_MAX_QUEUED_READS;
  keepRunning = true;

  // Fill up the read queue by turning on the stop state and enqueuing entries
  volume->reader_state |= READER_STATE_STOP;
  unsigned int i;
  for (i = 0; i < numRequests; i++) {
    struct uds_request *request = newReadRequest(i);
    int result = enqueue_read(volume->page_cache, request, i + 1);
    bool queued = (result == UDS_QUEUED);
    if (i < numRequests - 1) {
      CU_ASSERT_TRUE(queued);

      uds_lock_mutex(&numRequestsMutex);
      ++numRequestsQueued;
      uds_unlock_mutex(&numRequestsMutex);
    } else {
      CU_ASSERT_FALSE(queued);
      freeReadRequest(request);
    }
  }
  volume->reader_state &= ~READER_STATE_STOP;

  ThreadArg *args;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(numZones, ThreadArg, __func__, &args));
  unsigned int k;
  for (k = 0; k < numZones; k++) {
    args[k].zoneNumber = k;
  }

  struct thread **threads;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(numThreads, struct thread *, __func__,
                              &threads));

  int result = UDS_SUCCESS;
  for (i = 0; i < numAsyncIndexThreads; i++) {
    char nameBuf[100];
    UDS_ASSERT_SUCCESS(uds_fixed_sprintf(nameBuf, sizeof(nameBuf),
                                         "asyncIndex%d", i));
    result = uds_create_thread(indexThreadAsync, &args[i], nameBuf,
                               &threads[i]);
    UDS_ASSERT_SUCCESS(result);
  }

  result = uds_create_thread(invalidatePageThread, NULL, "invalidPage",
                             &threads[i++]);
  UDS_ASSERT_SUCCESS(result);

  CU_ASSERT_EQUAL(i, numThreads);

  for (i = 0; i < numThreads; ++i) {
    uds_join_threads(threads[i]);
  }

  uds_lock_mutex(&numRequestsMutex);
  while (numRequestsQueued > 0) {
    CU_ASSERT_TRUE(waitCondTimeout(&allDoneCond, &numRequestsMutex,
                                   seconds_to_ktime(60)));
  }
  uds_unlock_mutex(&numRequestsMutex);

  UDS_FREE(threads);
  UDS_FREE(args);
}

/**********************************************************************/
static void testMultiThreadStress1Async(void)
{
  testMultiThreadStress(1);
}

/**********************************************************************/
static void testMultiThreadStress4Async(void)
{
  testMultiThreadStress(4);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"Invalid Read Queue", testInvalidateReadQueue},
  {"SequentialGet",      testSequentialGet},
  {"StumblingGet",       testStumblingGet},
  {"Full Read Queue",    testFullReadQueue},
  {"MT Stress 1 async",  testMultiThreadStress1Async},
  {"MT Stress 4 async",  testMultiThreadStress4Async},
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name    = "Volume_n4",
  .cleaner = deinit,
  .tests   = tests,
};

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}

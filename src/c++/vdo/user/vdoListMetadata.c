/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#include <err.h>
#include <getopt.h>
#include <stdlib.h>

#include "errors.h"
#include "string-utils.h"
#include "syscalls.h"

#include "encodings.h"
#include "status-codes.h"
#include "types.h"
#include "vdo-layout.h"

#include "userVDO.h"
#include "vdoVolumeUtils.h"

static const char usageString[] = "[--help] [--version] <vdoBackingDevice>";

static const char helpString[] =
  "vdoListMetadata - list the metadata regions on a VDO device\n"
  "\n"
  "SYNOPSIS\n"
  "  vdoListMetadata <vdoBackingDevice>\n"
  "\n"
  "DESCRIPTION\n"
  "  vdoListMetadata lists the metadata regions of a VDO device\n"
  "  as ranges of block numbers. Each range is on a separate line\n"
  "  of the form:\n"
  "    startBlock .. endBlock: label\n"
  "  Both endpoints are included in the range, and are the zero-based\n"
  "  indexes of 4KB VDO metadata blocks on the backing device.\n"
  "\n";

static struct option options[] = {
  { "help",    no_argument, NULL, 'h' },
  { "version", no_argument, NULL, 'V' },
  { NULL,      0,           NULL,  0  },
};

static char    *vdoBackingName = NULL;
static UserVDO *vdo            = NULL;

/**
 * Explain how this command-line tool is used.
 *
 * @param programName  Name of this program
 * @param usageString  Multi-line explanation
 **/
static void usage(const char *programName)
{
  errx(1, "Usage: %s %s\n", programName, usageString);
}

/**
 * Parse the arguments passed; print command usage if arguments are wrong.
 *
 * @param argc  Number of input arguments
 * @param argv  Array of input arguments
 **/
static void processArgs(int argc, char *argv[])
{
  int c;
  while ((c = getopt_long(argc, argv, "hV", options, NULL)) != -1) {
    switch (c) {
    case 'h':
      printf("%s", helpString);
      exit(0);

    case 'V':
      printf("%s version is: %s\n", argv[0], CURRENT_VERSION);
      exit(0);

    default:
      usage(argv[0]);
      break;
    }
  }

  // Explain usage and exit
  if (optind != (argc - 1)) {
    usage(argv[0]);
  }

  vdoBackingName = argv[optind++];
}

/**
 * List a range of metadata blocks on stdout.
 *
 * @param label       The type of metadata
 * @param startBlock  The block to start at in the VDO backing device
 * @param count       The number of metadata blocks in the range
 **/
static void listBlocks(const char              *label,
                       physical_block_number_t  startBlock,
                       block_count_t            count)
{
  printf("%ld .. %ld: %s\n", startBlock, startBlock + count - 1, label);
}

/**********************************************************************/
static void listGeometryBlock(void)
{
  // The geometry block is a single block at the start of the volume.
  listBlocks("geometry block", 0, 1);
}

/**********************************************************************/
static void listIndex(void)
{
  // The index is all blocks from the geometry block to the super block,
  // exclusive.
  listBlocks("index", 1, vdo_get_data_region_start(vdo->geometry) - 1);
}

/**********************************************************************/
static void listSuperBlock(void)
{
  // The SuperBlock is a single block at the start of the data region.
  listBlocks("super block", vdo_get_data_region_start(vdo->geometry), 1);
}

/**********************************************************************/
static void listBlockMap(void)
{
  struct block_map_state_2_0 map = vdo->states.block_map;
  if (map.root_count > 0) {
    listBlocks("block map tree roots", map.root_origin, map.root_count);
  }
}

/**********************************************************************/
static void listSlabs(void)
{
  struct slab_depot_state_2_0 depot = vdo->states.slab_depot;
  physical_block_number_t slabOrigin = depot.first_block;
  for (slab_count_t slab = 0; slab < vdo->slabCount; slab++) {
    // List the slab's reference count blocks.
    char buffer[64];
    sprintf(buffer, "slab %u reference blocks", slab);
    listBlocks(buffer, slabOrigin + depot.slab_config.data_blocks,
               depot.slab_config.reference_count_blocks);

    // List the slab's journal blocks.
    sprintf(buffer, "slab %u journal", slab);
    listBlocks(buffer, vdo_get_slab_journal_start_block(&depot.slab_config,
                                                        slabOrigin),
               depot.slab_config.slab_journal_blocks);

    slabOrigin += vdo->states.vdo.config.slab_size;
  }
}

/**********************************************************************/
static void listRecoveryJournal(void)
{
  const struct partition *partition
    = getPartition(vdo, VDO_RECOVERY_JOURNAL_PARTITION,
                   "no recovery journal partition");
  listBlocks("recovery journal",
             vdo_get_fixed_layout_partition_offset(partition),
             vdo->states.vdo.config.recovery_journal_size);
}

/**********************************************************************/
static void listSlabSummary(void)
{
  const struct partition *partition
    = getPartition(vdo, VDO_SLAB_SUMMARY_PARTITION,
                   "no slab summary partition");
  listBlocks("slab summary", vdo_get_fixed_layout_partition_offset(partition),
             VDO_SLAB_SUMMARY_BLOCKS);
}

/**********************************************************************/
int main(int argc, char *argv[])
{
  static char errBuf[UDS_MAX_ERROR_MESSAGE_SIZE];

  int result = vdo_register_status_codes();
  if (result != VDO_SUCCESS) {
    errx(1, "Could not register status codes: %s",
         uds_string_error(result, errBuf, UDS_MAX_ERROR_MESSAGE_SIZE));
  }

  processArgs(argc, argv);

  // Read input VDO, without validating its config.
  result = readVDOWithoutValidation(vdoBackingName, &vdo);
  if (result != VDO_SUCCESS) {
    errx(1, "Could not load VDO from '%s'", vdoBackingName);
  }

  listGeometryBlock();
  listIndex();
  listSuperBlock();
  listBlockMap();
  listSlabs();
  listRecoveryJournal();
  listSlabSummary();

  freeVDOFromFile(&vdo);
  exit(0);
}

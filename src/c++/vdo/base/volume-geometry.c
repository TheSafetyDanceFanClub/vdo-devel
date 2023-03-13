// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "volume-geometry.h"

#include "buffer.h"
#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "permassert.h"

#include "constants.h"
#include "encodings.h"
#ifndef __KERNEL__
#include "physicalLayer.h"
#endif /* not __KERNEL__ */
#include "release-versions.h"
#include "status-codes.h"
#include "types.h"

enum {
	MAGIC_NUMBER_SIZE = 8,
	DEFAULT_GEOMETRY_BLOCK_VERSION = 5,
};

struct geometry_block {
	char magic_number[MAGIC_NUMBER_SIZE];
	struct header header;
	u32 checksum;
} __packed;

static const struct header GEOMETRY_BLOCK_HEADER_5_0 = {
	.id = VDO_GEOMETRY_BLOCK,
	.version = {
		.major_version = 5,
		.minor_version = 0,
	},
	/*
	 * Note: this size isn't just the payload size following the header, like it is everywhere
	 * else in VDO.
	 */
	.size = sizeof(struct geometry_block) + sizeof(struct volume_geometry),
};

static const struct header GEOMETRY_BLOCK_HEADER_4_0 = {
	.id = VDO_GEOMETRY_BLOCK,
	.version = {
		.major_version = 4,
		.minor_version = 0,
	},
	/*
	 * Note: this size isn't just the payload size following the header, like it is everywhere
	 * else in VDO.
	 */
	.size = sizeof(struct geometry_block) + sizeof(struct volume_geometry_4_0),
};

static const u8 MAGIC_NUMBER[MAGIC_NUMBER_SIZE + 1] = "dmvdo001";

static const release_version_number_t COMPATIBLE_RELEASE_VERSIONS[] = {
	VDO_MAGNESIUM_RELEASE_VERSION_NUMBER,
	VDO_ALUMINUM_RELEASE_VERSION_NUMBER,
};

/**
 * is_loadable_release_version() - Determine whether the supplied release version can be understood
 *                                 by the VDO code.
 * @version: The release version number to check.
 *
 * Return: True if the given version can be loaded.
 */
static inline bool is_loadable_release_version(release_version_number_t version)
{
	unsigned int i;

	if (version == VDO_CURRENT_RELEASE_VERSION_NUMBER)
		return true;

	for (i = 0; i < ARRAY_SIZE(COMPATIBLE_RELEASE_VERSIONS); i++)
		if (version == COMPATIBLE_RELEASE_VERSIONS[i])
			return true;

	return false;
}

/**
 * decode_index_config() - Decode the on-disk representation of an index configuration from a
 *                         buffer.
 * @buffer: A buffer positioned at the start of the encoding.
 * @config: The structure to receive the decoded fields.
 *
 * Return: UDS_SUCCESS or an error.
 */
static int decode_index_config(struct buffer *buffer, struct index_config *config)
{
	u32 mem;
	bool sparse;
	int result;

	result = get_u32_le_from_buffer(buffer, &mem);
	if (result != VDO_SUCCESS)
		return result;

	result = skip_forward(buffer, sizeof(u32));
	if (result != VDO_SUCCESS)
		return result;

	result = get_boolean(buffer, &sparse);
	if (result != VDO_SUCCESS)
		return result;

	*config = (struct index_config) {
		.mem = mem,
		.sparse = sparse,
	};
	return VDO_SUCCESS;
}

#if (defined(VDO_USER) || defined(INTERNAL))
/**
 * encode_index_config() - Encode the on-disk representation of an index configuration into a
 *                         buffer.
 * @config: The index configuration to encode.
 * @buffer: A buffer positioned at the start of the encoding.
 *
 * Return: UDS_SUCCESS or an error.
 */
static int encode_index_config(const struct index_config *config, struct buffer *buffer)
{
	int result;

	result = put_u32_le_into_buffer(buffer, config->mem);
	if (result != VDO_SUCCESS)
		return result;

	result = zero_bytes(buffer, sizeof(u32));
	if (result != VDO_SUCCESS)
		return result;

	return put_boolean(buffer, config->sparse);
}
#endif /* VDO_USER */

/**
 * decode_volume_region() - Decode the on-disk representation of a volume region from a buffer.
 * @buffer: A buffer positioned at the start of the encoding.
 * @region: The structure to receive the decoded fields.
 *
 * Return: UDS_SUCCESS or an error.
 */
static int decode_volume_region(struct buffer *buffer, struct volume_region *region)
{
	physical_block_number_t start_block;
	enum volume_region_id id;
	int result;

	result = get_u32_le_from_buffer(buffer, &id);
	if (result != VDO_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &start_block);
	if (result != VDO_SUCCESS)
		return result;

	*region = (struct volume_region) {
		.id = id,
		.start_block = start_block,
	};
	return VDO_SUCCESS;
}

#if (defined(VDO_USER) || defined(INTERNAL))
/**
 * encode_volume_region() - Encode the on-disk representation of a volume region into a buffer.
 * @region: The region to encode.
 * @buffer: A buffer positioned at the start of the encoding.
 *
 * Return: UDS_SUCCESS or an error.
 */
static int encode_volume_region(const struct volume_region *region, struct buffer *buffer)
{
	int result;

	result = put_u32_le_into_buffer(buffer, region->id);
	if (result != VDO_SUCCESS)
		return result;

	return put_u64_le_into_buffer(buffer, region->start_block);
}
#endif /* VDO_USER */

/**
 * decode_volume_geometry() - Decode the on-disk representation of a volume geometry from a buffer.
 * @buffer: A buffer positioned at the start of the encoding.
 * @geometry: The structure to receive the decoded fields.
 * @version: The geometry block version to decode.
 *
 * Return: UDS_SUCCESS or an error.
 */
static int
decode_volume_geometry(struct buffer *buffer, struct volume_geometry *geometry, u32 version)
{
	release_version_number_t release_version;
	enum volume_region_id id;
	nonce_t nonce;
	block_count_t bio_offset;
	int result;

	result = get_u32_le_from_buffer(buffer, &release_version);
	if (result != VDO_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &nonce);
	if (result != VDO_SUCCESS)
		return result;

	geometry->release_version = release_version;
	geometry->nonce = nonce;

	result = get_bytes_from_buffer(buffer, sizeof(uuid_t), (unsigned char *) &geometry->uuid);
	if (result != VDO_SUCCESS)
		return result;

	bio_offset = 0;
	if (version > 4) {
		result = get_u64_le_from_buffer(buffer, &bio_offset);
		if (result != VDO_SUCCESS)
			return result;
	}
	geometry->bio_offset = bio_offset;

	for (id = 0; id < VDO_VOLUME_REGION_COUNT; id++) {
		result = decode_volume_region(buffer, &geometry->regions[id]);
		if (result != VDO_SUCCESS)
			return result;
	}

	return decode_index_config(buffer, &geometry->index_config);
}

#if (defined(VDO_USER) || defined(INTERNAL))
/**
 * encode_volume_geometry() - Encode the on-disk representation of a volume geometry into a buffer.
 * @geometry: The geometry to encode.
 * @buffer: A buffer positioned at the start of the encoding.
 * @version: The geometry block version to encode.
 *
 * Return: UDS_SUCCESS or an error.
 */
static int
encode_volume_geometry(const struct volume_geometry *geometry, struct buffer *buffer, u32 version)
{
	enum volume_region_id id;
	int result;

	result = put_u32_le_into_buffer(buffer, geometry->release_version);
	if (result != VDO_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, geometry->nonce);
	if (result != VDO_SUCCESS)
		return result;

	result = put_bytes(buffer, sizeof(uuid_t), (unsigned char *) &geometry->uuid);
	if (result != VDO_SUCCESS)
		return result;

	if (version >= 5) {
		result = put_u64_le_into_buffer(buffer, geometry->bio_offset);
		if (result != VDO_SUCCESS)
			return result;
	}

	for (id = 0; id < VDO_VOLUME_REGION_COUNT; id++) {
		result = encode_volume_region(&geometry->regions[id], buffer);
		if (result != VDO_SUCCESS)
			return result;
	}

	return encode_index_config(&geometry->index_config, buffer);
}
#endif /* VDO_USER */

/**
 * decode_geometry_block() - Decode the on-disk representation of a geometry block, up to but not
 *                           including the checksum, from a buffer.
 * @buffer: A buffer positioned at the start of the block.
 * @geometry: The structure to receive the decoded volume geometry fields.
 *
 * Return: UDS_SUCCESS or an error.
 */
static int decode_geometry_block(struct buffer *buffer, struct volume_geometry *geometry)
{
	int result;
	struct header header;

	if (!has_same_bytes(buffer, MAGIC_NUMBER, MAGIC_NUMBER_SIZE))
		return VDO_BAD_MAGIC;

	result = skip_forward(buffer, MAGIC_NUMBER_SIZE);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_decode_header(buffer, &header);
	if (result != VDO_SUCCESS)
		return result;

	if (header.version.major_version <= 4)
		result = vdo_validate_header(&GEOMETRY_BLOCK_HEADER_4_0, &header, true, __func__);
	else
		result = vdo_validate_header(&GEOMETRY_BLOCK_HEADER_5_0, &header, true, __func__);
	if (result != VDO_SUCCESS)
		return result;

	result = decode_volume_geometry(buffer, geometry, header.version.major_version);
	if (result != VDO_SUCCESS)
		return result;

	/* Leave the CRC for the caller to decode and verify. */
	return ASSERT(header.size == (uncompacted_amount(buffer) + sizeof(u32)),
		      "should have decoded up to the geometry checksum");
}

/**
 * vdo_parse_geometry_block() - Decode and validate an encoded geometry block.
 * @block: The encoded geometry block.
 * @geometry: The structure to receive the decoded fields.
 */
int __must_check vdo_parse_geometry_block(unsigned char *block, struct volume_geometry *geometry)
{
	u32 checksum, saved_checksum;
	struct buffer *buffer;
	int result;

	result = wrap_buffer(block, VDO_BLOCK_SIZE, VDO_BLOCK_SIZE, &buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = decode_geometry_block(buffer, geometry);
	if (result != VDO_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	/* Checksum everything decoded so far. */
	checksum = vdo_crc32(block, uncompacted_amount(buffer));
	result = get_u32_le_from_buffer(buffer, &saved_checksum);
	if (result != VDO_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	/* Finished all decoding. Everything that follows is validation code. */
	free_buffer(UDS_FORGET(buffer));

	if (!is_loadable_release_version(geometry->release_version))
		return uds_log_error_strerror(VDO_UNSUPPORTED_VERSION,
					      "release version %d cannot be loaded",
					      geometry->release_version);

	return ((checksum == saved_checksum) ? VDO_SUCCESS : VDO_CHECKSUM_MISMATCH);
}

#ifndef __KERNEL__
/**
 * encode_geometry_block() - Encode the on-disk representation of a geometry block, up to but not
 *                           including the checksum, into a buffer.
 * @geometry: The volume geometry to encode into the block.
 * @buffer: A buffer positioned at the start of the block.
 * @version: The geometry block version to encode.
 *
 * Return: UDS_SUCCESS or an error.
 */
static int
encode_geometry_block(const struct volume_geometry *geometry, struct buffer *buffer, u32 version)
{
	const struct header *header;
	int result;

	result = put_bytes(buffer, MAGIC_NUMBER_SIZE, MAGIC_NUMBER);
	if (result != VDO_SUCCESS)
		return result;

	header = ((version <= 4) ? &GEOMETRY_BLOCK_HEADER_4_0 : &GEOMETRY_BLOCK_HEADER_5_0);
	result = vdo_encode_header(header, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = encode_volume_geometry(geometry, buffer, version);
	if (result != VDO_SUCCESS)
		return result;

	/* Leave the CRC for the caller to compute and encode. */
	return ASSERT(header->size == (content_length(buffer) + sizeof(u32)),
		      "should have decoded up to the geometry checksum");
}

/**
 * vdo_load_volume_geometry() - Load the volume geometry from a layer.
 * @layer: The layer to read and parse the geometry from.
 * @geometry: The structure to receive the decoded fields.
 */
int vdo_load_volume_geometry(PhysicalLayer *layer, struct volume_geometry *geometry)
{
	char *block;
	int result;

	result = layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE, "geometry block", &block);
	if (result != VDO_SUCCESS)
		return result;

	result = layer->reader(layer, VDO_GEOMETRY_BLOCK_LOCATION, 1, block);
	if (result != VDO_SUCCESS) {
		UDS_FREE(block);
		return result;
	}

	result = vdo_parse_geometry_block((u8 *) block, geometry);
	UDS_FREE(block);
	return result;
}

/**
 * vdo_compute_index_blocks() - Compute the index size in blocks from the index_config.
 * @index_config: The index config.
 * @index_blocks_ptr: A pointer to return the index size in blocks.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_compute_index_blocks(const struct index_config *index_config,
			     block_count_t *index_blocks_ptr)
{
	int result;
	u64 index_bytes;
	block_count_t index_blocks;
	struct uds_parameters uds_parameters = {
		.memory_size = index_config->mem,
		.sparse = index_config->sparse,
	};

	result = uds_compute_index_size(&uds_parameters, &index_bytes);
	if (result != UDS_SUCCESS)
		return uds_log_error_strerror(result, "error computing index size");

	index_blocks = index_bytes / VDO_BLOCK_SIZE;
	if ((((u64) index_blocks) * VDO_BLOCK_SIZE) != index_bytes)
		return uds_log_error_strerror(VDO_PARAMETER_MISMATCH,
					      "index size must be a multiple of block size %d",
					      VDO_BLOCK_SIZE);

	*index_blocks_ptr = index_blocks;
	return VDO_SUCCESS;
}

/**
 * vdo_initialize_volume_geometry() - Initialize a volume_geometry for a VDO.
 * @nonce: The nonce for the VDO.
 * @uuid: The uuid for the VDO.
 * @index_config: The index config of the VDO.
 * @geometry: The geometry being initialized.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_initialize_volume_geometry(nonce_t nonce,
				   uuid_t *uuid,
				   const struct index_config *index_config,
				   struct volume_geometry *geometry)
{
	int result;
	block_count_t index_size = 0;

	if (index_config != NULL) {
		result = vdo_compute_index_blocks(index_config, &index_size);
		if (result != VDO_SUCCESS)
			return result;
	}

	*geometry = (struct volume_geometry) {
		.release_version = VDO_CURRENT_RELEASE_VERSION_NUMBER,
		.nonce = nonce,
		.bio_offset = 0,
		.regions = {
			[VDO_INDEX_REGION] = {
				.id = VDO_INDEX_REGION,
				.start_block = 1,
			},
			[VDO_DATA_REGION] = {
				.id = VDO_DATA_REGION,
				.start_block = 1 + index_size,
			}
		}
	};

	uuid_copy(geometry->uuid, *uuid);
	if (index_size > 0)
		memcpy(&geometry->index_config, index_config, sizeof(struct index_config));

	return VDO_SUCCESS;
}

/**
 * vdo_clear_volume_geometry() - Zero out the geometry on a layer.
 * @layer: The layer to clear.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_clear_volume_geometry(PhysicalLayer *layer)
{
	char *block;
	int result;

	result = layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE, "geometry block", &block);
	if (result != VDO_SUCCESS)
		return result;

	result = layer->writer(layer, VDO_GEOMETRY_BLOCK_LOCATION, 1, block);
	UDS_FREE(block);
	return result;
}

/**
 * vdo_write_volume_geometry() - Write a geometry block for a VDO.
 * @layer: The layer on which to write.
 * @geometry: The volume_geometry to be written.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_write_volume_geometry(PhysicalLayer *layer, struct volume_geometry *geometry)
{
	return vdo_write_volume_geometry_with_version(layer,
						      geometry,
						      DEFAULT_GEOMETRY_BLOCK_VERSION);
}

/**
 * vdo_write_volume_geometry_with_version() - Write a specific version of geometry block for a VDO.
 * @layer: The layer on which to write.
 * @geometry: The VolumeGeometry to be written.
 * @version: The version of VolumeGeometry to write.
 *
 * Return: VDO_SUCCESS or an error.
 */
int __must_check
vdo_write_volume_geometry_with_version(PhysicalLayer *layer,
				       struct volume_geometry *geometry,
				       u32 version)
{
	char *block;
	struct buffer *buffer;
	u32 checksum;
	int result;

	result = layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE, "geometry block", &block);
	if (result != VDO_SUCCESS)
		return result;

	result = wrap_buffer((u8 *) block, VDO_BLOCK_SIZE, 0, &buffer);
	if (result != VDO_SUCCESS) {
		UDS_FREE(block);
		return result;
	}

	result = encode_geometry_block(geometry, buffer, version);
	if (result != VDO_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		UDS_FREE(block);
		return result;
	}

	/* Checksum everything encoded so far and then encode the checksum. */
	checksum = vdo_crc32((u8 *) block, content_length(buffer));
	result = put_u32_le_into_buffer(buffer, checksum);
	if (result != VDO_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		UDS_FREE(block);
		return result;
	}

	/* Write it. */
	result = layer->writer(layer, VDO_GEOMETRY_BLOCK_LOCATION, 1, block);
	free_buffer(UDS_FORGET(buffer));
	UDS_FREE(block);
	return result;
}
#endif /* not __KERNEL__ */

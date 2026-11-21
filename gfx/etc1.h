/* ETC1 -> DXT1 block transcoder (portable C11).
 *
 * The Vita GPU samples DXT1/3/5 and PVRTC but not ETC1, which the game's .ktx textures use. Both formats store 4x4 texels in 8 bytes,
 * so a texture keeps its size and layout: each block is decoded to RGB and re-encoded as DXT1 (lossy, bounding-box endpoints).
 */
#ifndef BV_ETC1_H
#define BV_ETC1_H

#include <stddef.h>
#include <stdint.h>

/* Decodes one 8-byte ETC1 block into 16 RGB texels, row-major (index = y * 4 + x). */
void bv_etc1_decode_block(const uint8_t blk[8], uint8_t rgb[16][3]);
/* Encodes 16 RGB texels (row-major) into one 8-byte DXT1 block (4-colour mode, no alpha). */
void bv_dxt1_encode_block(const uint8_t rgb[16][3], uint8_t out[8]);
/* The slower nearest-colour encoder used up to update 32; kept as the quality reference for the host test. */
void bv_dxt1_encode_block_ref(const uint8_t rgb[16][3], uint8_t out[8]);
/* Decodes a DXT1 block (used by the tests and the quality report). */
void bv_dxt1_decode_block(const uint8_t blk[8], uint8_t rgb[16][3]);

/* Converts `nblocks` consecutive ETC1 blocks to DXT1 blocks. in and out may not overlap. */
void bv_etc1_to_dxt1(const uint8_t *etc1, uint8_t *dxt1, size_t nblocks);

#endif

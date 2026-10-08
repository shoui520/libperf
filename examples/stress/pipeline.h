#ifndef STRESS_PIPELINE_H
#define STRESS_PIPELINE_H

#include <stdint.h>

#define TILE_WORDS 1024U
#define ARENA_WORDS (2U * 1024 * 1024 / 4)

typedef struct {
	uint32_t input[TILE_WORDS], transformed[TILE_WORDS];
	unsigned char tile[TILE_WORDS], encoded[TILE_WORDS * 2];
	uint32_t *arena;
	unsigned encodedSize;
} Pipeline;

void generateTile(Pipeline *p, uint32_t seed);
void transformTile(Pipeline *p);
void compressTile(Pipeline *p);
uint32_t validateTile(Pipeline *p);
uint32_t referenceChecksum(uint32_t seed);
#endif

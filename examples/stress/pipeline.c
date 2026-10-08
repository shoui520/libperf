#include "pipeline.h"
#include <arm_neon.h>

static uint32_t hashWord(uint32_t hash, uint32_t x)
{
	return (hash ^ x) * 16777619U;
}

__attribute__((noinline)) void generateTile(Pipeline *p, uint32_t seed)
{
	for (unsigned i = 0; i < TILE_WORDS; i++) {
		p->input[i] = seed + i * 2654435761U;
		p->tile[i] = (unsigned char)((seed + (i >> 3) * 37U) & 255);
		unsigned at = (seed + i * 4099U) & (ARENA_WORDS - 1);
		p->arena[at] = p->input[i];
	}
}

__attribute__((noinline)) void transformTile(Pipeline *p)
{
	uint32x4_t factor = vdupq_n_u32(1664525U), add = vdupq_n_u32(1013904223U);
	for (unsigned i = 0; i < TILE_WORDS; i += 4) {
		uint32x4_t x = vld1q_u32(p->input + i);
		for (unsigned j = 0; j < 16; j++)
			x = vaddq_u32(vmulq_u32(x, factor), add);
		vst1q_u32(p->transformed + i, x);
	}
}

__attribute__((noinline)) void compressTile(Pipeline *p)
{
	unsigned out = 0;
	for (unsigned i = 0; i < TILE_WORDS;) {
		unsigned char value = p->tile[i];
		unsigned count = 1;
		while (i + count < TILE_WORDS && p->tile[i + count] == value && count < 255)
			count++;
		p->encoded[out++] = (unsigned char)count;
		p->encoded[out++] = value;
		i += count;
	}
	p->encodedSize = out;
}

__attribute__((noinline)) uint32_t validateTile(Pipeline *p)
{
	uint32_t h = 2166136261U;
	for (unsigned i = 0; i < TILE_WORDS; i++)
		h = hashWord(h, p->transformed[i]);
	unsigned pos = 0;
	for (unsigned i = 0; i < p->encodedSize; i += 2) {
		unsigned count = p->encoded[i], value = p->encoded[i + 1];
		if (!count || pos + count > TILE_WORDS)
			return 0;
		for (unsigned j = 0; j < count; j++) {
			if (p->tile[pos++] != value)
				return 0;
			h = hashWord(h, value);
		}
	}
	return pos == TILE_WORDS ? h : 0;
}

/* Independent scalar oracle: no queue, encoded stream, or SIMD dependency. */
uint32_t referenceChecksum(uint32_t seed)
{
	uint32_t h = 2166136261U;
	for (unsigned i = 0; i < TILE_WORDS; i++) {
		uint32_t x = seed + i * 2654435761U;
		for (unsigned j = 0; j < 16; j++)
			x = x * 1664525U + 1013904223U;
		h = hashWord(h, x);
	}
	for (unsigned i = 0; i < TILE_WORDS; i++)
		h = hashWord(h, (seed + (i >> 3) * 37U) & 255);
	return h;
}

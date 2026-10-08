#include "workloads.h"
#include <arm_neon.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WORDS (8U * 1024 * 1024 / sizeof(uint32_t))
#define FLOATS 4096U

static uint32_t *memory, *copy, *predictable;
static float *input, *weight, *output;
static volatile uint32_t sink;
static volatile float floatSink;

__attribute__((noinline)) void emptyWork(void)
{
	__asm__ volatile("" ::: "memory");
}

__attribute__((noinline)) static void integerChain(void)
{
	uint32_t x = 123;
	for (unsigned i = 0; i < 65536; i++) {
		x = (x * 1664525U + 1013904223U) ^ (x >> 7);
		__asm__ volatile("" : "+r"(x));
	}
	sink = x;
}

/* The branch is deliberate; neither side is replaced by conditional moves. */
static inline uint32_t branchStep(uint32_t x, uint32_t condition)
{
	__asm__ volatile("cmp %1, #0\n beq 1f\n add %0, %0, #3\n b 2f\n"
			 "1: sub %0, %0, #3\n 2:"
			 : "+r"(x)
			 : "r"(condition)
			 : "cc");
	return x;
}

__attribute__((noinline)) static void branchPredictable(void)
{
	uint32_t x = 1;
	for (unsigned i = 0; i < 32768; i++)
		x = branchStep(x, *(volatile uint32_t *)&predictable[i] & 1);
	sink = x;
}

__attribute__((noinline)) static void branchRandom(void)
{
	uint32_t x = 1;
	for (unsigned i = 0; i < 32768; i++)
		x = branchStep(x, *(volatile uint32_t *)&memory[i] & 1);
	sink = x;
}

__attribute__((noinline)) static void cacheHot(void)
{
	uint32_t x = 0;
	for (unsigned i = 0; i < 32768; i++)
		x += *(volatile uint32_t *)&memory[(i * 16U) & 1023];
	sink = x;
}

__attribute__((noinline)) static void cacheCold(void)
{
	uint32_t x = 0;
	for (unsigned i = 0; i < 32768; i++)
		x += *(volatile uint32_t *)&memory[(i * 4099U) & (WORDS - 1)];
	sink = x;
}

__attribute__((noinline)) static void scalarFloat(void)
{
	for (unsigned pass = 0; pass < 32; pass++) {
		for (unsigned i = 0; i < FLOATS; i++)
			output[i] = input[i] * 1.001f + weight[i];
		__asm__ volatile("" ::: "memory");
	}
	floatSink = output[FLOATS - 1];
}

__attribute__((noinline)) static void neonFloat(void)
{
	float32x4_t factor = vdupq_n_f32(1.001f);
	for (unsigned pass = 0; pass < 32; pass++) {
		for (unsigned i = 0; i < FLOATS; i += 4) {
			float32x4_t a = vld1q_f32(input + i), b = vld1q_f32(weight + i);
			vst1q_f32(output + i, vaddq_f32(vmulq_f32(a, factor), b));
		}
		__asm__ volatile("" ::: "memory");
	}
	floatSink = output[FLOATS - 1];
}

__attribute__((noinline)) static void copyBlock(void)
{
	memcpy(copy, memory, 512U * 1024);
	__asm__ volatile("" ::: "memory");
	sink = copy[65535];
}

const Work workloads[] = {
    {"integer_chain", "65,536 dependent integer steps", integerChain},
    {"branch_predictable", "32,768 input loads/branches; sorted", branchPredictable},
    {"branch_random", "32,768 branches; random input loads", branchRandom},
    {"cache_hot", "32,768 loads over a 4 KiB set", cacheHot},
    {"cache_cold", "32,768 loads over an 8 MiB set", cacheCold},
    {"scalar_float", "131,072 float multiply/add elements", scalarFloat},
    {"neon_float", "Same float elements, four per vector", neonFloat},
    {"memcpy_512KiB", "Copy 512 KiB using libc memcpy", copyBlock}};
const unsigned workloadCount = sizeof(workloads) / sizeof(workloads[0]);
int workloadsInit(void)
{
	memory = malloc(8U * 1024 * 1024);
	copy = malloc(512U * 1024);
	predictable = malloc(32768 * sizeof(uint32_t));
	input = malloc(FLOATS * sizeof(float));
	weight = malloc(FLOATS * sizeof(float));
	output = malloc(FLOATS * sizeof(float));
	if (!memory || !copy || !predictable || !input || !weight || !output)
		return -1;
	uint32_t x = 0x12345678;
	for (unsigned i = 0; i < WORDS; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		memory[i] = x;
	}
	for (unsigned i = 0; i < 32768; i++)
		predictable[i] = i >= 16384;
	for (unsigned i = 0; i < FLOATS; i++) {
		input[i] = (float)(i % 101) / 101;
		weight[i] = (float)(i % 37) / 37;
	}
	return 0;
}

int workloadsCheck(void)
{
	scalarFloat();
	float expected[FLOATS];
	memcpy(expected, output, sizeof(expected));
	neonFloat();
	for (unsigned i = 0; i < FLOATS; i++)
		if (fabsf(expected[i] - output[i]) > 0.00001f)
			return -1;
	copyBlock();
	return memcmp(copy, memory, 512U * 1024) ? -1 : 0;
}

void workloadsFree(void)
{
	free(memory);
	free(copy);
	free(predictable);
	free(input);
	free(weight);
	free(output);
}

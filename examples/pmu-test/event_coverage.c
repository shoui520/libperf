#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include <stdlib.h>
#include "libperf.h"

typedef void (*Log)(const char *, ...);
typedef struct {
	unsigned code;
	const char *name;
} Event;

static const Event events[] = {
    {0x00, "SOFT_INCREMENT"},
    {0x01, "ICACHE_MISS"},
    {0x02, "ITLB_MISS"},
    {0x03, "DCACHE_MISS"},
    {0x04, "DCACHE_ACCESS"},
    {0x05, "DTLB_MISS"},
    {0x06, "DATA_READ"},
    {0x07, "DATA_WRITE"},
    {0x09, "EXCEPTION_TAKEN"},
    {0x0A, "EXCEPTION_RETURN"},
    {0x0B, "WRITE_CONTEXTID"},
    {0x0C, "SOFT_CHANGEPC"},
    {0x0D, "IMMEDIATE_BRANCH"},
    {0x0F, "UNALIGNED"},
    {0x10, "BRANCH_MISPREDICT"},
    {0x11, "CYCLE_COUNT"},
    {0x12, "PREDICT_BRANCH"},
    {0x40, "JAZELLE_BYTECODE"},
    {0x41, "JAZELLE_SOFTWARE"},
    {0x42, "JAZELLE_BACK_BRANCH"},
    {0x50, "COHERENT_LF_MISS"},
    {0x51, "COHERENT_LF_HIT"},
    {0x60, "ICACHE_STALL"},
    {0x61, "DCACHE_STALL"},
    {0x62, "MAINTLB_STALL"},
    {0x63, "STREX_PASSED"},
    {0x64, "STREX_FAILED"},
    {0x65, "DATA_EVICTION"},
    {0x66, "ISSUE_NO_DISPATCH"},
    {0x67, "ISSUE_EMPTY"},
    {0x68, "INST_RENAME"},
    {0x6E, "PREDICT_FUNC_RET"},
    {0x70, "MAIN_PIPE"},
    {0x71, "SECOND_PIPE"},
    {0x72, "LS_PIPE"},
    {0x73, "FPU_RENAME"},
    {0x74, "NEON_RENAME"},
    {0x80, "PLD_STALL"},
    {0x81, "WRITE_STALL"},
    {0x82, "INST_MAINTLB_STALL"},
    {0x83, "DATA_MAINTLB_STALL"},
    {0x84, "INST_UTLB_STALL"},
    {0x85, "DATA_UTLB_STALL"},
    {0x86, "DMB_STALL"},
    {0x8A, "INTEGER_CLOCK"},
    {0x8B, "DATAENGINE_CLOCK"},
    {0x90, "ISB"},
    {0x91, "DSB"},
    {0x92, "DMB"},
    {0x93, "EXT_INTERRUPT"},
    {0xA0, "PLE_LINE_REQ_COMPLETED"},
    {0xA1, "PLE_CHANNEL_SKIPPED"},
    {0xA2, "PLE_FIFO_FLUSH"},
    {0xA3, "PLE_REQ_COMPLETED"},
    {0xA4, "PLE_FIFO_OVERFLOW"},
    {0xA5, "PLE_REQ_PROGRAMMED"},
};
static volatile uint32_t sink;
static uint32_t *memory;
static volatile uint32_t exclusiveWord __attribute__((aligned(16)));

/* Ordinary executable text; assembler owns its size and branch distances. */
void instructionFootprint(void);
unsigned coverageLoads(const uint32_t *buffer, unsigned n);
void coverageDmbVariants(uint32_t *buffer, unsigned n);
void coverageVfp(unsigned n);
void coverageNeon(unsigned n);
__attribute__((noinline)) static unsigned branchLeaf(unsigned x)
{
	return (x * 1664525U + 1013904223U) ^ (x >> 7);
}

/* Available user instructions only: no MMIO or optional engine configuration. */
__attribute__((noinline)) static void mixed(unsigned n, unsigned swMask)
{
	unsigned x = 1;
	for (unsigned i = 0; i < n; i++) {
		unsigned index = ((i * 4099U) & ((8U * 1024 * 1024 / 4) - 1));
		x ^= *(volatile uint32_t *)&memory[index];
		*(volatile uint32_t *)&memory[index] = x + i;
		__asm__ volatile(
		    "pld [%0]" ::"r"(&memory[(index + 64) & ((8U * 1024 * 1024 / 4) - 1)]));
		if (x & 0x400)
			x = branchLeaf(x);
		else
			x = branchLeaf(x ^ i);
		uint32_t value, result;
		__asm__ volatile("ldrex %0, [%2]\n strex %1, %0, [%2]\n"
				 "ldrex %0, [%2]\n clrex\n strex %1, %0, [%2]"
				 : "=&r"(value), "=&r"(result)
				 : "r"(&exclusiveWord)
				 : "memory");
		__asm__ volatile("vadd.f32 s0, s0, s0\n vadd.i32 q1, q1, q1\n"
				 "dmb sy\n dsb sy\n isb sy" ::
				     : "d0", "d2", "d3", "memory");
		if (swMask)
			scePerfArmPmonSoftwareIncrement(swMask);
		if (!(i & 63)) {
			instructionFootprint();
			sceKernelGetThreadId();
		}
	}
	sink = x;
}

enum {
	READ,
	WRITE,
	UNALIGNED,
	ISB,
	DSB,
	DMB,
	EXCLUSIVE_OK,
	EXCLUSIVE_FAIL,
	FLOAT,
	NEON,
	WARM,
	COLD,
	DMB_VARIANTS,
	BURST_WRITE,
	BURST_PLD
};
__attribute__((noinline)) static void workload(unsigned kind, unsigned n)
{
	uint32_t word = 0, result;
	uint32_t *const buffer = memory;
	if (kind == FLOAT) {
		coverageVfp(n);
		return;
	}
	if (kind == NEON) {
		coverageNeon(n);
		return;
	}
	if (kind == READ) {
		sink = coverageLoads(buffer, n);
		return;
	}
	if (kind == DMB_VARIANTS) {
		coverageDmbVariants(buffer, n);
		return;
	}
	const unsigned char *unaligned = (const unsigned char *)buffer + 1;
	for (unsigned i = 0; i < n; i++) {
		switch (kind) {
		case READ:
			break;
		case WRITE:
			__asm__ volatile("str %0, [%1]" ::"r"(word), "r"(buffer) : "memory");
			break;
		case UNALIGNED:
			__asm__ volatile("ldr %0, [%1]" : "=r"(word) : "r"(unaligned) : "memory");
			break;
		case ISB:
			__asm__ volatile("isb sy" ::: "memory");
			break;
		case DSB:
			__asm__ volatile("dsb sy" ::: "memory");
			break;
		case DMB:
			__asm__ volatile("dmb sy" ::: "memory");
			break;
		case EXCLUSIVE_OK:
			__asm__ volatile("ldrex %0, [%2]\n strex %1, %0, [%2]"
					 : "=&r"(word), "=&r"(result)
					 : "r"(&exclusiveWord)
					 : "memory");
			break;
		case EXCLUSIVE_FAIL:
			__asm__ volatile("ldrex %0, [%2]\n clrex\n strex %1, %0, [%2]"
					 : "=&r"(word), "=&r"(result)
					 : "r"(&exclusiveWord)
					 : "memory");
			break;
		case FLOAT:
			__asm__ volatile(".rept 8\n vadd.f32 s0, s0, s0\n .endr" ::: "d0");
			break;
		case NEON:
			__asm__ volatile(".rept 8\n vadd.i32 q1, q1, q1\n .endr" ::: "d2", "d3");
			break;
		case WARM:
			word ^= *(volatile uint32_t *)&buffer[(i * 16U) & 1023];
			break;
		case BURST_WRITE:
			*(volatile uint32_t *)&buffer[(i * 4112U) & ((8U * 1024 * 1024 / 4) - 1)] =
			    i;
			break;
		case BURST_PLD:
			__asm__ volatile(
			    "pld [%0]" ::"r"(&buffer[(i * 4112U) & ((8U * 1024 * 1024 / 4) - 1)]));
			break;
		case COLD:
			word ^= *(
			    volatile uint32_t *)&buffer[(i * 4112U) & ((8U * 1024 * 1024 / 4) - 1)];
			break;
		}
	}
	sink = word;
}

static int measure(unsigned code, unsigned kind, unsigned n, unsigned *value)
{
	int r = scePerfArmPmonSelectEvent(0, 0, code);
	if (!r)
		r = scePerfArmPmonReset(0);
	if (!r)
		r = scePerfArmPmonStart(0);
	if (!r)
		workload(kind, n);
	int stop = scePerfArmPmonStop(0);
	if (!r)
		r = stop;
	if (!r)
		r = scePerfArmPmonGetCounterValue(0, 0, value);
	return r;
}

int testEventCoverage(Log log)
{
	int failed = 0;
	SceKernelThreadInfo info = {.size = sizeof(info)};
	if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0)
		return 1;
	memory = malloc(8U * 1024 * 1024);
	if (!memory) {
		log("Event memory allocation failed");
		return 1;
	}
	for (unsigned i = 0; i < 8U * 1024 * 1024 / 4; i++)
		memory[i] = i;
	log("EVENT survey: core,code,name,base,n512,n1024,status");
	for (unsigned core = 0; core < 3; core++) {
		if (sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0 << core) <
		    0) {
			failed = 1;
			break;
		}
		for (unsigned base = 0; base < sizeof(events) / sizeof(events[0]); base += 6) {
			unsigned count = sizeof(events) / sizeof(events[0]) - base;
			if (count > 6)
				count = 6;
			unsigned values[3][6] = {{0}}, sw = 0;
			int r = 0;
			for (unsigned c = 0; c < 6; c++) {
				unsigned code = c < count ? events[base + c].code : 0;
				int select = scePerfArmPmonSelectEvent(0, c, code);
				if (select < 0)
					r = select;
				if (!code)
					sw |= 1U << c;
			}
			for (unsigned run = 0; run < 3 && !r; run++) {
				r = scePerfArmPmonReset(0);
				if (!r)
					r = scePerfArmPmonStart(0);
				if (!r)
					mixed(run == 0 ? 0 : run == 1 ? 512 : 1024, sw);
				int stop = scePerfArmPmonStop(0);
				if (!r)
					r = stop;
				for (unsigned c = 0; c < count && !r; c++)
					r = scePerfArmPmonGetCounterValue(0, c, &values[run][c]);
			}
			for (unsigned c = 0; c < count; c++) {
				const Event *e = &events[base + c];
				log("EVENT,%u,%02X,%s,%u,%u,%u,%s", core, e->code, e->name,
				    values[0][c], values[1][c], values[2][c],
				    r				     ? "API_FAIL"
				    : (values[1][c] || values[2][c]) ? "RESPONDS"
								     : "ZERO_IN_WORKLOAD");
				if (r ||
				    (!e->code && (values[1][c] != 512 || values[2][c] != 1024)))
					failed = 1;
			}
		}
		static const struct {
			unsigned code, kind, each;
			const char *name;
		} targets[] = {{0x06, READ, 1, "loads"},
			       {0x07, WRITE, 1, "stores"},
			       {0x0F, UNALIGNED, 1, "unaligned"},
			       {0x90, ISB, 1, "isb"},
			       {0x91, DSB, 1, "dsb"},
			       {0x92, DMB, 1, "dmb"},
			       {0x63, EXCLUSIVE_OK, 1, "strex_pass"},
			       {0x64, EXCLUSIVE_FAIL, 1, "strex_fail"},
			       {0x92, DMB_VARIANTS, 4, "dmb_variants"},
			       {0x73, FLOAT, 8, "vfp"},
			       {0x74, NEON, 8, "neon"}};
		for (unsigned j = 0; j < sizeof(targets) / sizeof(targets[0]); j++) {
			unsigned a = 0, b = 0, expected = 4096 * targets[j].each;
			int r = measure(targets[j].code, targets[j].kind, 4096, &a);
			if (!r)
				r = measure(targets[j].code, targets[j].kind, 8192, &b);
			unsigned delta = b - a, tolerance = expected / 20 + 64;
			int good = !r && b >= a && delta + tolerance >= expected &&
				   delta <= expected + tolerance;
			log("TARGET,%u,%02X,%s,%u,%u,delta=%u,expected=%u,%s", core,
			    targets[j].code, targets[j].name, a, b, delta, expected,
			    good ? "PASS" : "REVIEW");
			/* A REVIEW is measurement evidence, not a libperf API failure. */
			if (r)
				failed = 1;
		}
		static const struct {
			unsigned code, kind;
		} stalls[] = {{0x80, BURST_PLD}, {0x81, BURST_WRITE}, {0x86, DMB_VARIANTS}};
		for (unsigned j = 0; j < sizeof(stalls) / sizeof(stalls[0]); j++) {
			unsigned value = 0;
			int r = measure(stalls[j].code, stalls[j].kind, 131072, &value);
			log("STRESS,%u,%02X,value=%u,%s", core, stalls[j].code, value,
			    r	    ? "API_FAIL"
			    : value ? "RESPONDS"
				    : "ZERO_IN_WORKLOAD");
			if (r)
				failed = 1;
		}
		for (unsigned code = 3; code <= 5; code += 2) {
			unsigned warm = 0, cold = 0;
			int r = measure(code, WARM, 16384, &warm);
			if (!r)
				r = measure(code, COLD, 16384, &cold);
			log("CACHE,%u,%02X,warm=%u,cold=%u,%s", core, code, warm, cold,
			    !r && cold > warm ? "RESPONDS" : "REVIEW");
			if (r)
				failed = 1;
		}
	}
	scePerfArmPmonStop(0);
	if (sceKernelChangeThreadCpuAffinityMask(0, info.currentCpuAffinityMask) < 0)
		failed = 1;
	free(memory);
	memory = NULL;
	log("Event coverage API/software checks: %s", failed ? "FAIL" : "PASS");
	return failed;
}

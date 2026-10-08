#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <vita2d.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "libperf.h"
#include "workloads.h"

#define MAX_WORK 8
#define REPEATS 7
#define CALLS 8
#define METRICS 8

static const unsigned codes[2][6] = {{0x68, 0x03, 0x05, 0x10, 0x06, 0x07},
				     {0x73, 0x74, 0x01, 0x61, 0x90, 0x91}};
static const char *metricNames[2][METRICS] = {
    {"cycles", "us", "rename", "dcache_miss", "dtlb_miss", "branch_miss", "loads", "stores"},
    {"cycles", "us", "vfp_rename", "neon_rename", "icache_miss", "dcache_stall", "isb", "dsb"}};
typedef struct {
	uint64_t m[METRICS];
} Sample;

static Sample raw[2][MAX_WORK][REPEATS], overhead[2][REPEATS], median[2][MAX_WORK];
static uint64_t minimum[MAX_WORK], maximum[MAX_WORK];
static unsigned order[MAX_WORK], core, page, selected;
static int ready, errorCode, clockBefore, clockAfter;
static char statusText[128] = "Preparing profiler";
static vita2d_pgf *font;
static void text(float x, float y, unsigned color, const char *fmt, ...)
{
	char s[256];
	va_list a;
	va_start(a, fmt);
	vsnprintf(s, sizeof(s), fmt, a);
	va_end(a);
	if (font)
		vita2d_pgf_draw_text(font, x, y, color, 0.8f, s);
}

static uint64_t medianValue(uint64_t *v)
{
	for (unsigned i = 1; i < REPEATS; i++) {
		uint64_t x = v[i];
		unsigned j = i;
		while (j && v[j - 1] > x) {
			v[j] = v[j - 1];
			j--;
		}
		v[j] = x;
	}
	return v[REPEATS / 2];
}

static uint64_t adjusted(unsigned bank, unsigned w, unsigned run, unsigned m)
{
	uint64_t a = raw[bank][w][run].m[m], b = overhead[bank][run].m[m];
	return a > b ? a - b : 0;
}

static int measure(WorkFunction fn, Sample *s)
{
	int r = scePerfArmPmonReset(0);
	if (r < 0)
		return r;
	r = scePerfArmPmonStart(0);
	if (r < 0)
		return r;
	uint64_t t0 = scePerfGetTimebaseValue();
	for (unsigned i = 0; i < CALLS; i++)
		fn();
	uint64_t t1 = scePerfGetTimebaseValue();
	r = scePerfArmPmonStop(0);
	if (r < 0)
		return r;
	uint32_t value = 0;
	r = scePerfArmPmonGetCounterValue(0, 31, &value);
	if (r < 0)
		return r;
	s->m[0] = value;
	s->m[1] = (t1 - t0) & 0xFFFFFFFFFFFFULL;
	for (unsigned c = 0; c < 6; c++) {
		r = scePerfArmPmonGetCounterValue(0, c, &value);
		if (r < 0)
			return r;
		s->m[c + 2] = value;
	}
	return 0;
}

static void draw(void)
{
	unsigned white = RGBA8(235, 239, 247, 255), muted = RGBA8(150, 167, 188, 255),
		 blue = RGBA8(85, 185, 255, 255);
	vita2d_start_drawing();
	vita2d_clear_screen();
	text(20, 28, blue, "CPU FUNCTION PROFILER   |   Core %u   |   CPU %d MHz", core,
	     clockAfter);
	text(20, 54, muted, "%s", statusText);
	if (ready) {
		uint64_t sum = 0, best = UINT64_MAX, top = 0;
		for (unsigned w = 0; w < workloadCount; w++) {
			uint64_t c = median[0][w].m[0];
			sum += c;
			if (c < best && c)
				best = c;
			if (c > top)
				top = c;
		}
		if (!page) {
			text(20, 88, white, "Function");
			text(300, 88, white, "cycles/call");
			text(465, 88, white, "us/call");
			text(600, 88, white, "relative");
			text(740, 88, white, "mix share");
			for (unsigned row = 0; row < workloadCount; row++) {
				unsigned w = order[row];
				double cycles = (double)median[0][w].m[0] / CALLS;
				float y = 124 + row * 39;
				vita2d_draw_rectangle(18, y - 23,
						      top ? 410.0f * median[0][w].m[0] / top : 0,
						      31, RGBA8(25, 55, 82, 255));
				unsigned color = row == selected ? blue : white;
				text(22, y, color, "%s", workloads[w].name);
				text(300, y, color, "%.0f", cycles);
				text(465, y, color, "%.1f", (double)median[0][w].m[1] / CALLS);
				text(600, y, color, "%.2fx",
				     best ? (double)median[0][w].m[0] / best : 0);
				text(740, y, color, "%.1f%%",
				     sum ? 100.0 * median[0][w].m[0] / sum : 0);
			}
		} else {
			unsigned bank = page == 1 ? 0 : 1;
			text(20, 88, white, "Function");
			for (unsigned col = 0; col < 4; col++)
				text(300 + col * 150, 88, white, "%s", metricNames[bank][col + 2]);
			for (unsigned row = 0; row < workloadCount; row++) {
				unsigned w = order[row], color = row == selected ? blue : white;
				float y = 124 + row * 39;
				text(22, y, color, "%s", workloads[w].name);
				for (unsigned col = 0; col < 4; col++)
					text(300 + col * 150, y, color, "%.0f",
					     (double)median[bank][w].m[col + 2] / CALLS);
			}
		}
		unsigned w = order[selected];
		text(20, 455, blue, "%s", workloads[w].job);
		if (!page)
			text(
			    20, 478, muted,
			    "Cycle range/call: %.0f - %.0f | Same number of calls per function in mix",
			    (double)minimum[w] / CALLS, (double)maximum[w] / CALLS);
		else
			text(
			    20, 478, muted,
			    "Loads/stores per call: %.0f / %.0f | All events and raw runs saved as CSV",
			    (double)median[0][w].m[6] / CALLS, (double)median[0][w].m[7] / CALLS);
	} else
		text(20, 125, white,
		     errorCode ? "Profiling failed: 0x%08X" : "Measuring named functions ...",
		     (unsigned)errorCode);
	text(
	    20, 506, muted,
	    "Left/Right: view  Up/Down: function  Cross: rerun  Triangle: next core  Circle: exit");
	text(20, 530, muted,
	     "7-run medians; empty-call baseline removed. Rename events are approximate.");
	vita2d_end_drawing();
	vita2d_swap_buffers();
	vita2d_wait_rendering_done();
}

static int saveResults(void)
{
	mkdir("ux0:data/libperf-demo", 0777);
	FILE *f = fopen("ux0:data/libperf-demo/results.csv", "w");
	if (!f)
		return -1;
	fprintf(
	    f,
	    "# CPU Function Profiler,core=%u,clock_before_mhz=%d,clock_after_mhz=%d,repeats=%u,calls_per_batch=%u,timebase_mhz=%u\n",
	    core, clockBefore, clockAfter, REPEATS, CALLS, scePerfGetTimebaseFrequency());
	fprintf(f, "kind,bank,function,run,calls");
	fprintf(f, ",cycles,us,event0,event1,event2,event3,event4,event5\n");
	for (unsigned b = 0; b < 2; b++) {
		fprintf(f, "# bank%u", b);
		for (unsigned c = 0; c < 6; c++)
			fprintf(f, ",event%u=%s(0x%02X)", c, metricNames[b][c + 2], codes[b][c]);
		fputc('\n', f);
	}
	for (unsigned b = 0; b < 2; b++) {
		for (unsigned run = 0; run < REPEATS; run++) {
			fprintf(f, "baseline,%u,empty,%u,%u", b, run, CALLS);
			for (unsigned m = 0; m < METRICS; m++)
				fprintf(f, ",%llu", (unsigned long long)overhead[b][run].m[m]);
			fputc('\n', f);
		}
		for (unsigned w = 0; w < workloadCount; w++) {
			for (unsigned run = 0; run < REPEATS; run++) {
				fprintf(f, "raw,%u,%s,%u,%u", b, workloads[w].name, run, CALLS);
				for (unsigned m = 0; m < METRICS; m++)
					fprintf(f, ",%llu",
						(unsigned long long)raw[b][w][run].m[m]);
				fputc('\n', f);
			}
			fprintf(f, "median_adjusted,%u,%s,-1,%u", b, workloads[w].name, CALLS);
			for (unsigned m = 0; m < METRICS; m++)
				fprintf(f, ",%llu", (unsigned long long)median[b][w].m[m]);
			fputc('\n', f);
		}
	}
	int bad = ferror(f);
	if (fclose(f))
		bad = 1;
	return bad ? -1 : 0;
}

static int profile(void)
{
	ready = 0;
	errorCode = 0;
	clockBefore = scePowerGetArmClockFrequency();
	clockAfter = clockBefore;
	snprintf(statusText, sizeof(statusText), "Running 8 functions, two event banks, core %u",
		 core);
	draw();
	int r = sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0 << core);
	if (r < 0)
		return r;
	for (unsigned b = 0; b < 2; b++) {
		for (unsigned c = 0; c < 6; c++) {
			r = scePerfArmPmonSelectEvent(0, c, codes[b][c]);
			if (r < 0)
				return r;
		}
		for (unsigned run = 0; run < REPEATS; run++) {
			r = measure(emptyWork, &overhead[b][run]);
			if (r < 0)
				return r;
			for (unsigned offset = 0; offset < workloadCount; offset++) {
				unsigned w = (offset + run) % workloadCount;
				/* Keep graphics idle during collection to reduce memory-bus interference. */
				workloads[w]
				    .run(); /* Explicit warm-up outside the measured interval. */
				r = measure(workloads[w].run, &raw[b][w][run]);
				if (r < 0)
					return r;
			}
		}
	}
	clockAfter = scePowerGetArmClockFrequency();
	for (unsigned b = 0; b < 2; b++)
		for (unsigned w = 0; w < workloadCount; w++)
			for (unsigned m = 0; m < METRICS; m++) {
				uint64_t v[REPEATS];
				for (unsigned run = 0; run < REPEATS; run++)
					v[run] = adjusted(b, w, run, m);
				median[b][w].m[m] = medianValue(v);
				if (!b && !m) {
					minimum[w] = v[0];
					maximum[w] = v[REPEATS - 1];
				}
			}
	for (unsigned w = 0; w < workloadCount; w++)
		order[w] = w;
	for (unsigned i = 1; i < workloadCount; i++) {
		unsigned w = order[i], j = i;
		while (j && median[0][order[j - 1]].m[0] < median[0][w].m[0]) {
			order[j] = order[j - 1];
			j--;
		}
		order[j] = w;
	}
	int saved = saveResults();
	ready = 1;
	snprintf(statusText, sizeof(statusText),
		 saved			     ? "Measured; CSV could not be saved"
		 : clockBefore != clockAfter ? "Saved results.csv; CPU clock changed during run"
					     : "Saved ux0:data/libperf-demo/results.csv");
	return 0;
}

int main(void)
{
	vita2d_init();
	font = vita2d_load_default_pgf();
	int moduleStatus = 0;
	SceUID module =
	    sceKernelLoadStartModule("app0:libperf.suprx", 0, NULL, 0, NULL, &moduleStatus);
	if (module < 0)
		errorCode = module;
	else if (moduleStatus != SCE_KERNEL_START_SUCCESS)
		errorCode = moduleStatus ? moduleStatus : -1;
	else if (workloadCount > MAX_WORK || workloadsInit() < 0)
		errorCode = -1;
	else if (workloadsCheck() < 0) {
		errorCode = -1;
		snprintf(statusText, sizeof(statusText), "Float/copy correctness check failed");
	} else
		errorCode = profile();
	unsigned previous = 0;
	for (;;) {
		draw();
		SceCtrlData pad = {0};
		sceCtrlPeekBufferPositive(0, &pad, 1);
		unsigned pressed = pad.buttons & ~previous;
		previous = pad.buttons;
		if (pressed & SCE_CTRL_CIRCLE)
			break;
		if (ready) {
			if (pressed & SCE_CTRL_RIGHT)
				page = (page + 1) % 3;
			if (pressed & SCE_CTRL_LEFT)
				page = (page + 2) % 3;
			if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
				vita2d_wait_rendering_done();
				vita2d_free_pgf(font);
				font = vita2d_load_default_pgf();
			}
			if (pressed & SCE_CTRL_DOWN)
				selected = (selected + 1) % workloadCount;
			if (pressed & SCE_CTRL_UP)
				selected = (selected + workloadCount - 1) % workloadCount;
			if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_TRIANGLE)) {
				if (pressed & SCE_CTRL_TRIANGLE)
					core = (core + 1) % 3;
				errorCode = profile();
			}
		}
		sceKernelDelayThread(16666);
	}
	if (module >= 0)
		scePerfArmPmonStop(0);
	workloadsFree();
	sceKernelExitProcess(0);
	return 0;
}

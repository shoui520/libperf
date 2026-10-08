#include <psp2/ctrl.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vita2d.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include "libperf.h"

/* Test the PMU lifecycle without unlinking the user module's weak imports. */
int sceKernelPerfArmPmonOpen(void);
int sceKernelPerfArmPmonClose(void);
int testEventCoverage(void (*log)(const char *, ...));

static char lines[48][128];
static int lineCount, failed;
static FILE *report;
typedef struct {
	SceUID go, done, thread;
	int error;
	SceUInt32 count, cycles, instructions;
} Worker;

static Worker workers[3];
static void logLine(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int slot = lineCount % 48;
	vsnprintf(lines[slot], sizeof(lines[0]), fmt, args);
	if (report) {
		fprintf(report, "%s\n", lines[slot]);
		fflush(report);
	}
	lineCount++;
	va_end(args);
}

static int check(const char *name, int result)
{
	logLine("%s: 0x%08X", name, (unsigned)result);
	if (result < 0)
		failed = 1;
	return result >= 0;
}

static int workerMain(SceSize size, void *data)
{
	(void)size;
	Worker *w = *(Worker **)data;
	sceKernelSignalSema(w->done, 1);
	sceKernelWaitSema(w->go, 1, NULL);
	for (int i = 0; i < 64 && w->error == 0; i++)
		w->error = scePerfArmPmonSoftwareIncrement(1);
	if (!w->error)
		w->error = scePerfArmPmonGetCounterValue(0, 0, &w->count);
	volatile unsigned value = 1;
	for (unsigned i = 0; i < 100000; i++)
		value = value * 1664525u + i;
	if (!w->error)
		w->error = scePerfArmPmonGetCounterValue(0, 31, &w->cycles);
	if (!w->error)
		w->error = scePerfArmPmonGetCounterValue(0, 1, &w->instructions);
	sceKernelSignalSema(w->done, 1);
	/* Keep the target alive for the cross-thread counter read. */
	sceKernelWaitSema(w->go, 1, NULL);
	return 0;
}

static int createWorker(int i)
{
	Worker *w = &workers[i];
	w->go = sceKernelCreateSema("perf_go", 0, 0, 1, NULL);
	w->done = sceKernelCreateSema("perf_done", 0, 0, 1, NULL);
	w->thread = sceKernelCreateThread("perf_worker", workerMain, 0x80, 0x10000, 0,
					  SCE_KERNEL_CPU_MASK_USER_0 << i, NULL);
	if (w->go < 0 || w->done < 0 || w->thread < 0) {
		logLine("Worker %d creation: go=%08X done=%08X thread=%08X", i, (unsigned)w->go,
			(unsigned)w->done, (unsigned)w->thread);
		failed = 1;
		return 0;
	}
	if (!check("Worker start", sceKernelStartThread(w->thread, sizeof(w), &w)))
		return 0;
	SceUInt timeout = 3000000;
	return check("Worker ready", sceKernelWaitSema(w->done, 1, &timeout));
}

static int transientMain(SceSize size, void *data)
{
	(void)size;
	(void)data;
	sceKernelDelayThread(500);
	return 0;
}

static atomic_int churnDone;
static atomic_int churnError;
static int churnMain(SceSize size, void *data)
{
	(void)size;
	(void)data;
	for (int i = 0; i < 32; i++) {
		SceUID t = sceKernelCreateThread("perf_transient", transientMain, 0x80, 0x10000, 0,
						 SCE_KERNEL_CPU_MASK_USER_1, NULL);
		if (t < 0) {
			atomic_store(&churnError, t);
			break;
		}
		int ret = sceKernelStartThread(t, 0, NULL);
		if (ret >= 0)
			ret = sceKernelWaitThreadEnd(t, NULL, NULL);
		sceKernelDeleteThread(t);
		if (ret < 0) {
			atomic_store(&churnError, ret);
			break;
		}
	}
	atomic_store(&churnDone, 1);
	return 0;
}

static void testNonRazor(void)
{
	unsigned frequency = scePerfGetTimebaseFrequency();
	SceUInt64 t0 = scePerfGetTimebaseValue();
	SceUInt64 us0 = sceKernelGetSystemTimeWide();
	sceKernelDelayThread(100000);
	SceUInt64 t1 = scePerfGetTimebaseValue();
	SceUInt64 us1 = sceKernelGetSystemTimeWide();
	SceUInt64 ticks = (t1 - t0) & 0xFFFFFFFFFFFFULL;
	SceUInt64 elapsed = us1 - us0;
	logLine("Timebase: MHz=%u ticks=%llu us=%llu", frequency, (unsigned long long)ticks,
		(unsigned long long)elapsed);
	if (frequency != 1 || t0 >> 48 || t1 >> 48 || !ticks || !elapsed ||
	    ticks < elapsed * frequency * 95 / 100 || ticks > elapsed * frequency * 105 / 100)
		failed = 1;

	for (unsigned c = 0; c < 6; c++) {
		if (!check("Select software counter", scePerfArmPmonSelectEvent(0, c, 0)))
			return;
		if (!check("Set counter near wrap",
			   scePerfArmPmonSetCounterValue(0, c, 0xFFFFFFF0)))
			return;
	}
	SceKernelThreadInfo info = {.size = sizeof(info)};
	if (!check("Get main thread affinity",
		   sceKernelGetThreadInfo(sceKernelGetThreadId(), &info)))
		return;
	if (!check("Start all six self counters", scePerfArmPmonStart(0)))
		return;
	for (unsigned i = 0; i < 32; i++) {
		if (i % 4 == 0) {
			if (!check("Migrate main thread",
				   sceKernelChangeThreadCpuAffinityMask(
				       0, SCE_KERNEL_CPU_MASK_USER_0 << ((i / 4) % 3))))
				return;
			sceKernelDelayThread(1000);
		}
		if (scePerfArmPmonSoftwareIncrement(0x3F) < 0)
			failed = 1;
	}
	check("Stop self counters", scePerfArmPmonStop(0));
	check("Restore affinity",
	      sceKernelChangeThreadCpuAffinityMask(0, info.currentCpuAffinityMask));
	for (unsigned c = 0; c < 6; c++) {
		SceUInt32 value = 0;
		int ret = scePerfArmPmonGetCounterValue(0, c, &value);
		logLine("Counter %u wrap/migration: ret=%08X value=%u", c, (unsigned)ret,
			(unsigned)value);
		if (ret || value != 16)
			failed = 1;
	}
	int ret = scePerfArmPmonSetCounterValue(-1, 31, 0x11223344);
	check("ALL set cycle counter", ret);
	for (int i = 0; i < 3 && ret >= 0; i++) {
		SceUInt32 value = 0;
		ret = scePerfArmPmonGetCounterValue(workers[i].thread, 31, &value);
		logLine("Worker %d cycle write: ret=%08X value=%08X", i, (unsigned)ret,
			(unsigned)value);
		if (ret || value != 0x11223344)
			failed = 1;
	}
	check("Self cycle near wrap", scePerfArmPmonSetCounterValue(0, 31, 0xFFFFFFF0));
	check("Start cycle wrap", scePerfArmPmonStart(0));
	sceKernelDelayThread(1000);
	check("Stop cycle wrap", scePerfArmPmonStop(0));
	SceUInt32 value = 0;
	ret = scePerfArmPmonGetCounterValue(0, 31, &value);
	logLine("Cycle wrap: ret=%08X value=%08X", (unsigned)ret, (unsigned)value);
	if (ret || value >= 0xFFFFFFF0 || !value)
		failed = 1;
	if (check("ALL reset seven counters", scePerfArmPmonReset(-1))) {
		for (int i = 0; i < 3; i++)
			for (unsigned c = 0; c < 7; c++) {
				ret = scePerfArmPmonGetCounterValue(workers[i].thread,
								    c == 6 ? 31 : c, &value);
				if (ret || value)
					failed = 1;
			}
		logLine("ALL reset verification: %s", failed ? "FAIL" : "PASS");
	}
	SceUID churn = sceKernelCreateThread("perf_churn", churnMain, 0x80, 0x10000, 0,
					     SCE_KERNEL_CPU_MASK_USER_1, NULL);
	if (check("Create thread churn", churn) &&
	    check("Start churn", sceKernelStartThread(churn, 0, NULL))) {
		unsigned broadcasts = 0, vanished = 0;
		while (!atomic_load(&churnDone) && broadcasts < 256) {
			ret = scePerfArmPmonSelectEvent(-1, 2, 0);
			if (ret == (int)0x80028021 || ret == SCE_PERF_ERROR_INVALID_ARGUMENT)
				vanished++;
			else if (ret < 0) {
				check("Churn broadcast", ret);
				break;
			}
			broadcasts++;
			sceKernelDelayThread(100);
		}
		SceUInt timeout = 5000000;
		check("Wait churn", sceKernelWaitThreadEnd(churn, NULL, &timeout));
		check("Churn worker result", atomic_load(&churnError));
		sceKernelDeleteThread(churn);
		logLine("Thread churn: broadcasts=%u vanished=%u", broadcasts, vanished);
		if (!broadcasts)
			failed = 1;
	}
	ret = sceKernelPerfArmPmonClose();
	check("Close libperf", ret);
	ret = scePerfArmPmonSoftwareIncrement(1);
	logLine("Closed PMU guard: ret=%08X", (unsigned)ret);
	if (ret != SCE_PERF_ERROR_NOT_INITIALIZED)
		failed = 1;
	t0 = scePerfGetTimebaseValue();
	sceKernelDelayThread(1000);
	t1 = scePerfGetTimebaseValue();
	logLine("Timebase while PMU closed: ticks=%llu", (unsigned long long)(t1 - t0));
	if (t1 == t0)
		failed = 1;
	check("Reopen libperf", sceKernelPerfArmPmonOpen());
	ret = scePerfArmPmonSelectEvent(0, 0, 0);
	if (!ret)
		ret = scePerfArmPmonReset(0);
	if (!ret)
		ret = scePerfArmPmonStart(0);
	if (!ret)
		ret = scePerfArmPmonSoftwareIncrement(1);
	if (!ret)
		ret = scePerfArmPmonGetCounterValue(0, 0, &value);
	logLine("Reopened PMU: ret=%08X value=%u", (unsigned)ret, (unsigned)value);
	if (ret || value != 1)
		failed = 1;
	check("Final ALL stop", scePerfArmPmonStop(-1));
}

int main(void)
{
	vita2d_init();
	vita2d_pgf *font = vita2d_load_default_pgf();
	mkdir("ux0:data/retail-perf-test", 0777);
	report = fopen("ux0:data/retail-perf-test/results.txt", "w");
	logLine("Retail 3.65 CPU PMU test - build 9");
	logLine("Report: ux0:data/retail-perf-test/results.txt");
	int started = 0;
	for (int i = 0; i < 2; i++) {
		if (!createWorker(i))
			break;
		started++;
	}
	int status = 0;
	SceUID module = -1;
	if (!failed)
		module = sceKernelLoadStartModule("app0:libperf.suprx", 0, NULL, 0, NULL, &status);
	check("Load custom libperf", module);
	check("Module start status", status);
	if (status != SCE_KERNEL_START_SUCCESS)
		failed = 1;
	if (module >= 0 && status >= 0 && !failed) {
		logLine("Creating worker 2 after libperf open");
		if (createWorker(2))
			started++;
		int ok = !failed &&
			 check("ALL select software event", scePerfArmPmonSelectEvent(-1, 0, 0));
		if (ok)
			ok = check("ALL instruction rename event",
				   scePerfArmPmonSelectEvent(-1, 1, 0x68));
		if (ok)
			ok = check("ALL reset", scePerfArmPmonReset(-1));
		if (ok)
			ok = check("ALL start", scePerfArmPmonStart(-1));
		if (ok) {
			for (int i = 0; i < started; i++)
				sceKernelSignalSema(workers[i].go, 1);
			for (int i = 0; i < started; i++) {
				SceUInt timeout = 5000000;
				Worker *w = &workers[i];
				int result = sceKernelWaitSema(w->done, 1, &timeout);
				check("Worker finished", result);
				if (result >= 0) {
					logLine("Worker %d: ret=%08X sw=%u cyc=%u inst=%u", i,
						(unsigned)w->error, (unsigned)w->count,
						(unsigned)w->cycles, (unsigned)w->instructions);
					if (w->error || w->count != 64 || !w->cycles ||
					    !w->instructions)
						failed = 1;
					SceUInt32 count = 0;
					int ret =
					    scePerfArmPmonGetCounterValue(w->thread, 0, &count);
					logLine("Cross-thread %d: ret=%08X count=%u", i,
						(unsigned)ret, (unsigned)count);
					if (ret || count != 64)
						failed = 1;
				}
			}
			SceUInt32 count = 0;
			int ret = 0;
			for (int i = 0; i < 64 && !ret; i++)
				ret = scePerfArmPmonSoftwareIncrement(1);
			if (!ret)
				ret = scePerfArmPmonGetCounterValue(0, 0, &count);
			logLine("Main: ret=%08X software=%u", (unsigned)ret, (unsigned)count);
			if (ret || count != 64)
				failed = 1;
		}
		if (check("ALL stop", scePerfArmPmonStop(-1)) && !failed) {
			SceUInt32 before = 0, after = 0;
			int ret = scePerfArmPmonGetCounterValue(0, 0, &before);
			if (!ret)
				ret = scePerfArmPmonSoftwareIncrement(1);
			if (!ret)
				ret = scePerfArmPmonGetCounterValue(0, 0, &after);
			logLine("Stopped counter: ret=%08X before=%u after=%u", (unsigned)ret,
				(unsigned)before, (unsigned)after);
			if (ret || before != after)
				failed = 1;
			if (check("ALL set counter 77", scePerfArmPmonSetCounterValue(-1, 0, 77))) {
				for (int i = 0; i < started; i++) {
					SceUInt32 count = 0;
					ret = scePerfArmPmonGetCounterValue(workers[i].thread, 0,
									    &count);
					logLine("ALL set read %d: ret=%08X count=%u", i,
						(unsigned)ret, (unsigned)count);
					if (ret || count != 77)
						failed = 1;
				}
			}
			ret = scePerfArmPmonSetCounterValue(workers[0].thread, 0, 123);
			if (!ret)
				ret = scePerfArmPmonGetCounterValue(workers[0].thread, 0, &after);
			logLine("Specific-thread set: ret=%08X count=%u", (unsigned)ret,
				(unsigned)after);
			if (ret || after != 123)
				failed = 1;
			ret = scePerfArmPmonReset(0);
			if (!ret)
				ret = scePerfArmPmonGetCounterValue(0, 0, &after);
			logLine("Self reset while stopped: ret=%08X count=%u", (unsigned)ret,
				(unsigned)after);
			if (ret || after != 0)
				failed = 1;
		}
	}
	if (module >= 0 && !failed)
		testNonRazor();
	if (module >= 0 && !failed)
		failed |= testEventCoverage(logLine);
	logLine("%s", failed ? "FAIL: send results.txt for diagnosis"
			     : "PASS: complete non-Razor libperf tests");
	logLine("Circle: exit. Restart app for another run.");
	if (report)
		fclose(report);
	for (;;) {
		vita2d_start_drawing();
		vita2d_clear_screen();
		int first = lineCount > 24 ? lineCount - 24 : 0;
		if (font)
			for (int i = first; i < lineCount; i++)
				vita2d_pgf_draw_text(font, 20, 25 + (i - first) * 21,
						     RGBA8(230, 230, 230, 255), 0.75f,
						     lines[i % 48]);
		vita2d_end_drawing();
		vita2d_swap_buffers();
		SceCtrlData pad;
		memset(&pad, 0, sizeof(pad));
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_CIRCLE)
			break;
		sceKernelDelayThread(16666);
	}
	/* Process exit also cleans up any worker waiting after a failed step. */
	sceKernelExitProcess(0);
	return 0;
}

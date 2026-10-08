#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <vita2d.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include "libperf.h"
#include "queue.h"
#include "pipeline.h"

#define CORES 3
#define JOBS 192U
#define PRIVATE_JOBS 24U
#define SHORT_THREADS 12
#define TIME_MASK 0xFFFFFFFFFFFFULL

int sceKernelPerfArmPmonOpen(void);
int sceKernelPerfArmPmonClose(void);
static atomic_uint apiCalls[9];

static int perfReset(int t)
{
	atomic_fetch_add(&apiCalls[0], 1);
	return scePerfArmPmonReset(t);
}

static int perfSelectEvent(int t, unsigned c, unsigned e)
{
	atomic_fetch_add(&apiCalls[1], 1);
	return scePerfArmPmonSelectEvent(t, c, e);
}

static int perfStart(int t)
{
	atomic_fetch_add(&apiCalls[2], 1);
	return scePerfArmPmonStart(t);
}

static int perfStop(int t)
{
	atomic_fetch_add(&apiCalls[3], 1);
	return scePerfArmPmonStop(t);
}

static int perfGetCounter(int t, unsigned c, unsigned *v)
{
	atomic_fetch_add(&apiCalls[4], 1);
	return scePerfArmPmonGetCounterValue(t, c, v);
}

static int perfSetCounter(int t, unsigned c, unsigned v)
{
	atomic_fetch_add(&apiCalls[5], 1);
	return scePerfArmPmonSetCounterValue(t, c, v);
}

static int perfSoftwareIncrement(unsigned m)
{
	atomic_fetch_add(&apiCalls[6], 1);
	return scePerfArmPmonSoftwareIncrement(m);
}

static uint64_t perfTimebase(void)
{
	atomic_fetch_add(&apiCalls[7], 1);
	return scePerfGetTimebaseValue();
}

static unsigned perfFrequency(void)
{
	atomic_fetch_add(&apiCalls[8], 1);
	return scePerfGetTimebaseFrequency();
}

enum {
	MODE_PROBE = 1,
	MODE_WRAP,
	MODE_WORK,
	MODE_EXIT
};

typedef struct {
	SceUID thread, go, done;
	unsigned core, jobs;
	atomic_uint mode, finished;
	int error;
	unsigned counters[7];
	uint64_t elapsed, stageCycles[4];
	Pipeline pipeline;
} Worker;

static Worker workers[CORES];
static Queue incoming, outgoing;
static atomic_uint submittedAll, stopWork;
static atomic_int churnError;
static atomic_uint churnCreated;
static SceUID churnThread, churnGo, churnDone;
static FILE *report;
static vita2d_pgf *font;
static char lines[18][160];
static unsigned lineCount, epoch, passes, liveReads, broadcasts, vanished;
static int failed, cancelled;

static void logLine(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	vsnprintf(lines[lineCount % 18], sizeof(lines[0]), fmt, a);
	va_end(a);
	if (report) {
		fprintf(report, "%s\n", lines[lineCount % 18]);
		fflush(report);
	}
	lineCount++;
}

static int check(const char *stage, int r)
{
	if (r < 0) {
		logLine("FAIL epoch=%u stage=%s error=%08X", epoch, stage, (unsigned)r);
		failed = 1;
		return 0;
	}
	return 1;
}

static int expect(const char *stage, int actual, int expected)
{
	if (actual != expected) {
		logLine("FAIL epoch=%u stage=%s got=%08X expected=%08X", epoch, stage,
			(unsigned)actual, (unsigned)expected);
		failed = 1;
		return 0;
	}
	return 1;
}

static void draw(void)
{
	vita2d_start_drawing();
	vita2d_clear_screen();
	if (font) {
		vita2d_pgf_draw_text(font, 18, 28, RGBA8(90, 195, 255, 255), 0.85f,
				     "THREE-CORE LIBPERF STRESS LAB");
		unsigned first = lineCount > 18 ? lineCount - 18 : 0;
		for (unsigned i = first; i < lineCount; i++)
			vita2d_pgf_draw_text(font, 18, 56 + (i - first) * 23,
					     RGBA8(230, 235, 242, 255), 0.75f, lines[i % 18]);
		vita2d_pgf_draw_text(
		    font, 18, 520, RGBA8(160, 170, 190, 255), 0.75f,
		    "Cross: 32 epochs  Triangle: 256  Square: continuous  Circle: stop/exit");
	}
	vita2d_end_drawing();
	vita2d_swap_buffers();
	vita2d_wait_rendering_done();
}

static void workerError(Worker *w, int r)
{
	if (r < 0 && !w->error)
		w->error = r;
}

static void snapshot(Worker *w)
{
	workerError(w, perfStop(0));
	for (unsigned c = 0; c < 7; c++)
		workerError(w, perfGetCounter(0, c == 6 ? 31 : c, &w->counters[c]));
}

static int stageBegin(Worker *w, unsigned *cycles)
{
	int r = perfGetCounter(0, 31, cycles);
	workerError(w, r);
	return r;
}

static void stageEnd(Worker *w, unsigned stage, unsigned before)
{
	unsigned after = 0;
	workerError(w, perfGetCounter(0, 31, &after));
	w->stageCycles[stage] += (uint32_t)(after - before);
	workerError(w, perfSoftwareIncrement(1));
}

static void processJob(Worker *w, Message m)
{
	unsigned before = 0;
	stageBegin(w, &before);
	generateTile(&w->pipeline, m.seed);
	stageEnd(w, 0, before);
	stageBegin(w, &before);
	transformTile(&w->pipeline);
	stageEnd(w, 1, before);
	stageBegin(w, &before);
	compressTile(&w->pipeline);
	stageEnd(w, 2, before);
	stageBegin(w, &before);
	m.checksum = validateTile(&w->pipeline);
	stageEnd(w, 3, before);
	m.core = w->core;
	w->jobs++;
	while (!queuePush(&outgoing, m)) {
		if (atomic_load_explicit(&stopWork, memory_order_acquire))
			break;
		sceKernelDelayThread(50);
	}
}

static int workerMain(SceSize size, void *arg)
{
	(void)size;
	Worker *w = *(Worker **)arg;
	atomic_store_explicit(&w->finished, 1, memory_order_release);
	sceKernelSignalSema(w->done, 1);
	for (;;) {
		int r = sceKernelWaitSema(w->go, 1, NULL);
		if (r < 0)
			return r;
		unsigned mode = atomic_load_explicit(&w->mode, memory_order_acquire);
		if (mode == MODE_EXIT)
			break;
		w->error = 0;
		w->jobs = 0;
		memset(w->stageCycles, 0, sizeof(w->stageCycles));
		uint64_t begin = perfTimebase();
		if (mode == MODE_PROBE) {
			for (unsigned i = 0; i < 64 + w->core * 17 && !w->error; i++)
				workerError(w, perfSoftwareIncrement(1));
			generateTile(&w->pipeline, 123 + w->core);
			transformTile(&w->pipeline);
		} else if (mode == MODE_WRAP) {
			for (unsigned i = 0; i < 32 && !w->error; i++) {
				if (!(i & 3)) {
					workerError(w, sceKernelChangeThreadCpuAffinityMask(
							   0, SCE_KERNEL_CPU_MASK_USER_0
								  << ((w->core + i / 4) % 3)));
					sceKernelDelayThread(100);
				}
				workerError(w, perfSoftwareIncrement(0x3F));
			}
			workerError(w, sceKernelChangeThreadCpuAffinityMask(
					   0, SCE_KERNEL_CPU_MASK_USER_0 << w->core));
		} else if (mode == MODE_WORK) {
			/* Guaranteed work precedes competition for the shared queue. */
			for (unsigned i = 0; i < PRIVATE_JOBS && !w->error &&
					     !atomic_load_explicit(&stopWork, memory_order_acquire);
			     i++) {
				unsigned id = w->core * PRIVATE_JOBS + i;
				processJob(w, (Message){id, 0x12340000U + epoch * JOBS + id, 0, 0});
			}
			while (!atomic_load_explicit(&stopWork, memory_order_acquire)) {
				Message m;
				if (!queuePop(&incoming, &m)) {
					if (atomic_load_explicit(&submittedAll,
								 memory_order_acquire))
						break;
					sceKernelDelayThread(50);
					continue;
				}
				processJob(w, m);
				if (w->error)
					break;
			}
		}
		snapshot(w);
		w->elapsed = (perfTimebase() - begin) & TIME_MASK;
		atomic_store_explicit(&w->finished, 1, memory_order_release);
		sceKernelSignalSema(w->done, 1);
	}
	return 0;
}

static int waitWorker(Worker *w)
{
	SceUInt timeout = 10000000;
	if (!check("worker completion timeout", sceKernelWaitSema(w->done, 1, &timeout)))
		return 0;
	if (!atomic_load_explicit(&w->finished, memory_order_acquire)) {
		failed = 1;
		return 0;
	}
	return check("worker PMU operations", w->error);
}

static int createWorker(unsigned core)
{
	Worker *w = &workers[core];
	w->core = core;
	w->pipeline.arena = calloc(ARENA_WORDS, sizeof(uint32_t));
	if (!w->pipeline.arena) {
		failed = 1;
		return 0;
	}
	w->go = sceKernelCreateSema("stress_go", 0, 0, 1, NULL);
	w->done = sceKernelCreateSema("stress_done", 0, 0, 1, NULL);
	w->thread = sceKernelCreateThread("stress_worker", workerMain, 0xA0, 0x10000, 0,
					  SCE_KERNEL_CPU_MASK_USER_0 << core, NULL);
	if (!check("worker go", w->go) || !check("worker done", w->done) ||
	    !check("worker create", w->thread))
		return 0;
	if (!check("worker start", sceKernelStartThread(w->thread, sizeof(w), &w)))
		return 0;
	return waitWorker(w);
}

static int dispatch(unsigned mode)
{
	for (unsigned c = 0; c < CORES; c++) {
		atomic_store_explicit(&workers[c].finished, 0, memory_order_relaxed);
		atomic_store_explicit(&workers[c].mode, mode, memory_order_release);
		if (!check("dispatch", sceKernelSignalSema(workers[c].go, 1)))
			return 0;
	}
	return 1;
}

static int waitAll(void)
{
	int good = 1;
	for (unsigned c = 0; c < CORES; c++)
		if (!waitWorker(&workers[c]))
			good = 0;
	return good;
}

static int crossRead(void)
{
	for (unsigned c = 0; c < CORES; c++)
		for (unsigned n = 0; n < 7; n++) {
			unsigned value = 0;
			if (!check("parked cross-thread read",
				   perfGetCounter(workers[c].thread, n == 6 ? 31 : n, &value)))
				return 0;
			if (value != workers[c].counters[n]) {
				logLine("FAIL cross core=%u counter=%u got=%u saved=%u", c, n,
					value, workers[c].counters[n]);
				failed = 1;
				return 0;
			}
		}
	return 1;
}

static int transientMain(SceSize size, void *data)
{
	(void)size;
	(void)data;
	unsigned value = 0;
	int r = perfSelectEvent(0, 0, 0);
	if (!r)
		r = perfReset(0);
	if (!r)
		r = perfStart(0);
	if (!r)
		r = perfSoftwareIncrement(1);
	if (!r)
		r = perfStop(0);
	if (!r)
		r = perfGetCounter(0, 0, &value);
	if (!r && value != 1)
		r = -1;
	if (r < 0)
		atomic_store(&churnError, r);
	sceKernelDelayThread(100);
	return r;
}

static int churnMain(SceSize size, void *data)
{
	(void)size;
	(void)data;
	for (;;) {
		int r = sceKernelWaitSema(churnGo, 1, NULL);
		if (r < 0)
			return r;
		for (unsigned i = 0; i < SHORT_THREADS; i++) {
			SceUID t =
			    sceKernelCreateThread("stress_transient", transientMain, 0xA0, 0x10000,
						  0, SCE_KERNEL_CPU_MASK_USER_0 << (i % 3), NULL);
			if (t < 0) {
				atomic_store(&churnError, t);
				break;
			}
			r = sceKernelStartThread(t, 0, NULL);
			if (r >= 0) {
				SceUInt timeout = 3000000;
				r = sceKernelWaitThreadEnd(t, NULL, &timeout);
			}
			if (r < 0) {
				atomic_store(&churnError, r);
				break;
			}
			r = sceKernelDeleteThread(t);
			if (r < 0) {
				atomic_store(&churnError, r);
				break;
			}
			atomic_fetch_add(&churnCreated, 1);
		}
		sceKernelSignalSema(churnDone, 1);
	}
}

static int configure(int all, int softwareOnly)
{
	static const unsigned banks[3][5] = {{0x68, 0x03, 0x05, 0x10, 0x06},
					     {0x73, 0x74, 0x61, 0x07, 0x04},
					     {0x63, 0x64, 0x65, 0x70, 0x72}};
	for (unsigned c = 0; c < 6; c++)
		if (!check("ALL select", perfSelectEvent(all, c,
							 softwareOnly ? 0
							 : c	      ? banks[epoch % 3][c - 1]
								      : 0)))
			return 0;
	return check("ALL reset", perfReset(all));
}

static int invalidInputs(void)
{
	unsigned v = 0;
	return expect("invalid event", perfSelectEvent(0, 0, 0xFF),
		      SCE_PERF_ERROR_INVALID_ARGUMENT) &&
	       expect("invalid event counter", perfSelectEvent(0, 6, 0),
		      SCE_PERF_ERROR_INVALID_ARGUMENT) &&
	       expect("invalid read counter", perfGetCounter(0, 32, &v),
		      SCE_PERF_ERROR_INVALID_ARGUMENT) &&
	       expect("null read", perfGetCounter(0, 0, NULL), SCE_PERF_ERROR_INVALID_ARGUMENT) &&
	       expect("ALL read", perfGetCounter(SCE_PERF_ARM_PMON_THREAD_ID_ALL, 0, &v),
		      SCE_PERF_ERROR_INVALID_ARGUMENT) &&
	       expect("invalid write counter", perfSetCounter(0, 6, 1),
		      SCE_PERF_ERROR_INVALID_ARGUMENT) &&
	       expect("invalid software mask", perfSoftwareIncrement(0x40),
		      SCE_PERF_ERROR_INVALID_ARGUMENT) &&
	       expect("zero software mask", perfSoftwareIncrement(0), 0);
}

static int controlPhase(int all)
{
	if (!configure(all, 0))
		return 0;
	unsigned initial = 0xABC00000U + epoch * 256;
	if (!check("ALL write software", perfSetCounter(all, 0, initial)) ||
	    !check("ALL start probe", perfStart(all)) || !dispatch(MODE_PROBE) || !waitAll())
		return 0;
	for (unsigned c = 0; c < CORES; c++) {
		if (workers[c].counters[0] != initial + 64 + c * 17 || !workers[c].counters[6] ||
		    !workers[c].elapsed) {
			logLine("FAIL known software count core=%u value=%u", c,
				workers[c].counters[0]);
			failed = 1;
			return 0;
		}
	}
	if (!crossRead() || !check("ALL stop probe", perfStop(all)))
		return 0;
	for (unsigned c = 0; c < CORES; c++) {
		if (!check("specific write", perfSetCounter(workers[c].thread, 0, 123 + c)))
			return 0;
		unsigned value = 0;
		if (!check("specific read", perfGetCounter(workers[c].thread, 0, &value)) ||
		    !expect("specific value", value, 123 + c))
			return 0;
	}
	if (!configure(all, 1))
		return 0;
	for (unsigned c = 0; c < 6; c++)
		if (!check("ALL near-wrap writes", perfSetCounter(all, c, 0xFFFFFFF0)))
			return 0;
	if (!check("ALL near-wrap cycles", perfSetCounter(all, 31, 0xFFFFFFF0)) ||
	    !check("ALL start wrap", perfStart(all)) || !dispatch(MODE_WRAP) || !waitAll())
		return 0;
	for (unsigned c = 0; c < CORES; c++) {
		for (unsigned n = 0; n < 6; n++)
			if (!expect("wrap plus migration", workers[c].counters[n], 16))
				return 0;
		if (!workers[c].counters[6] || workers[c].counters[6] >= 0xFFFFFFF0) {
			logLine("FAIL cycle wrap core=%u", c);
			failed = 1;
			return 0;
		}
	}
	if (!crossRead() || !check("ALL stop wrap", perfStop(all)) ||
	    !check("ALL final reset", perfReset(all)))
		return 0;
	for (unsigned c = 0; c < CORES; c++)
		for (unsigned n = 0; n < 7; n++) {
			unsigned v = 0;
			if (!check("reset verify read",
				   perfGetCounter(workers[c].thread, n == 6 ? 31 : n, &v)) ||
			    !expect("reset verify zero", v, 0))
				return 0;
		}
	return invalidInputs();
}

static int lifecyclePhase(void)
{
	logLine("epoch=%u lifecycle: workers parked, closing PMU", epoch);
	if (!check("close", sceKernelPerfArmPmonClose()))
		return 0;
	unsigned v = 0;
	if (!expect("closed reset", perfReset(0), SCE_PERF_ERROR_NOT_INITIALIZED) ||
	    !expect("closed start", perfStart(0), SCE_PERF_ERROR_NOT_INITIALIZED) ||
	    !expect("closed stop", perfStop(0), SCE_PERF_ERROR_NOT_INITIALIZED) ||
	    !expect("closed get", perfGetCounter(0, 0, &v), SCE_PERF_ERROR_NOT_INITIALIZED) ||
	    !expect("closed set", perfSetCounter(0, 0, 0), SCE_PERF_ERROR_NOT_INITIALIZED) ||
	    !expect("closed software", perfSoftwareIncrement(1), SCE_PERF_ERROR_NOT_INITIALIZED))
		return 0;
	uint64_t t0 = perfTimebase();
	sceKernelDelayThread(1000);
	uint64_t t1 = perfTimebase();
	if (!expect("closed timebase frequency", perfFrequency(), 1) || t0 == t1) {
		failed = 1;
		return 0;
	}
	/* Native select is a kernel operation with no disabled guard. */
	if (!check("closed select", perfSelectEvent(0, 0, 0)))
		return 0;
	if (!check("reopen", sceKernelPerfArmPmonOpen()))
		return 0;
	if (!check("reopen reset", perfReset(0)) || !check("reopen start", perfStart(0)) ||
	    !check("reopen software", perfSoftwareIncrement(1)) ||
	    !check("reopen stop", perfStop(0)) || !check("reopen get", perfGetCounter(0, 0, &v)))
		return 0;
	return expect("reopen software value", v, 1);
}

static int workloadPhase(int all)
{
	queueInit(&incoming);
	queueInit(&outgoing);
	atomic_store(&submittedAll, 0);
	atomic_store(&stopWork, 0);
	atomic_store(&churnError, 0);
	unsigned received = 0, sent = CORES * PRIVATE_JOBS, perCore[3] = {0};
	unsigned char seen[JOBS] = {0};
	if (!configure(all, 0) || !check("ALL start jobs", perfStart(all)) ||
	    !dispatch(MODE_WORK) || !check("start thread churn", sceKernelSignalSema(churnGo, 1)))
		return 0;
	uint64_t begin = perfTimebase(), lastLive = begin;
	while (received < JOBS) {
		if (sent < JOBS) {
			Message m = {sent, 0x12340000U + epoch * JOBS + sent, 0, 0};
			if (queuePush(&incoming, m)) {
				sent++;
				if (sent == JOBS)
					atomic_store_explicit(&submittedAll, 1,
							      memory_order_release);
			}
		}
		Message done;
		while (queuePop(&outgoing, &done)) {
			if (done.id >= JOBS || seen[done.id] || done.core >= CORES ||
			    done.seed != 0x12340000U + epoch * JOBS + done.id ||
			    done.checksum != referenceChecksum(done.seed)) {
				logLine("FAIL queue/pipeline integrity epoch=%u id=%u core=%u",
					epoch, done.id, done.core);
				failed = 1;
				break;
			}
			seen[done.id] = 1;
			perCore[done.core]++;
			received++;
		}
		if (failed)
			break;
		uint64_t now = perfTimebase();
		if (((now - lastLive) & TIME_MASK) > 500) {
			unsigned value = 0;
			for (unsigned c = 0; c < CORES; c++)
				if (!check("live cross-thread cycles",
					   perfGetCounter(workers[c].thread, 31, &value)))
					break;
				else
					liveReads++;
			int r = perfSelectEvent(all, 5, epoch % 2 ? 0x07 : 0x06);
			broadcasts++;
			if (r == (int)0x80028021 || r == SCE_PERF_ERROR_INVALID_ARGUMENT)
				vanished++;
			else if (!check("live ALL event change", r))
				break;
			lastLive = now;
			SceCtrlData pad = {0};
			sceCtrlPeekBufferPositive(0, &pad, 1);
			if (pad.buttons & SCE_CTRL_CIRCLE)
				cancelled = 1;
		}
		if (((now - begin) & TIME_MASK) > 10000000) {
			logLine("FAIL work watchdog sent=%u received=%u", sent, received);
			failed = 1;
			break;
		}
		sceKernelDelayThread(50);
	}
	if (failed)
		atomic_store_explicit(&stopWork, 1, memory_order_release);
	int good = waitAll();
	SceUInt timeout = 10000000;
	if (!check("churn completion timeout", sceKernelWaitSema(churnDone, 1, &timeout)) ||
	    !check("transient PMU result", atomic_load(&churnError)))
		good = 0;
	if (failed || !good)
		return 0;
	if (!check("ALL stop jobs", perfStop(all)) || !crossRead())
		return 0;
	unsigned sum = 0;
	for (unsigned c = 0; c < CORES; c++) {
		Worker *w = &workers[c];
		sum += w->jobs;
		if (w->jobs < PRIVATE_JOBS || w->jobs != perCore[c] ||
		    w->counters[0] != 4 * w->jobs) {
			logLine("FAIL worker accounting core=%u jobs=%u completed=%u software=%u",
				c, w->jobs, perCore[c], w->counters[0]);
			failed = 1;
			return 0;
		}
		logLine("worker,%u,%u,jobs=%u,sw=%u,cycles=%u,us=%llu", epoch, c, w->jobs,
			w->counters[0], w->counters[6], (unsigned long long)w->elapsed);
		logLine(
		    "stages,%u,%u,generate=%llu,neon=%llu,rle=%llu,validate=%llu", epoch, c,
		    (unsigned long long)w->stageCycles[0], (unsigned long long)w->stageCycles[1],
		    (unsigned long long)w->stageCycles[2], (unsigned long long)w->stageCycles[3]);
		logLine("events,%u,%u,c1=%u,c2=%u,c3=%u,c4=%u,c5=%u", epoch, c, w->counters[1],
			w->counters[2], w->counters[3], w->counters[4], w->counters[5]);
	}
	if (sum != JOBS) {
		failed = 1;
		return 0;
	}
	logLine("epoch=%u PASS jobs=%u churn=%u live_reads=%u broadcasts=%u vanished=%u", epoch,
		received, atomic_load(&churnCreated), liveReads, broadcasts, vanished);
	return 1;
}

static void runStress(unsigned limit)
{
	if (!report)
		report = fopen("ux0:data/libperf-stress/results.txt", "a");
	if (!report) {
		logLine("FAIL opening report");
		failed = 1;
		return;
	}
	passes = 0;
	cancelled = 0;
	failed = 0;
	liveReads = 0;
	broadcasts = 0;
	vanished = 0;
	atomic_store(&churnCreated, 0);
	for (unsigned i = 0; i < 9; i++)
		atomic_store(&apiCalls[i], 0);
	logLine("START epochs=%u cores=3 jobs/epoch=%u sizeofQueue=%u", limit, JOBS,
		(unsigned)sizeof(Queue));
	for (epoch = 0; epoch < limit && !failed && !cancelled; epoch++) {
		int all = epoch & 1 ? SCE_PERF_ARM_PMON_THREAD_ID_ALL : -1;
		logLine("epoch=%u BEGIN probe/wrap/migration/control", epoch);
		draw();
		if (!(epoch % 4) && !lifecyclePhase())
			break;
		if (!controlPhase(all) || !workloadPhase(all))
			break;
		passes++;
		draw();
		SceCtrlData pad = {0};
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_CIRCLE)
			cancelled = 1;
	}
	if (!failed)
		check("final ALL stop", perfStop(SCE_PERF_ARM_PMON_THREAD_ID_ALL));
	logLine("RESULT %s completed_epochs=%u jobs=%u transient_threads=%u",
		failed	    ? "FAIL"
		: cancelled ? "CANCELLED"
			    : "PASS",
		passes, passes * JOBS, atomic_load(&churnCreated));
	static const char *names[9] = {"reset", "select",   "start", "stop",	 "get",
				       "set",	"software", "time",  "frequency"};
	for (unsigned i = 0; i < 9; i++)
		logLine("API %s calls=%u", names[i], atomic_load(&apiCalls[i]));
	logLine("Report: ux0:data/libperf-stress/results.txt");
	fclose(report);
	report = NULL;
}

int main(void)
{
	vita2d_init();
	font = vita2d_load_default_pgf();
	mkdir("ux0:data/libperf-stress", 0777);
	report = fopen("ux0:data/libperf-stress/results.txt", "w");
	logLine("Three-core libperf stress lab - build 3");
	draw();
	sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
	sceKernelChangeThreadPriority(0, 0x100);
	if (!report || !createWorker(0) || !createWorker(1)) {
		failed = 1;
		goto display;
	}
	int status = 0;
	SceUID module = sceKernelLoadStartModule("app0:libperf.suprx", 0, NULL, 0, NULL, &status);
	if (!check("load custom libperf", module) ||
	    !expect("custom module status", status, SCE_KERNEL_START_SUCCESS) || !createWorker(2))
		goto display;
	churnGo = sceKernelCreateSema("churn_go", 0, 0, 1, NULL);
	churnDone = sceKernelCreateSema("churn_done", 0, 0, 1, NULL);
	churnThread = sceKernelCreateThread("stress_churn", churnMain, 0xA0, 0x10000, 0,
					    SCE_KERNEL_CPU_MASK_USER_1, NULL);
	if (!check("churn go", churnGo) || !check("churn done", churnDone) ||
	    !check("churn thread", churnThread) ||
	    !check("churn start", sceKernelStartThread(churnThread, 0, NULL)))
		goto display;
	if (!expect("timebase frequency", perfFrequency(), 1))
		goto display;
	runStress(32);
display:
	if (report) {
		fclose(report);
		report = NULL;
	};
	unsigned previous = 0;
	for (;;) {
		draw();
		SceCtrlData pad = {0};
		sceCtrlPeekBufferPositive(0, &pad, 1);
		unsigned pressed = pad.buttons & ~previous;
		previous = pad.buttons;
		if (pressed & SCE_CTRL_CIRCLE)
			break;
		if (!failed && (pressed & (SCE_CTRL_CROSS | SCE_CTRL_TRIANGLE | SCE_CTRL_SQUARE)))
			runStress(pressed & SCE_CTRL_SQUARE	? UINT32_MAX
				  : pressed & SCE_CTRL_TRIANGLE ? 256
								: 32);
		sceKernelDelayThread(16666);
	}
	if (report)
		fclose(report);
	sceKernelExitProcess(failed ? 1 : 0);
	return 0;
}

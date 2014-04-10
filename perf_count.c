#include "perf_count.h"

#include <linux/perf_event.h>
#include <sys/types.h>
#include <sys/syscall.h>

#include <unistd.h>
#include <assert.h>
#include <stdlib.h>

struct perf_count_ctx
{
	int num_groups;
	int num_events;
	struct perf_event_attr *events;
	int *fds;
	uint64_t *counters;
};

static const struct perf_event_attr perf_count_mapping[] =
{
	{ .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_CPU_CYCLES },
	{ .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_INSTRUCTIONS },
	{ .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_CACHE_REFERENCES },
	{ .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_CACHE_MISSES },
	{ .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS },
	{ .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_BRANCH_MISSES },
	{ .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_BUS_CYCLES },
	{ .type = PERF_TYPE_HW_CACHE, .config = PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16) },
	{ .type = PERF_TYPE_HW_CACHE, .config = PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) },
	{ .type = PERF_TYPE_HW_CACHE, .config = PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_PREFETCH << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16) },
	{ .type = PERF_TYPE_HW_CACHE, .config = PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_PREFETCH << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) },
	{ .type = PERF_TYPE_HW_CACHE, .config = PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16) },
	{ .type = PERF_TYPE_HW_CACHE, .config = PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) },
	{ .type = PERF_TYPE_HW_CACHE, .config = PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_PREFETCH << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16) },
	{ .type = PERF_TYPE_HW_CACHE, .config = PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_PREFETCH << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) },
	{ .type = PERF_TYPE_HW_CACHE, .config = PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16) },
	{ .type = PERF_TYPE_HW_CACHE, .config = PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) },
	{ .type = PERF_TYPE_HW_CACHE, .config = PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_PREFETCH << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16) },
	{ .type = PERF_TYPE_HW_CACHE, .config = PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_PREFETCH << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) },
	{ .type = PERF_TYPE_SOFTWARE, .config = PERF_COUNT_SW_CPU_CLOCK },
	{ .type = PERF_TYPE_SOFTWARE, .config = PERF_COUNT_SW_TASK_CLOCK },
	{ .type = PERF_TYPE_SOFTWARE, .config = PERF_COUNT_SW_PAGE_FAULTS },
	{ .type = PERF_TYPE_SOFTWARE, .config = PERF_COUNT_SW_CONTEXT_SWITCHES },
	{ .type = PERF_TYPE_SOFTWARE, .config = PERF_COUNT_SW_CPU_MIGRATIONS },
	{ .type = PERF_TYPE_SOFTWARE, .config = PERF_COUNT_SW_PAGE_FAULTS_MIN },
	{ .type = PERF_TYPE_SOFTWARE, .config = PERF_COUNT_SW_PAGE_FAULTS_MAJ },
	{ .type = PERF_TYPE_SOFTWARE, .config = PERF_COUNT_SW_ALIGNMENT_FAULTS },
	{ .type = PERF_TYPE_SOFTWARE, .config = PERF_COUNT_SW_EMULATION_FAULTS },
};

static int
sys_perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu, int group_fd, unsigned long flags)
{
	attr->size = sizeof(*attr);
	return (int)syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

perf_count_t
perf_count_init(const enum PERF_COUNT_TYPE *perf_count_types, int num_events, int system_wide)
{
	if (perf_count_types == NULL)
		return NULL;

	struct perf_count_ctx *ctx = (struct perf_count_ctx *)malloc(sizeof(struct perf_count_ctx));
	assert(ctx);

	if (system_wide)
		ctx->num_groups = (int)sysconf(_SC_NPROCESSORS_ONLN);
	else
		ctx->num_groups = 1;
	ctx->num_events = num_events;

	ctx->events = (struct perf_event_attr *)calloc(sizeof(struct perf_event_attr), (size_t)ctx->num_events);
	assert(ctx->events);
	ctx->fds = (int *)calloc(sizeof(int), (size_t)ctx->num_groups * (size_t)ctx->num_events);
	assert(ctx->fds);
	ctx->counters = (uint64_t *)calloc(sizeof(uint64_t), (size_t)ctx->num_events);
	assert(ctx->counters);

	for (int event = 0; event < ctx->num_events; event++)
	{
		assert(perf_count_types[event] < sizeof(perf_count_mapping) / sizeof(perf_count_mapping[0]));

		ctx->events[event] = perf_count_mapping[perf_count_types[event]];
		ctx->events[event].read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
	}

	for (int group = 0; group < ctx->num_groups; group++)
		for (int event = 0; event < ctx->num_events; event++)
		{
			pid_t pid;
			int cpu;

			if (system_wide)
			{
				pid = -1;
				// XXX: assuming the IDs of online cpus range from 0 to (num_cpus - 1)
				cpu = group;
			}
			else
			{
				// this process
				pid = 0;
				cpu = -1;
			}

			ctx->fds[group * ctx->num_events + event] = sys_perf_event_open(&ctx->events[event], pid, cpu, -1, 0);
			assert(ctx->fds[group * ctx->num_events + event] >= 0);
		}

	return ctx;
}

void
perf_count_free(perf_count_t perf_count)
{
	struct perf_count_ctx *ctx = (struct perf_count_ctx *)perf_count;

	for (int group = 0; group < ctx->num_groups; group++)
		for (int event = 0; event < ctx->num_events; event++)
			close(ctx->fds[group * ctx->num_events + event]);

	free(ctx->counters);
	free(ctx->fds);
	free(ctx->events);
	free(ctx);
}

static void
perf_count_accumulate(perf_count_t perf_count, int additive)
{
	struct perf_count_ctx *ctx = (struct perf_count_ctx *)perf_count;

	for (int event = 0; event < ctx->num_events; event++)
	{
		uint64_t count[3];
		uint64_t accum_count[3] = {0, 0, 0};

		for (int group = 0; group < ctx->num_groups; group++)
		{
			count[0] = count[1] = count[2] = 0;
			ssize_t len = read(ctx->fds[group * ctx->num_events + event], count, sizeof(count));
			//printf("%d %ld %ld %ld\n", len, count[0], count[1], count[2]);
			assert((size_t)len == sizeof(count));

			accum_count[0] += count[0];
			accum_count[1] += count[1];
			accum_count[2] += count[2];
		}

		if (accum_count[2] == 0)
		{
			// no event occurred at all
		}
		else
		{
			if (accum_count[2] < accum_count[1])
			{
				// need to scale
				accum_count[0] = (uint64_t)((double)accum_count[0] * (double)accum_count[1] / (double)accum_count[2] + 0.5);
			}
		}

			if (additive)
			{
				ctx->counters[event] += accum_count[0];
				// due to the scaling, we may observe a negative increment
				if ((int64_t)ctx->counters[event] < 0)
					ctx->counters[event] = 0;
			}
			else
				ctx->counters[event] -= accum_count[0];
	}
}

void
perf_count_start(perf_count_t perf_count)
{
	perf_count_accumulate(perf_count, 0);
}

void
perf_count_stop(perf_count_t perf_count)
{
	perf_count_accumulate(perf_count, 1);
}

void
perf_count_reset(perf_count_t perf_count)
{
	struct perf_count_ctx *ctx = (struct perf_count_ctx *)perf_count;

	for (int event = 0; event < ctx->num_events; event++)
		ctx->counters[event] = 0;
}

uint64_t
perf_count_get_by_type(perf_count_t perf_count, enum PERF_COUNT_TYPE type)
{
	if (type >= sizeof(perf_count_mapping) / sizeof(perf_count_mapping[0]))
		return PERF_COUNT_INVALID;

	struct perf_count_ctx *ctx = (struct perf_count_ctx *)perf_count;

	for (int event = 0; event < ctx->num_events; event++)
	{
		if (ctx->events[event].type == perf_count_mapping[type].type &&
			ctx->events[event].config == perf_count_mapping[type].config)
			return ctx->counters[event];
	}

	return PERF_COUNT_INVALID;
}

uint64_t
perf_count_get_by_index(perf_count_t perf_count, int index)
{
	struct perf_count_ctx *ctx = (struct perf_count_ctx *)perf_count;

	if (index < 0 || index >= ctx->num_events)
		return PERF_COUNT_INVALID;

	return ctx->counters[index];
}


#define _POSIX_C_SOURCE 200809L

#include "cpu.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void cpu_monitor_destroy(cpu_monitor_t *monitor)
{
	if (!monitor)
		return;
	free(monitor->samples);
	memset(monitor, 0, sizeof *monitor);
}

static int cpu_read(cpu_sample_t *sample)
{
	FILE *file = fopen("/proc/stat", "r");
	char line[256];
	unsigned long long values[10];
	int count;

	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		count = sscanf(line,
			       "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
			       &values[0],
			       &values[1],
			       &values[2],
			       &values[3],
			       &values[4],
			       &values[5],
			       &values[6],
			       &values[7],
			       &values[8],
			       &values[9]);
		if (count >= 8) {
			sample->user = values[0];
			sample->nice = values[1];
			sample->system = values[2];
			sample->idle = values[3];
			sample->iowait = values[4];
			sample->irq = values[5];
			sample->softirq = values[6];
			sample->steal = values[7];
			fclose(file);
			return 0;
		}
	}
	fclose(file);
	return -1;
}

static int cpu_sample(cpu_sample_t *sample)
{
	if (clock_gettime(CLOCK_MONOTONIC, &sample->sampled_at))
		return -1;
	return cpu_read(sample);
}

static int cpu_wait(int64_t seconds)
{
	struct timespec duration = {
		.tv_sec = seconds,
		.tv_nsec = 0,
	};

	while (nanosleep(&duration, &duration)) {
		if (errno != EINTR)
			return -1;
	}
	return 0;
}

static double sample_elapsed(const cpu_sample_t *before,
			     const cpu_sample_t *after)
{
	return (double)(after->sampled_at.tv_sec - before->sampled_at.tv_sec)
	       + (double)(after->sampled_at.tv_nsec
			   - before->sampled_at.tv_nsec)
		 / 1000000000.0;
}

static int cpu_delta(const cpu_sample_t *before,
		     const cpu_sample_t *after,
		     uint64_t *busy,
		     uint64_t *total)
{
	uint64_t user, nice, system, idle, iowait, irq, softirq, steal;

	if (after->user < before->user || after->nice < before->nice
	    || after->system < before->system || after->idle < before->idle
	    || after->irq < before->irq || after->softirq < before->softirq
	    || after->steal < before->steal)
		return -1;
	/* Linux documents iowait as a counter that can decrease. */
	user = after->user - before->user;
	nice = after->nice - before->nice;
	system = after->system - before->system;
	idle = after->idle - before->idle;
	iowait = after->iowait >= before->iowait
		     ? after->iowait - before->iowait
		     : 0;
	irq = after->irq - before->irq;
	softirq = after->softirq - before->softirq;
	steal = after->steal - before->steal;
	*busy = user + nice + system + irq + softirq + steal;
	*total = *busy + idle + iowait;
	return *total ? 0 : -1;
}

static int monitor_append(cpu_monitor_t *monitor, const cpu_sample_t *sample)
{
	if (monitor->count == monitor->capacity) {
		size_t capacity = monitor->capacity ? monitor->capacity * 2 : 8;
		cpu_sample_t *samples = realloc(monitor->samples,
						capacity * sizeof *samples);

		if (!samples)
			return -1;
		monitor->samples = samples;
		monitor->capacity = capacity;
	}
	monitor->samples[monitor->count++] = *sample;
	return 0;
}

static void monitor_prune(cpu_monitor_t *monitor, int64_t window)
{
	while (monitor->count > 1
	       && sample_elapsed(&monitor->samples[1],
				 &monitor->samples[monitor->count - 1])
		      >= (double)window) {
		memmove(monitor->samples,
			monitor->samples + 1,
			(monitor->count - 1) * sizeof *monitor->samples);
		monitor->count--;
	}
}

static int cpu_result(const config_t *config,
		      const cpu_sample_t *before,
		      const cpu_sample_t *after,
		      char *reason,
		      size_t reason_size)
{
	uint64_t busy, total;
	double elapsed = sample_elapsed(before, after);
	double utilisation;

	if (elapsed <= 0.0 || cpu_delta(before, after, &busy, &total)) {
		snprintf(reason,
			 reason_size,
			 "CPU counters unavailable, reset, or measurement window invalid");
		return 0;
	}
	utilisation = cpu_utilisation(busy, total);
	if (utilisation > (double)config->cpu_threshold) {
		snprintf(reason,
			 reason_size,
			 "aggregate CPU utilisation %.1f%% over %.1f seconds exceeds "
			 "%.1f%%",
			 utilisation,
			 elapsed,
			 (double)config->cpu_threshold);
		return 0;
	}
	snprintf(reason,
		 reason_size,
		 "aggregate CPU utilisation %.1f%% over %.1f seconds",
		 utilisation,
		 elapsed);
	return 1;
}

int cpu_check(const config_t *config,
	      char *reason,
	      size_t reason_size,
	      cpu_monitor_t *monitor,
	      int wait_for_window)
{
	cpu_monitor_t one_shot = {0};
	cpu_sample_t sample;
	int continuous = monitor != NULL;

	if (!monitor)
		monitor = &one_shot;
	if (cpu_sample(&sample)) {
		snprintf(reason,
			 reason_size,
			 "cannot read CPU counters or monotonic clock");
		return 0;
	}
	if (!continuous) {
		cpu_sample_t before = sample;

		if (wait_for_window) {
			snprintf(reason,
				 reason_size,
				 "waiting for a %lld-second CPU measurement window",
				 (long long)config->cpu_window);
			if (cpu_wait(config->cpu_window)) {
				snprintf(reason,
					 reason_size,
					 "CPU measurement wait failed");
				return 0;
			}
		}
		if (cpu_sample(&sample)) {
			snprintf(reason,
				 reason_size,
				 "cannot read CPU counters or monotonic clock");
			return 0;
		}
		return cpu_result(config, &before, &sample, reason, reason_size);
	}

	if (monitor_append(monitor, &sample)) {
		snprintf(reason, reason_size, "cannot retain CPU sample");
		return 0;
	}
	if (monitor->count > 1
	    && cpu_delta(&monitor->samples[monitor->count - 2],
			 &monitor->samples[monitor->count - 1],
			 &(uint64_t){0},
			 &(uint64_t){0})) {
		cpu_monitor_destroy(monitor);
		snprintf(reason, reason_size, "CPU counters reset; restarting window");
		return 0;
	}
	monitor_prune(monitor, config->cpu_window);
	if (sample_elapsed(&monitor->samples[0],
			   &monitor->samples[monitor->count - 1])
	    < (double)config->cpu_window) {
		snprintf(reason,
			 reason_size,
			 "collecting CPU sample %zu for a %lld-second rolling window "
			 "(%.1f seconds elapsed)",
			 monitor->count,
			 (long long)config->cpu_window,
			 sample_elapsed(&monitor->samples[0],
					&monitor->samples[monitor->count - 1]));
		return 0;
	}
	return cpu_result(config,
			  &monitor->samples[0],
			  &monitor->samples[monitor->count - 1],
			  reason,
			  reason_size);
}

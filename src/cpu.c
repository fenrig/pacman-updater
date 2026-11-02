#define _POSIX_C_SOURCE 200809L

#include "cpu.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct cpu_counters {
	uint64_t user;
	uint64_t nice;
	uint64_t system;
	uint64_t idle;
	uint64_t iowait;
	uint64_t irq;
	uint64_t softirq;
	uint64_t steal;
};

static void monitor_store(cpu_monitor_t *monitor,
			  const struct cpu_counters *sample,
			  const struct timespec *sampled_at)
{
	monitor->user = sample->user;
	monitor->nice = sample->nice;
	monitor->system = sample->system;
	monitor->idle = sample->idle;
	monitor->iowait = sample->iowait;
	monitor->irq = sample->irq;
	monitor->softirq = sample->softirq;
	monitor->steal = sample->steal;
	monitor->sampled_at = *sampled_at;
	monitor->sample_count = 1;
	monitor->initialized = 1;
}

static void monitor_sample(const cpu_monitor_t *monitor,
			   struct cpu_counters *sample)
{
	sample->user = monitor->user;
	sample->nice = monitor->nice;
	sample->system = monitor->system;
	sample->idle = monitor->idle;
	sample->iowait = monitor->iowait;
	sample->irq = monitor->irq;
	sample->softirq = monitor->softirq;
	sample->steal = monitor->steal;
}

static int cpu_sample(struct cpu_counters *sample)
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

static int cpu_delta(const struct cpu_counters *before,
			     const struct cpu_counters *after,
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

int cpu_check(const config_t *config,
	      char *reason,
	      size_t reason_size,
	      cpu_monitor_t *monitor,
	      int wait_for_window)
{
	struct cpu_counters sample1, sample2;
	struct timespec started, sampled_at;
	uint64_t busy, total;
	double elapsed;
	int continuous = monitor != NULL;
	cpu_monitor_t one_shot = {0};

	if (!monitor)
		monitor = &one_shot;
	if (clock_gettime(CLOCK_MONOTONIC, &started)) {
		snprintf(reason, reason_size, "cannot read monotonic clock");
		return 0;
	}
	sampled_at = started;
	if (!monitor->initialized) {
		if (cpu_sample(&sample1)) {
			snprintf(reason, reason_size, "cannot read /proc/stat");
			return 0;
		}
		if (!continuous || wait_for_window) {
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
		} else {
			monitor_store(monitor, &sample1, &started);
			snprintf(reason,
				 reason_size,
				 "collecting CPU sample 1 for a %lld-second window",
				 (long long)config->cpu_window);
			return 0;
		}
	} else {
		monitor_sample(monitor, &sample1);
	}
	if (cpu_sample(&sample2) || cpu_delta(&sample1, &sample2, &busy, &total)) {
		monitor->initialized = 0;
		monitor->sample_count = 0;
		snprintf(
		    reason, reason_size, "CPU counters unavailable or reset");
		return 0;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &sampled_at)) {
		snprintf(reason, reason_size, "cannot read monotonic clock");
		return 0;
	}
	if (monitor->initialized) {
		elapsed = (double)(sampled_at.tv_sec - monitor->sampled_at.tv_sec)
			  + (double)(sampled_at.tv_nsec
				      - monitor->sampled_at.tv_nsec)
				/ 1000000000.0;
	} else {
		elapsed = (double)(sampled_at.tv_sec - started.tv_sec)
			  + (double)(sampled_at.tv_nsec - started.tv_nsec)
				/ 1000000000.0;
	}
	if (continuous && !wait_for_window && elapsed < (double)config->cpu_window) {
		monitor->sample_count++;
		snprintf(reason,
			 reason_size,
			 "collecting CPU sample %u for a %lld-second window "
			 "(%.1f seconds elapsed)",
			 monitor->sample_count,
			 (long long)config->cpu_window,
			 elapsed);
		return 0;
	}
	monitor_store(monitor, &sample2, &sampled_at);
	if (elapsed <= 0.0) {
		snprintf(reason, reason_size, "CPU measurement window is invalid");
		return 0;
	}
	double utilisation = cpu_utilisation(busy, total);

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

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
	      int wait_for_window)
{
	struct cpu_counters sample1, sample2;
	struct timespec started, finished;
	uint64_t busy, total;
	double elapsed;

	if (clock_gettime(CLOCK_MONOTONIC, &started)) {
		snprintf(reason, reason_size, "cannot read monotonic clock");
		return 0;
	}

	if (cpu_sample(&sample1)) {
		snprintf(reason, reason_size, "cannot read /proc/stat");
		return 0;
	}
	if (wait_for_window) {
		snprintf(reason,
			 reason_size,
			 "waiting for a %lld-second CPU measurement window",
			 (long long)config->cpu_window);
		if (cpu_wait(config->cpu_window)) {
			snprintf(reason, reason_size, "CPU measurement wait failed");
			return 0;
		}
	}
	if (cpu_sample(&sample2)
	    || cpu_delta(&sample1, &sample2, &busy, &total)) {
		snprintf(
		    reason, reason_size, "CPU counters unavailable or reset");
		return 0;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &finished)) {
		snprintf(reason, reason_size, "cannot read monotonic clock");
		return 0;
	}
	elapsed = (double)(finished.tv_sec - started.tv_sec)
		  + (double)(finished.tv_nsec - started.tv_nsec) / 1000000000.0;
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

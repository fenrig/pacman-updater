#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int cpu_sample(uint64_t *busy, uint64_t *total)
{
	FILE *file = fopen("/proc/stat", "r");
	char line[256];
	unsigned long long user, nice, system, idle, iowait;
	unsigned long long irq, softirq, steal;

	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line,
			   "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
			   &user,
			   &nice,
			   &system,
			   &idle,
			   &iowait,
			   &irq,
			   &softirq,
			   &steal)
		    == 8) {
			*total = user + nice + system + idle + iowait + irq
				 + softirq + steal;
			*busy = user + nice + system + irq + softirq + steal;
			fclose(file);
			return 0;
		}
	}
	fclose(file);
	return -1;
}

int cpu_check(const config_t *config,
	      char *reason,
	      size_t reason_size,
	      int wait_for_window)
{
	uint64_t busy1, total1, busy2, total2;

	if (cpu_sample(&busy1, &total1)) {
		snprintf(reason, reason_size, "cannot read /proc/stat");
		return 0;
	}
	if (wait_for_window) {
		snprintf(reason,
			 reason_size,
			 "waiting for a %lld-second CPU measurement window",
			 (long long)config->cpu_window);
		sleep((unsigned)config->cpu_window);
	}
	if (cpu_sample(&busy2, &total2) || total2 <= total1 || busy2 < busy1) {
		snprintf(
		    reason, reason_size, "CPU counters unavailable or reset");
		return 0;
	}
	double utilisation = cpu_utilisation(busy2 - busy1, total2 - total1);

	if (utilisation > (double)config->cpu_threshold) {
		snprintf(reason,
			 reason_size,
			 "aggregate CPU utilisation %.1f%% exceeds %.1f%%",
			 utilisation,
			 (double)config->cpu_threshold);
		return 0;
	}
	snprintf(reason,
		 reason_size,
		 "aggregate CPU utilisation %.1f%%",
		 utilisation);
	return 1;
}

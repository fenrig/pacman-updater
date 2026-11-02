#ifndef PACMAN_UPDATER_CPU_H
#define PACMAN_UPDATER_CPU_H

#include "core.h"

typedef struct {
	uint64_t user;
	uint64_t nice;
	uint64_t system;
	uint64_t idle;
	uint64_t iowait;
	uint64_t irq;
	uint64_t softirq;
	uint64_t steal;
	struct timespec sampled_at;
	unsigned sample_count;
	int initialized;
} cpu_monitor_t;

int cpu_check(const config_t *config,
	      char *reason,
	      size_t reason_size,
	      cpu_monitor_t *monitor,
	      int wait_for_window);

#endif

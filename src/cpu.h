#ifndef PACMAN_UPDATER_CPU_H
#define PACMAN_UPDATER_CPU_H

#include "core.h"

int cpu_check(const config_t *config,
	      char *reason,
	      size_t reason_size,
	      int wait_for_window);

#endif

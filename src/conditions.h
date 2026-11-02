#ifndef PACMAN_UPDATER_CONDITIONS_H
#define PACMAN_UPDATER_CONDITIONS_H

#include "core.h"
#include "cpu.h"

int conditions_check(const config_t *config,
		     char *reason,
		     size_t reason_size,
		     cpu_monitor_t *cpu_monitor,
		     int wait_for_cpu_window);

#endif

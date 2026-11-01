#ifndef PACMAN_UPDATER_CONDITIONS_H
#define PACMAN_UPDATER_CONDITIONS_H

#include "core.h"

int conditions_check(const config_t *config,
		     char *reason,
		     size_t reason_size,
		     int wait_for_cpu_window);

#endif

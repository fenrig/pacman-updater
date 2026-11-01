#ifndef PACMAN_UPDATER_BATTERY_H
#define PACMAN_UPDATER_BATTERY_H

#include "core.h"

int battery_check(const config_t *config, char *reason, size_t reason_size);

#endif

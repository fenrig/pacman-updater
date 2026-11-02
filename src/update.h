#ifndef PACMAN_UPDATER_UPDATE_H
#define PACMAN_UPDATER_UPDATE_H

#include "core.h"

int update_run(config_t *config, state_t *state, const char *state_path);
int update_self_test(config_t *config);

#endif

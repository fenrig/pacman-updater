#ifndef PACMAN_UPDATER_PROCESS_H
#define PACMAN_UPDATER_PROCESS_H

#include "core.h"

int process_run(const char *path,
		char *const argv[],
		const config_t *config,
		int as_aur_user,
		const char *cwd);
int process_capture(const char *path,
			char *const argv[],
			const config_t *config,
			int as_aur_user,
			const char *cwd,
			char *output,
			size_t output_size);

#endif

#include "conditions.h"
#include "battery.h"
#include "connectivity.h"
#include "cpu.h"
#include "idle.h"
#include <stdio.h>

static int connectivity_condition(const config_t *config,
				  char *reason,
				  size_t reason_size)
{
	if (connectivity_check(config))
		return 1;
	snprintf(reason,
		 reason_size,
		 "connectivity probe failed (requires HTTPS 204 response)");
	return 0;
}

static int either_idle_condition(const config_t *config,
				cpu_monitor_t *cpu_monitor,
				int wait_for_cpu_window,
				char *reason,
				size_t reason_size)
{
	char user_reason[256];
	char cpu_reason[256];

	if (idle_check(config, user_reason, sizeof(user_reason))) {
		if (!connectivity_condition(config, reason, reason_size))
			return 0;
		snprintf(reason, reason_size, "user idle condition passed");
		return 1;
	}
	if (!cpu_check(config,
		       cpu_reason,
		       sizeof(cpu_reason),
		       cpu_monitor,
		       wait_for_cpu_window)) {
		snprintf(reason,
			 reason_size,
			 "user idle failed: %s; CPU failed: %s",
			 user_reason,
			 cpu_reason);
		return 0;
	}
	if (!connectivity_condition(config, reason, reason_size))
		return 0;
	snprintf(reason, reason_size, "CPU condition passed");
	return 1;
}

int conditions_check(const config_t *config,
		     char *reason,
		     size_t reason_size,
		     cpu_monitor_t *cpu_monitor,
		     int wait_for_cpu_window)
{
	if (!battery_check(config, reason, reason_size))
		return 0;
	if (config->idle_mode == IDLE_EITHER)
		return either_idle_condition(config,
					     cpu_monitor,
					     wait_for_cpu_window,
					     reason,
					     reason_size);
	if (idle_mode_uses_user(config->idle_mode)
	    && !idle_check(config, reason, reason_size))
		return 0;
	if (idle_mode_uses_cpu(config->idle_mode)
	    && !cpu_check(config,
			  reason,
			  reason_size,
			  cpu_monitor,
			  wait_for_cpu_window))
		return 0;
	return connectivity_condition(config, reason, reason_size);
}

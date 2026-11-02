#include "conditions.h"
#include "battery.h"
#include "connectivity.h"
#include "cpu.h"
#include "idle.h"
#include <stdio.h>

int conditions_check(const config_t *config,
		     char *reason,
		     size_t reason_size,
		     cpu_monitor_t *cpu_monitor,
		     int wait_for_cpu_window)
{
	if (!battery_check(config, reason, reason_size))
		return 0;
	if ((config->idle_mode == IDLE_USER || config->idle_mode == IDLE_BOTH)
	    && !idle_check(config, reason, reason_size))
		return 0;
	if ((config->idle_mode == IDLE_CPU || config->idle_mode == IDLE_BOTH)
	    && !cpu_check(config,
			  reason,
			  reason_size,
			  cpu_monitor,
			  wait_for_cpu_window))
		return 0;
	if (!connectivity_check(config)) {
		snprintf(
		    reason,
		    reason_size,
		    "connectivity probe failed (requires HTTPS 204 response)");
		return 0;
	}
	return 1;
}

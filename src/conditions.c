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
	char user_reason[256];
	char cpu_reason[256];
	int user_ok;
	int cpu_ok;

	if (!battery_check(config, reason, reason_size))
		return 0;
	if (config->idle_mode == IDLE_EITHER) {
		user_ok = idle_check(config, user_reason, sizeof(user_reason));
		if (!user_ok) {
			cpu_ok = cpu_check(config,
					   cpu_reason,
					   sizeof(cpu_reason),
					   cpu_monitor,
					   wait_for_cpu_window);
			if (!cpu_ok) {
				snprintf(reason,
					 reason_size,
					 "user idle failed: %s; CPU failed: %s",
					 user_reason,
					 cpu_reason);
				return 0;
			}
		} else {
			cpu_ok = 1;
		}
		if (!connectivity_check(config)) {
			snprintf(reason,
				 reason_size,
				 "connectivity probe failed (requires HTTPS 204 response)");
			return 0;
		}
		snprintf(reason,
			 reason_size,
			 "%s condition passed",
			 user_ok ? "user idle" : "CPU");
		(void)cpu_ok;
		return 1;
	}
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

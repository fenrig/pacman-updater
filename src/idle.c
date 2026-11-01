#define _GNU_SOURCE
#include "idle.h"
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int
no_user_uptime_ok(const config_t *config, char *reason, size_t reason_size)
{
	FILE *file = fopen("/proc/uptime", "r");
	double uptime;

	if (!file)
		goto unavailable;
	if (fscanf(file, "%lf", &uptime) != 1) {
		fclose(file);
		goto unavailable;
	}
	if (fclose(file)) {
		file = NULL;
		goto unavailable;
	}
	if (uptime < (double)config->no_user_uptime) {
		snprintf(reason,
			 reason_size,
			 "no logged-in users; uptime %.0f seconds, need %lld",
			 uptime,
			 (long long)config->no_user_uptime);
		return 0;
	}
	snprintf(reason,
		 reason_size,
		 "no logged-in users; uptime %.0f seconds",
		 uptime);
	return 1;
unavailable:
	if (file)
		fclose(file);
	snprintf(reason, reason_size, "machine uptime unavailable");
	return 0;
}

int idle_check(const config_t *config, char *reason, size_t reason_size)
{
	char **sessions = NULL;
	int session_count = sd_get_sessions(&sessions);

	if (session_count == 0) {
		free(sessions);
		return no_user_uptime_ok(config, reason, reason_size);
	}
	if (session_count < 0) {
		snprintf(reason,
			 reason_size,
			 "login session information unavailable");
		return 0;
	}
	for (int i = 0; sessions[i]; i++)
		free(sessions[i]);
	free(sessions);

	sd_bus *bus = NULL;
	int result = sd_bus_open_system(&bus);
	int idle = 0;
	uint64_t since = 0;

	if (result < 0) {
		snprintf(reason,
			 reason_size,
			 "logind unavailable (%s)",
			 strerror(-result));
		return 0;
	}
	result = sd_bus_get_property_trivial(bus,
					     "org.freedesktop.login1",
					     "/org/freedesktop/login1",
					     "org.freedesktop.login1.Manager",
					     "IdleHint",
					     NULL,
					     'b',
					     &idle);
	if (result >= 0)
		result = sd_bus_get_property_trivial(
		    bus,
		    "org.freedesktop.login1",
		    "/org/freedesktop/login1",
		    "org.freedesktop.login1.Manager",
		    "IdleSinceHintMonotonic",
		    NULL,
		    't',
		    &since);
	sd_bus_unref(bus);
	if (result < 0) {
		snprintf(reason,
			 reason_size,
			 "user inactivity information unavailable");
		return 0;
	}
	if (!idle || !since) {
		snprintf(
		    reason, reason_size, "user inactivity below threshold");
		return 0;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	uint64_t now_usec =
	    (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_nsec / 1000;
	uint64_t required = (uint64_t)config->idle_required * 1000000ULL;

	if (now_usec < since || now_usec - since < required) {
		snprintf(reason,
			 reason_size,
			 "idle for %llu seconds, need %lld",
			 (unsigned long long)(now_usec > since
						  ? now_usec - since
						  : 0)
			     / 1000000,
			 (long long)config->idle_required);
		return 0;
	}
	snprintf(reason,
		 reason_size,
		 "user idle for %llu seconds",
		 (unsigned long long)(now_usec - since) / 1000000);
	return 1;
}

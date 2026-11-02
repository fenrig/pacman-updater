#define _GNU_SOURCE
#include "battery.h"
#include "connectivity.h"
#include "core.h"
#include "cpu.h"
#include "idle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_time(const char *name, int64_t value)
{
	char buffer[64];
	time_t timestamp = (time_t)value;
	struct tm tm;

	if (!value || !localtime_r(&timestamp, &tm)
	    || !strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S %Z", &tm))
		printf("%-16s never\n", name);
	else
		printf("%-16s %s\n", name, buffer);
}

static const char *idle_mode_name(idle_mode_t mode)
{
	switch (mode) {
	case IDLE_USER:
		return "user";
	case IDLE_CPU:
		return "cpu";
	case IDLE_BOTH:
		return "both";
	}
	return "unknown";
}

static void print_config(const config_t *config)
{
	printf("effective configuration:\n");
	printf("  update_interval:      %llds\n",
	       (long long)config->update_interval);
	printf("  poll_interval:        %llds\n",
	       (long long)config->poll_interval);
	printf("  wait_until_due:       %s\n",
	       config->wait_until_due ? "yes" : "no");
	printf("  idle_mode:            %s\n",
	       idle_mode_name(config->idle_mode));
	printf("  idle_required:        %llds\n",
	       (long long)config->idle_required);
	printf("  cpu_window:           %llds\n",
	       (long long)config->cpu_window);
	printf("  cpu_threshold:        %d%%\n", config->cpu_threshold);
	printf("  no_user_uptime:       %llds\n",
	       (long long)config->no_user_uptime);
	printf("  battery_external:     %d%%\n", config->battery_external);
	printf("  battery_charging:     %d%%\n", config->battery_charging);
	printf("  battery_discharging:  %d%%\n",
	       config->battery_discharging);
	printf("  timeout:               %llds\n",
	       (long long)config->timeout);
	printf("  aur_user:              %s\n", config->aur_user);
	printf("  endpoint:              %s\n", config->endpoint);
	printf("  fallback_endpoint:     %s\n", config->fallback_endpoint);
	printf("  custom_repo_url:       %s\n",
	       *config->custom_repo_url ? config->custom_repo_url : "disabled");
	printf("  custom_repo_ref:       %s\n", config->custom_repo_ref);
	printf("  timer_calendar:        %s\n", config->timer_calendar);
	printf("  state_dir:             %s\n", config->state_dir);
	printf("  parallelism:           %d\n", config->parallelism);
	printf("  verbosity:              %d\n", config->verbosity);
}

static int condition_unavailable(const char *reason)
{
	return strstr(reason, "unavailable") || strstr(reason, "cannot read")
	       || strstr(reason, "counter") || strstr(reason, "logind");
}

static int check_condition(const char *name,
			   int result,
			   const char *reason,
			   int *check_error)
{
	const char *status =
	    result ? "PASS"
		   : (condition_unavailable(reason) ? "ERROR" : "POSTPONE");

	if (!result && !strcmp(status, "ERROR"))
		*check_error = 1;
	printf("%-16s %s (%s)\n", name, status, reason);
	return result;
}

int main(int argc, char **argv)
{
	const char *config_path = "/etc/pacman-updater.conf";
	char error[256];
	char reason[256];
	config_t config;
	state_t state;
	int wait_for_cpu = 0;
	int cpu_window_override = 0;
	const char *cpu_window_value = NULL;
	int all_passed = 1;
	int check_error = 0;
	int due;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--wait-cpu"))
			wait_for_cpu = 1;
		else if (!strcmp(argv[i], "--cpu-window")
			 && i + 1 < argc) {
			cpu_window_override = 1;
			cpu_window_value = argv[++i];
			wait_for_cpu = 1;
		}
		else if (!strcmp(argv[i], "--config") && i + 1 < argc)
			config_path = argv[++i];
		else {
			fprintf(stderr,
				"usage: %s [--config FILE] [--wait-cpu] "
				"[--cpu-window DURATION]\n",
				argv[0]);
			return 2;
		}
	}
	if (load_config(config_path, &config, error, sizeof(error))) {
		fprintf(stderr, "configuration invalid: %s\n", error);
		return 2;
	}
	if (cpu_window_override
	    && parse_duration(cpu_window_value, &config.cpu_window)) {
		fprintf(stderr, "invalid CPU window: %s\n", cpu_window_value);
		return 2;
	}
	char state_path[512];
	if (snprintf(
		state_path, sizeof(state_path), "%s/state", config.state_dir)
	    >= (int)sizeof(state_path)) {
		fprintf(stderr, "state path is too long\n");
		return 2;
	}
	if (load_state(state_path, &state, error, sizeof(error))) {
		fprintf(stderr, "state invalid: %s\n", error);
		return 2;
	}

	due = update_due(&state, time(NULL), config.update_interval);
	printf("configuration: %s\n", config_path);
	print_config(&config);
	printf("state:         %s\n", state_path);
	printf("update due:    %s\n", due ? "yes" : "no");
	print_time("last success", state.last_success);
	print_time("last attempt", state.last_attempt);
	print_time("last pacman", state.last_pacman);
	print_time("last yay", state.last_yay);
	print_time("last custom", state.last_custom);
	printf("last result:   %d\n", state.last_result);
	printf("\nconditions (worker order):\n");

	{
		int result = battery_check(&config, reason, sizeof(reason));

		if (!check_condition("battery", result, reason, &check_error))
			all_passed = 0;
	}

	if (config.idle_mode == IDLE_USER || config.idle_mode == IDLE_BOTH) {
		int result = idle_check(&config, reason, sizeof(reason));
		if (!check_condition("user idle", result, reason, &check_error))
			all_passed = 0;
	} else {
		printf("%-16s disabled\n", "user idle");
	}

	if (wait_for_cpu || cpu_window_override || config.idle_mode == IDLE_CPU
	    || config.idle_mode == IDLE_BOTH) {
		int result =
		    cpu_check(&config,
			      reason,
			      sizeof(reason),
			      NULL,
			      wait_for_cpu);
		if (!check_condition("cpu", result, reason, &check_error))
			all_passed = 0;
		if (!wait_for_cpu)
			printf("  note: CPU used a quick sample; use "
			       "--wait-cpu for the configured window\n");
		else if (cpu_window_override)
			printf("  note: CPU window overridden by command line: %s\n",
			       cpu_window_value);
	} else {
		printf("%-16s disabled\n", "cpu");
	}

	if (!connectivity_check(&config)) {
		all_passed = 0;
		check_condition("connectivity",
				0,
				"HTTPS 204 probe failed (primary and fallback)",
				&check_error);
	} else {
		check_condition("connectivity",
				1,
				"HTTPS 204 probe succeeded",
				&check_error);
	}

	printf("\nresult: %s\n",
	       due && all_passed ? "would start an update"
	       : due		 ? "postponed until the next check"
				 : "no update needed yet");
	return check_error ? 2 : (due && !all_passed ? 1 : 0);
}

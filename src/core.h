#ifndef PACMAN_UPDATER_CORE_H
#define PACMAN_UPDATER_CORE_H
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum { IDLE_USER, IDLE_CPU, IDLE_BOTH } idle_mode_t;

typedef enum {
	POWER_UNKNOWN,
	POWER_EXTERNAL,
	POWER_CHARGING,
	POWER_DISCHARGING,
	POWER_FULL
} power_t;

typedef struct {
	int64_t update_interval, poll_interval, idle_required, cpu_window,
	    no_user_uptime, timeout;
	int cpu_threshold, battery_external, battery_charging,
	    battery_discharging;
	idle_mode_t idle_mode;
	char aur_user[64], endpoint[512], fallback_endpoint[512];
	char custom_repo_url[512], custom_repo_ref[128];
	char timer_calendar[128], state_dir[256];
	int parallelism, verbosity;
} config_t;

typedef struct {
	int version;
	int64_t last_success, last_attempt, last_pacman, last_yay, last_custom;
	int last_result;
} state_t;

typedef struct {
	power_t power;
	int capacity;
	bool capacity_known, any_battery, status_known;
} battery_info_t;

int parse_duration(const char *, int64_t *);
bool update_due(const state_t *, int64_t now, int64_t interval);
bool battery_eligible(const battery_info_t *, const config_t *, char *, size_t);
double cpu_utilisation(uint64_t busy_delta, uint64_t total_delta);
int load_config(const char *, config_t *, char *, size_t);
int load_state(const char *, state_t *, char *, size_t);
int save_state_atomic(const char *, const state_t *, char *, size_t);
void defaults(config_t *);

#endif

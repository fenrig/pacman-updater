#define _POSIX_C_SOURCE 200809L
#include "core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
	int64_t d;
	assert(parse_duration("7d", &d) == 0 && d == 604800);
	assert(parse_duration("5m", &d) == 0 && d == 300);
	assert(parse_duration("0s", &d) < 0);
	state_t s = {.version = 1};
	assert(update_due(&s, 1, 10));
	s.last_success = 100;
	assert(!update_due(&s, 109, 10));
	assert(update_due(&s, 110, 10));
	config_t c;
	defaults(&c);
	assert(c.no_user_uptime == 900);
	battery_info_t b = {.any_battery = true,
			    .status_known = true,
			    .capacity_known = true,
			    .power = POWER_DISCHARGING,
			    .capacity = 90};
	char why[128];
	assert(!battery_eligible(&b, &c, why, sizeof why));
	b.capacity = 91;
	assert(battery_eligible(&b, &c, why, sizeof why));
	b.power = POWER_EXTERNAL;
	b.capacity_known = false;
	assert(battery_eligible(&b, &c, why, sizeof why));
	assert(cpu_utilisation(10, 100) == 10.0);
	char path[] = "/tmp/pacman-updater-state-XXXXXX";
	int fd = mkstemp(path);
	close(fd);
	unlink(path);
	state_t out = {.version = 1,
		       .last_success = 42,
		       .last_attempt = 43,
		       .last_pacman = 44,
		       .last_yay = 45,
		       .last_custom = 46,
		       .last_result = 7};
	assert(save_state_atomic(path, &out, why, sizeof why) == 0);
	state_t in;
	assert(load_state(path, &in, why, sizeof why) == 0
	       && in.last_success == 42 && in.last_custom == 46
	       && in.last_result == 7);
	unlink(path);
	puts("core tests passed");
}

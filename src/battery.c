#include "battery.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_string(const char *path, char *buffer, size_t size)
{
	FILE *file = fopen(path, "r");
	int ok;

	if (!file)
		return -1;
	ok = fgets(buffer, (int)size, file) != NULL;
	fclose(file);
	if (ok)
		buffer[strcspn(buffer, "\r\n")] = '\0';
	return ok ? 0 : -1;
}

static int read_integer(const char *path, int *value)
{
	char buffer[64], *end;
	long number;

	if (read_string(path, buffer, sizeof(buffer)))
		return -1;
	number = strtol(buffer, &end, 10);
	if (*end)
		return -1;
	*value = (int)number;
	return 0;
}

int battery_check(const config_t *config, char *reason, size_t reason_size)
{
	battery_info_t battery = {.power = POWER_UNKNOWN};
	DIR *directory = opendir("/sys/class/power_supply");
	struct dirent *entry;
	bool external = false, charging = false, discharging = false;
	bool full = false, status_bad = false, capacity_bad = false;
	int minimum_capacity = 101;

	if (!directory) {
		snprintf(reason,
			 reason_size,
			 "battery information unavailable: cannot read "
			 "/sys/class/power_supply");
		return 0;
	}
	{
		while ((entry = readdir(directory))) {
			char path[512], buffer[128], type[32];
			FILE *uevent;
			bool peripheral = false;
			int capacity;

			if (entry->d_name[0] == '.')
				continue;
			snprintf(path,
				 sizeof(path),
				 "/sys/class/power_supply/%s/type",
				 entry->d_name);
			if (read_string(path, type, sizeof(type))
			    || strcmp(type, "Battery"))
				continue;
			snprintf(path,
				 sizeof(path),
				 "/sys/class/power_supply/%s/uevent",
				 entry->d_name);
			uevent = fopen(path, "r");
			if (uevent) {
				while (fgets(buffer, sizeof(buffer), uevent))
					if (!strncmp(
						buffer,
						"POWER_SUPPLY_SCOPE=Device",
						26))
						peripheral = true;
				fclose(uevent);
			}
			if (peripheral)
				continue;
			battery.any_battery = true;
			snprintf(path,
				 sizeof(path),
				 "/sys/class/power_supply/%s/status",
				 entry->d_name);
			if (read_string(path, buffer, sizeof(buffer)))
				status_bad = true;
			else if (!strcmp(buffer, "Charging"))
				charging = true;
			else if (!strcmp(buffer, "Discharging"))
				discharging = true;
			else if (!strcmp(buffer, "Full"))
				full = true;
			else
				status_bad = true;
			snprintf(path,
				 sizeof(path),
				 "/sys/class/power_supply/%s/capacity",
				 entry->d_name);
			if (read_integer(path, &capacity))
				capacity_bad = true;
			else if (capacity < minimum_capacity)
				minimum_capacity = capacity;
		}
		closedir(directory);
	}
	directory = opendir("/sys/class/power_supply");
	if (directory) {
		while ((entry = readdir(directory))) {
			char path[512], online[16], type[32];

			if (entry->d_name[0] == '.')
				continue;
			snprintf(path,
				 sizeof(path),
				 "/sys/class/power_supply/%s/type",
				 entry->d_name);
			if (read_string(path, type, sizeof(type))
			    || strcmp(type, "Mains"))
				continue;
			snprintf(path,
				 sizeof(path),
				 "/sys/class/power_supply/%s/online",
				 entry->d_name);
			if (!read_string(path, online, sizeof(online))
			    && !strcmp(online, "1"))
				external = true;
		}
		closedir(directory);
	}
	battery.capacity_known = !capacity_bad;
	battery.capacity = minimum_capacity;
	battery.status_known = battery.any_battery && !status_bad;
	if (!battery.any_battery) {
		battery.status_known = true;
		battery.power = POWER_EXTERNAL;
	} else if (full) {
		battery.power = POWER_FULL;
	} else if (external) {
		battery.power = POWER_EXTERNAL;
	} else if (charging) {
		battery.power = POWER_CHARGING;
	} else if (discharging) {
		battery.power = POWER_DISCHARGING;
	}
	if (!battery_eligible(&battery, config, reason, reason_size))
		return 0;
	return 1;
}

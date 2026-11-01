#define _GNU_SOURCE
#include "core.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void err(char *e, size_t n, const char *s)
{
	if (n)
		snprintf(e, n, "%s", s);
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *p = s + strlen(s);
	while (p > s && isspace((unsigned char)p[-1]))
		*--p = 0;
	return s;
}

int parse_duration(const char *s, int64_t *out)
{
	char *e;
	errno = 0;
	long long v = strtoll(s, &e, 10);
	if (errno || e == s || v <= 0)
		return -1;
	long long m = 1;
	if (!strcmp(e, "s"))
		m = 1;
	else if (!strcmp(e, "m"))
		m = 60;
	else if (!strcmp(e, "h"))
		m = 3600;
	else if (!strcmp(e, "d"))
		m = 86400;
	else
		return -1;
	if (v > INT64_MAX / m)
		return -1;
	*out = v * m;
	return 0;
}

void defaults(config_t *c)
{
	memset(c, 0, sizeof(*c));
	c->update_interval = 604800;
	c->poll_interval = 300;
	c->idle_mode = IDLE_USER;
	c->idle_required = 1800;
	c->cpu_window = 300;
	c->no_user_uptime = 900;
	c->cpu_threshold = 10;
	c->battery_external = 25;
	c->battery_charging = 25;
	c->battery_discharging = 90;
	c->timeout = 10;
	c->parallelism = 0;
	c->verbosity = 1;
	strcpy(c->endpoint,
	       "https://connectivitycheck.gstatic.com/generate_204");
	strcpy(c->fallback_endpoint, "https://www.google.com/generate_204");
	strcpy(c->custom_repo_ref, "main");
	strcpy(c->timer_calendar, "*-*-* 00:00:00");
	strcpy(c->state_dir, "/var/lib/pacman-updater");
}

bool update_due(const state_t *s, int64_t now, int64_t interval)
{
	return s->last_success <= 0 || now - s->last_success >= interval;
}

double cpu_utilisation(uint64_t b, uint64_t t)
{
	return t ? 100.0 * (double)b / (double)t : 0.0;
}

bool battery_eligible(const battery_info_t *b,
		      const config_t *c,
		      char *why,
		      size_t n)
{
	if (!b->any_battery) {
		err(why, n, "no system battery");
		return true;
	}
	if (!b->status_known) {
		err(why, n, "battery power status unknown");
		return false;
	}
	int min = (b->power == POWER_DISCHARGING)
		      ? c->battery_discharging
		      : (b->power == POWER_EXTERNAL ? c->battery_external
						    : c->battery_charging);
	if (!b->capacity_known && b->power == POWER_EXTERNAL) {
		err(why, n, "capacity unavailable on confirmed external power");
		return true;
	}
	if (!b->capacity_known) {
		err(why, n, "battery capacity unavailable");
		return false;
	}
	bool pass = (b->power == POWER_FULL)
		    || (b->power == POWER_DISCHARGING ? b->capacity > min
						      : b->capacity >= min);
	if (pass) {
		err(why, n, "battery eligible");
		return true;
	}
	snprintf(why,
		 n,
		 "battery capacity %d%% below required %d%%",
		 b->capacity,
		 min);
	return false;
}

static int setval(config_t *c, const char *k, const char *v, char *e, size_t n)
{
	int64_t d;
	char *x;
	long z;
#define DUR(field)                                                             \
	do {                                                                   \
		if (parse_duration(v, &d)) {                                   \
			err(e, n, "invalid duration");                         \
			return -1;                                             \
		}                                                              \
		c->field = d;                                                  \
	} while (0)
	if (!strcmp(k, "update_interval"))
		DUR(update_interval);
	else if (!strcmp(k, "poll_interval"))
		DUR(poll_interval);
	else if (!strcmp(k, "idle_required"))
		DUR(idle_required);
	else if (!strcmp(k, "cpu_window"))
		DUR(cpu_window);
	else if (!strcmp(k, "no_user_uptime"))
		DUR(no_user_uptime);
	else if (!strcmp(k, "connect_timeout"))
		DUR(timeout);
	else if (!strcmp(k, "idle_mode")) {
		if (!strcmp(v, "user"))
			c->idle_mode = IDLE_USER;
		else if (!strcmp(v, "cpu"))
			c->idle_mode = IDLE_CPU;
		else if (!strcmp(v, "both"))
			c->idle_mode = IDLE_BOTH;
		else {
			err(e, n, "idle_mode must be user, cpu, or both");
			return -1;
		}
	} else if (!strcmp(k, "aur_user")) {
		if (strlen(v) == 0 || strlen(v) >= sizeof(c->aur_user)
		    || strchr(v, '/')) {
			err(e, n, "invalid aur_user");
			return -1;
		}
		strcpy(c->aur_user, v);
	} else if (!strcmp(k, "endpoint")) {
		if (strlen(v) >= sizeof(c->endpoint)
		    || strncmp(v, "https://", 8)) {
			err(e, n, "endpoint must be an https URL");
			return -1;
		}
		strcpy(c->endpoint, v);
	} else if (!strcmp(k, "fallback_endpoint")) {
		if (strlen(v) >= sizeof(c->fallback_endpoint)
		    || (v[0] && strncmp(v, "https://", 8))) {
			err(e,
			    n,
			    "fallback_endpoint must be empty or an https URL");
			return -1;
		}
		strcpy(c->fallback_endpoint, v);
	} else if (!strcmp(k, "custom_repo_url")) {
		if (strlen(v) >= sizeof(c->custom_repo_url)
		    || (v[0] && strncmp(v, "https://", 8))) {
			err(e,
			    n,
			    "custom_repo_url must be empty or an https URL");
			return -1;
		}
		strcpy(c->custom_repo_url, v);
	} else if (!strcmp(k, "custom_repo_ref")) {
		if (strlen(v) >= sizeof(c->custom_repo_ref)) {
			err(e, n, "custom_repo_ref too long");
			return -1;
		}
		strcpy(c->custom_repo_ref, v);
	} else if (!strcmp(k, "timer_calendar")) {
		if (strlen(v) >= sizeof(c->timer_calendar)) {
			err(e, n, "timer_calendar too long");
			return -1;
		}
		strcpy(c->timer_calendar, v);
	} else if (!strcmp(k, "state_dir")) {
		if (strlen(v) >= sizeof(c->state_dir) || v[0] != '/') {
			err(e, n, "state_dir must be absolute");
			return -1;
		}
		strcpy(c->state_dir, v);
	} else if (!strcmp(k, "cpu_threshold") || !strcmp(k, "battery_external")
		   || !strcmp(k, "battery_charging")
		   || !strcmp(k, "battery_discharging")
		   || !strcmp(k, "verbosity") || !strcmp(k, "parallelism")) {
		errno = 0;
		z = strtol(v, &x, 10);
		if (errno || *x || z < 0
		    || z > (!strcmp(k, "cpu_threshold")
				? 100
				: (!strcmp(k, "verbosity") ? 3 : 1000))) {
			err(e, n, "invalid numeric setting");
			return -1;
		}
		if (!strcmp(k, "cpu_threshold"))
			c->cpu_threshold = (int)z;
		else if (!strcmp(k, "battery_external"))
			c->battery_external = (int)z;
		else if (!strcmp(k, "battery_charging"))
			c->battery_charging = (int)z;
		else if (!strcmp(k, "battery_discharging"))
			c->battery_discharging = (int)z;
		else if (!strcmp(k, "verbosity"))
			c->verbosity = (int)z;
		else
			c->parallelism = (int)z;
	} else {
		err(e, n, "unknown configuration key");
		return -1;
	}
	return 0;
#undef DUR
}

int load_config(const char *path, config_t *c, char *e, size_t n)
{
	struct stat file_status;
	FILE *f;
	char line[1024];
	int ln = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	if (fd < 0) {
		snprintf(e, n, "cannot open config: %s", strerror(errno));
		return -1;
	}
	if (fstat(fd, &file_status)) {
		snprintf(e, n, "cannot inspect config: %s", strerror(errno));
		close(fd);
		return -1;
	}
	if (!S_ISREG(file_status.st_mode)) {
		err(e, n, "config must be a regular file");
		close(fd);
		return -1;
	}
	if (file_status.st_uid != 0) {
		err(e, n, "config must be root-owned");
		close(fd);
		return -1;
	}
	if (file_status.st_mode & 0022) {
		err(e, n, "config must not be group/world writable");
		close(fd);
		return -1;
	}
	f = fdopen(fd, "r");
	if (!f) {
		snprintf(e, n, "cannot read config: %s", strerror(errno));
		close(fd);
		return -1;
	}
	defaults(c);
	while (fgets(line, sizeof line, f)) {
		ln++;
		char *p = trim(line);
		if (!*p || *p == '#')
			continue;
		char *eq = strchr(p, '=');
		if (!eq) {
			fclose(f);
			snprintf(e, n, "line %d: expected key=value", ln);
			return -1;
		}
		*eq = 0;
		char *k = trim(p), *v = trim(eq + 1);
		if (setval(c, k, v, e, n)) {
			char tmp[256];
			snprintf(tmp, sizeof tmp, "line %d: %s", ln, e);
			err(e, n, tmp);
			fclose(f);
			return -1;
		}
	}
	fclose(f);
	if (!*c->aur_user) {
		err(e, n, "aur_user is required");
		return -1;
	}
	{
		struct passwd *user = getpwnam(c->aur_user);
		struct stat home_status;

		if (!user) {
			snprintf(
			    e, n, "aur_user '%s' does not exist", c->aur_user);
			return -1;
		}
		if (user->pw_uid == 0) {
			err(e, n, "aur_user must not be root");
			return -1;
		}
		if (!user->pw_dir || user->pw_dir[0] != '/') {
			err(e,
			    n,
			    "aur_user must have an absolute home directory");
			return -1;
		}
		if (stat(user->pw_dir, &home_status)
		    || !S_ISDIR(home_status.st_mode)) {
			err(e, n, "aur_user home directory does not exist");
			return -1;
		}
	}
	if (c->idle_mode == IDLE_USER || c->idle_mode == IDLE_BOTH) {
		if (c->idle_required <= 0) {
			err(e, n, "idle_required must be positive");
			return -1;
		}
	}
	return 0;
}

int load_state(const char *path, state_t *s, char *e, size_t n)
{
	FILE *f = fopen(path, "r");
	char k[64];
	long long v;
	if (!f) {
		if (errno == ENOENT) {
			memset(s, 0, sizeof *s);
			s->version = 1;
			return 0;
		}
		snprintf(e, n, "cannot open state: %s", strerror(errno));
		return -1;
	}
	memset(s, 0, sizeof *s);
	s->version = 1;
	while (fscanf(f, "%63[^=]=%lld\n", k, &v) == 2) {
		if (!strcmp(k, "version"))
			s->version = (int)v;
		else if (!strcmp(k, "last_success"))
			s->last_success = v;
		else if (!strcmp(k, "last_attempt"))
			s->last_attempt = v;
		else if (!strcmp(k, "last_pacman"))
			s->last_pacman = v;
		else if (!strcmp(k, "last_yay"))
			s->last_yay = v;
		else if (!strcmp(k, "last_custom"))
			s->last_custom = v;
		else if (!strcmp(k, "last_result"))
			s->last_result = (int)v;
		else {
			fclose(f);
			err(e, n, "unknown state field");
			return -1;
		}
	}
	if (ferror(f) || !feof(f)) {
		fclose(f);
		err(e, n, "corrupt state");
		return -1;
	}
	fclose(f);
	if (s->version != 1 || s->last_success < 0 || s->last_attempt < 0
	    || s->last_custom < 0) {
		err(e, n, "invalid state values");
		return -1;
	}
	return 0;
}

int save_state_atomic(const char *path, const state_t *s, char *e, size_t n)
{
	char tmp[512];
	snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		snprintf(e, n, "state temp: %s", strerror(errno));
		return -1;
	}
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(tmp);
		err(e, n, "state fdopen failed");
		return -1;
	}
	fprintf(f,
		"version=1\nlast_success=%lld\nlast_attempt=%lld\nlast_pacman=%"
		"lld\nlast_yay=%lld\nlast_custom=%lld\nlast_result=%d\n",
		(long long)s->last_success,
		(long long)s->last_attempt,
		(long long)s->last_pacman,
		(long long)s->last_yay,
		(long long)s->last_custom,
		s->last_result);
	if (fclose(f) || rename(tmp, path)) {
		unlink(tmp);
		err(e, n, "state atomic commit failed");
		return -1;
	}
	return 0;
}

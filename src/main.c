#define _GNU_SOURCE
#include "conditions.h"
#include "core.h"
#include "custom.h"
#include "logging.h"
#include "process.h"
#include "systemd.h"
#include "update.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static time_t midnight(void)
{
	time_t now = time(NULL);
	struct tm t;
	localtime_r(&now, &t);
	t.tm_hour = 0;
	t.tm_min = 0;
	t.tm_sec = 0;
	t.tm_mday++;
	return mktime(&t);
}

int main(int argc, char **argv)
{
	const char *cfgpath = "/etc/pacman-updater.conf";
	int dry = 0;
	int selftest = 0;
	int force_update = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dry-run"))
			dry = 1;
		else if (!strcmp(argv[i], "--self-test"))
			selftest = 1;
		else if (!strcmp(argv[i], "--force-update"))
			force_update = 1;
		else if (!strcmp(argv[i], "--config") && i + 1 < argc)
			cfgpath = argv[++i];
		else {
			fprintf(stderr,
				"usage: %s [--dry-run] [--self-test] "
				"[--force-update] [--config FILE]\n",
				argv[0]);
			return 2;
		}
	}
	config_t c;
	char e[256];
	if (load_config(cfgpath, &c, e, sizeof e)) {
		fprintf(stderr, "configuration invalid: %s\n", e);
		return 2;
	}
	if (dry && force_update) {
		fprintf(stderr,
			"--dry-run and --force-update cannot be combined\n");
		return 2;
	}
	if (selftest)
		return update_self_test(&c);
	if (force_update && geteuid() != 0) {
		fprintf(stderr,
			"--force-update must be run as root, for example with "
			"sudo\n");
		return 2;
	}
	log_set_verbosity(c.verbosity);
	if (mkdir(c.state_dir, 0700) && errno != EEXIST) {
		fprintf(stderr,
			"cannot create state directory: %s\n",
			strerror(errno));
		return 2;
	}
	char lockp[512], statep[512];
	snprintf(lockp, sizeof lockp, "%s/worker.lock", c.state_dir);
	snprintf(statep, sizeof statep, "%s/state", c.state_dir);
	int lf = open(lockp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lf < 0 || flock(lf, LOCK_EX | LOCK_NB)) {
		logmsg(1, "another worker is already active; exiting");
		return 0;
	}
	state_t s;
	if (load_state(statep, &s, e, sizeof e)) {
		fprintf(stderr, "state invalid: %s\n", e);
		return 2;
	}
	time_t now = time(NULL);
	int due = update_due(&s, now, c.update_interval);
	logmsg(1,
	       "update is %s",
	       force_update ? "forced" : (due ? "due" : "not due"));
	if (!force_update && !due) {
		int64_t remaining = c.update_interval
				    - (int64_t)(now - s.last_success);

		if (!c.wait_until_due || remaining >= 86400)
			return 0;
		logmsg(1,
		       "update is due within %lld seconds; waiting for due time",
		       (long long)remaining);
		for (;;) {
			now = time(NULL);
			if (update_due(&s, now, c.update_interval)) {
				due = 1;
				logmsg(1, "update interval elapsed; checking eligibility");
				break;
			}
			remaining = c.update_interval
				    - (int64_t)(now - s.last_success);
			time_t next_timer;
			unsigned sleepsec;
			time_t due_at = now + (time_t)remaining;

			if (systemd_next_timer_invocation(&next_timer)) {
				logmsg(0,
				       "cannot determine next timer invocation; exiting "
				       "before update is due");
				return 0;
			}
			if (next_timer <= due_at) {
				logmsg(1,
				       "next timer invocation precedes update due time; "
				       "exiting");
				return 0;
			}
			logmsg(1,
			       "update not due; sleeping until due in %lld seconds",
			       (long long)remaining);
			sleepsec = (unsigned)remaining;
			if (!sleepsec)
				sleepsec = 1;
			sleep(sleepsec);
		}
	}
	if (force_update) {
		logmsg(0,
		       "WARNING: forcing update; eligibility conditions are "
		       "being bypassed");
		s.last_attempt = time(NULL);
		if (save_state_atomic(statep, &s, e, sizeof e)) {
			logmsg(0, "could not persist forced attempt: %s", e);
			return 1;
		}
		int rc = update_run(&c, &s, statep);
		if (save_state_atomic(statep, &s, e, sizeof e))
			logmsg(0, "could not persist state: %s", e);
		return rc ? 1 : 0;
	}
	if (dry) {
		char why[256];
		cpu_monitor_t cpu_monitor = {0};
		int ok = conditions_check(&c, why, sizeof why, &cpu_monitor, 0);
		printf(
		    "dry-run: %s (%s)\n", ok ? "eligible" : "postponed", why);
		cpu_monitor_destroy(&cpu_monitor);
		return 0;
	}
	time_t end = midnight();
	cpu_monitor_t cpu_monitor = {0};
	for (;;) {
		char why[256];
		if (time(NULL) >= end) {
			cpu_monitor_destroy(&cpu_monitor);
			return 0;
		}
		if (conditions_check(&c, why, sizeof why, &cpu_monitor, 0)) {
			if (time(NULL) >= end) {
				cpu_monitor_destroy(&cpu_monitor);
				return 0;
			}
			logmsg(1, "eligibility passed: %s", why);
			s.last_attempt = time(NULL);
			save_state_atomic(statep, &s, e, sizeof e);
			int rc = update_run(&c, &s, statep);
			if (save_state_atomic(statep, &s, e, sizeof e))
				logmsg(0, "could not persist state: %s", e);
			cpu_monitor_destroy(&cpu_monitor);
			return rc ? 1 : 0;
		}
		logmsg(1, "postponed: %s; retrying before local midnight", why);
		time_t left = end - time(NULL);
		if (left <= 0)
			return 0;
		unsigned sleepsec = (unsigned)(left < (time_t)c.poll_interval
						   ? left
						   : c.poll_interval);
		if (!sleepsec)
			sleepsec = 1;
		sleep(sleepsec);
	}
}

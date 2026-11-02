#define _GNU_SOURCE
#include "custom.h"
#include "logging.h"
#include "process.h"
#include "systemd.h"
#include "update.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PACMAN_UPDATER_SUDO                                                    \
	"/usr/bin/sudo"
#define PACMAN_UPDATER_SUDO_WRAPPER                                             \
	"/usr/lib/pacman-updater/pacman-updater-pacman-wrapper"

int update_run(config_t *c, state_t *s, const char *state_path)
{
	systemd_inhibitor_t inhibitor;
	int r = systemd_inhibit(&inhibitor);
	if (r < 0) {
		logmsg(0,
		       "cannot acquire sleep/shutdown inhibitor: %s",
		       strerror(-r));
		return 1;
	}
	logmsg(1, "inhibitor acquired; starting package update");
		systemd_notify_user("Update started",
				     "System and user package updates are running.");
	const char *pac = getenv("PACMAN_UPDATER_PACMAN");
	if (!pac)
		pac = "pacman";
	char *pa[] = {(char *)pac, "-Syu", "--noconfirm", NULL};
	r = process_run(pac, pa, c, 0, NULL);
	s->last_pacman = time(NULL);
	if (r) {
		s->last_result = r;
		save_state_atomic(state_path, s, (char[128]){0}, 128);
		logmsg(0, "pacman failed with status %d; yay will not run", r);
		systemd_notify_user("Update failed",
				     "System package update failed; see the journal for details.");
		systemd_inhibitor_release(&inhibitor);
		return r;
	}
	save_state_atomic(state_path, s, (char[128]){0}, 128);
	const char *yay = getenv("PACMAN_UPDATER_YAY");
	if (!yay)
		yay = "yay";
	char par[32], *ya[10];
	int j = 0;
	ya[j++] = (char *)yay;
	ya[j++] = "--sudo";
	ya[j++] = (char *)PACMAN_UPDATER_SUDO;
	ya[j++] = "--sudoflags";
	ya[j++] = (char *)PACMAN_UPDATER_SUDO_WRAPPER;
	/* Repository packages were already updated by the root pacman stage. */
	ya[j++] = "-Sua";
	ya[j++] = "--noconfirm";
	if (c->parallelism) {
		snprintf(par, sizeof par, "-j%d", c->parallelism);
		ya[j++] = "--mflags";
		ya[j++] = par;
	}
	ya[j] = NULL;
	r = process_run(yay, ya, c, 1, NULL);
	s->last_yay = time(NULL);
	s->last_result = r;
	if (r)
		logmsg(0, "yay failed with status %d", r);
	if (!r) {
		r = custom_stage_run(c);
		s->last_custom = time(NULL);
		s->last_result = r;
		if (r)
			logmsg(0,
			       "custom PKGBUILD stage failed with status %d",
			       r);
	}
	if (!r) {
		s->last_success = time(NULL);
		logmsg(1, "update completed successfully");
		systemd_notify_user(
		    "Update finished",
		    "System and user package updates completed successfully.");
	} else {
		systemd_notify_user(
		    "Update failed",
		    "One or more package update stages failed; see the journal for details.");
	}
	systemd_inhibitor_release(&inhibitor);
	return r;
}

int update_self_test(config_t *c)
{
	char *id[] = {"id", "-u", "-n", NULL};
	char *pacman_version[] = {"pacman", "--version", NULL};
	char *yay_version[] = {"yay", "--version", NULL};
	char *makepkg_version[] = {"makepkg", "--version", NULL};
	char *git_version[] = {"git", "--version", NULL};
	int failures = 0;

	if (geteuid() != 0) {
		fprintf(
		    stderr,
		    "self-test must be run as root, for example with sudo\n");
		return 2;
	}
	printf("self-test: configuration validation passed\n");
	printf("self-test: dropping privileges to %s:\n", c->aur_user);
	if (process_run("id", id, c, 1, NULL)) {
		fprintf(stderr, "self-test: cannot execute as aur_user\n");
		failures++;
	}
	if (process_run("pacman", pacman_version, c, 0, NULL)) {
		fprintf(stderr, "self-test: pacman --version failed\n");
		failures++;
	}
	if (process_run("yay", yay_version, c, 1, NULL)) {
		fprintf(stderr,
			"self-test: yay --version failed as aur_user\n");
		failures++;
	}
	if (process_run("makepkg", makepkg_version, c, 1, NULL)) {
		fprintf(stderr,
			"self-test: makepkg --version failed as aur_user\n");
		failures++;
	}
	if (process_run("git", git_version, c, 1, NULL)) {
		fprintf(stderr,
			"self-test: git --version failed as aur_user\n");
		failures++;
	}
	if (failures) {
		fprintf(stderr,
			"self-test: %d check(s) failed; no package operation "
			"was run\n",
			failures);
		return 1;
	}
	printf("self-test: all execution checks passed\n");
	return 0;
}


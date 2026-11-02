#define _GNU_SOURCE

#include "systemd.h"
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <systemd/sd-login.h>
#include <sys/wait.h>
#include <unistd.h>

void systemd_notify_user(const char *summary, const char *body)
{
	char **sessions = NULL;
	int session_count = sd_get_sessions(&sessions);

	if (session_count < 0)
		return;
	for (int i = 0; i < session_count; i++) {
		uid_t uid;
		struct passwd *pw;
		pid_t pid;
		int status;
		char runtime_dir[64];
		char bus_address[128];
		char *args[] = {"/usr/bin/notify-send",
				"--app-name=pacman-updater",
				"--icon=system-software-update",
				"--urgency=low",
				"--expire-time=5000",
				(char *)summary,
				(char *)body,
				NULL};

		if (sd_session_is_active(sessions[i]) <= 0
		    || sd_session_get_uid(sessions[i], &uid) < 0)
			continue;
		pw = getpwuid(uid);
		if (!pw || !pw->pw_dir)
			continue;
		if (snprintf(runtime_dir,
			     sizeof(runtime_dir),
			     "/run/user/%lu",
			     (unsigned long)uid)
			>= (int)sizeof(runtime_dir)
		    || snprintf(bus_address,
				 sizeof(bus_address),
				 "unix:path=%s/bus",
				 runtime_dir)
			       >= (int)sizeof(bus_address))
			continue;
		pid = fork();
		if (pid < 0)
			continue;
		if (!pid) {
			if (initgroups(pw->pw_name, pw->pw_gid)
			    || setgid(pw->pw_gid) || setuid(uid)
			    || setenv("HOME", pw->pw_dir, 1)
			    || setenv("USER", pw->pw_name, 1)
			    || setenv("LOGNAME", pw->pw_name, 1)
			    || setenv("XDG_RUNTIME_DIR", runtime_dir, 1)
			    || setenv("DBUS_SESSION_BUS_ADDRESS", bus_address, 1))
				_exit(125);
			if (chdir(pw->pw_dir))
				_exit(126);
			execv(args[0], args);
			_exit(127);
		}
		if (waitpid(pid, &status, 0) >= 0)
			break;
	}
	for (int i = 0; i < session_count; i++)
		free(sessions[i]);
	free(sessions);
}

int systemd_next_timer_invocation(time_t *when)
{
	sd_bus *bus = NULL;
	sd_bus_message *reply = NULL;
	const char *unit_path;
	uint64_t usec;
	int r;

	r = sd_bus_open_system(&bus);
	if (r < 0)
		return -1;
	r = sd_bus_call_method(bus,
			       "org.freedesktop.systemd1",
			       "/org/freedesktop/systemd1",
			       "org.freedesktop.systemd1.Manager",
			       "GetUnit",
			       NULL,
			       &reply,
			       "s",
			       "pacman-updater.timer");
	if (r < 0 || sd_bus_message_read(reply, "o", &unit_path) < 0)
		goto fail;
	r = sd_bus_get_property_trivial(bus,
					"org.freedesktop.systemd1",
					unit_path,
					"org.freedesktop.systemd1.Timer",
					"NextElapseUSecRealtime",
					NULL,
					't',
					&usec);
	if (r < 0 || usec == UINT64_MAX)
		goto fail;
	*when = (time_t)(usec / 1000000ULL);
	sd_bus_message_unref(reply);
	sd_bus_unref(bus);
	return 0;

fail:
	sd_bus_message_unref(reply);
	sd_bus_unref(bus);
	return -1;
}

int systemd_inhibit(systemd_inhibitor_t *inhibitor)
{
	sd_bus_message *reply = NULL;
	int r;

	inhibitor->bus = NULL;
	inhibitor->fd = -1;
	r = sd_bus_open_system(&inhibitor->bus);
	if (r < 0)
		return r;
	r = sd_bus_call_method(
	    inhibitor->bus,
	    "org.freedesktop.login1",
	    "/org/freedesktop/login1",
	    "org.freedesktop.login1.Manager",
	    "Inhibit",
	    NULL,
	    &reply,
	    "ssss",
	    "shutdown:sleep",
	    "pacman-updater",
	    "System package update in progress; interruption may leave "
	    "packages incompletely installed.",
	    "block");
	if (r < 0)
		goto fail;
	r = sd_bus_message_read(reply, "h", &inhibitor->fd);
	if (r < 0)
		goto fail;
	sd_bus_message_unref(reply);
	return 0;

fail:
	sd_bus_message_unref(reply);
	systemd_inhibitor_release(inhibitor);
	return r < 0 ? r : -1;
}

void systemd_inhibitor_release(systemd_inhibitor_t *inhibitor)
{
	if (inhibitor->fd >= 0) {
		close(inhibitor->fd);
		inhibitor->fd = -1;
	}
	if (inhibitor->bus) {
		sd_bus_unref(inhibitor->bus);
		inhibitor->bus = NULL;
	}
}

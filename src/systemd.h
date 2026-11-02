#ifndef PACMAN_UPDATER_SYSTEMD_H
#define PACMAN_UPDATER_SYSTEMD_H

#include <systemd/sd-bus.h>
#include <time.h>

typedef struct {
	sd_bus *bus;
	int fd;
} systemd_inhibitor_t;

void systemd_notify_user(const char *summary, const char *body);
int systemd_next_timer_invocation(time_t *when);
int systemd_inhibit(systemd_inhibitor_t *inhibitor);
void systemd_inhibitor_release(systemd_inhibitor_t *inhibitor);

#endif

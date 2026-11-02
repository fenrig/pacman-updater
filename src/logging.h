#ifndef PACMAN_UPDATER_LOGGING_H
#define PACMAN_UPDATER_LOGGING_H

void log_set_verbosity(int verbosity);
void logmsg(int level, const char *fmt, ...);

#endif

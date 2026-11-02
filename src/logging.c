#include "logging.h"
#include <stdarg.h>
#include <stdio.h>

static int verbosity = 1;

void log_set_verbosity(int value)
{
	verbosity = value;
}

void logmsg(int level, const char *fmt, ...)
{
	if (level > verbosity)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(level ? stdout : stderr, fmt, ap);
	fputc('\n', level ? stdout : stderr);
	va_end(ap);
}

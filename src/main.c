#define _GNU_SOURCE
#include "conditions.h"
#include "core.h"
#include <systemd/sd-bus.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <systemd/sd-login.h>

static int verbose = 1;
static char state_file[512];

#define PACMAN_UPDATER_SUDO                                                    \
	"/usr/lib/pacman-updater/pacman-updater-pacman-wrapper"

static void logmsg(int level, const char *fmt, ...)
{
	if (level > verbose)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(level ? stdout : stderr, fmt, ap);
	fputc('\n', level ? stdout : stderr);
	va_end(ap);
}

static void notify_user(const char *summary, const char *body)
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

static int inhibit(sd_bus **bp, int *fdp)
{
	int r = sd_bus_open_system(bp);
	if (r < 0)
		return r;
	sd_bus_message *reply = NULL;
	r = sd_bus_call_method(
	    *bp,
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
	if (r < 0) {
		sd_bus_unref(*bp);
		*bp = NULL;
		return r;
	}
	r = sd_bus_message_read(reply, "h", fdp);
	sd_bus_message_unref(reply);
	if (r < 0) {
		sd_bus_unref(*bp);
		*bp = NULL;
	}
	return r;
}

static int runprog(const char *path,
		   char *const av[],
		   const config_t *c,
		   int yay,
		   const char *cwd)
{
	pid_t p = fork();
	if (p < 0)
		return -1;
	if (!p) {
		const char *default_cwd = "/";

		if (yay) {
			struct passwd *pw = getpwnam(c->aur_user);
			if (!pw)
				_exit(125);
			if (initgroups(pw->pw_name, pw->pw_gid)
			    || setgid(pw->pw_gid) || setuid(pw->pw_uid))
				_exit(125);
			setenv("HOME", pw->pw_dir, 1);
			setenv("USER", pw->pw_name, 1);
			setenv("LOGNAME", pw->pw_name, 1);
			default_cwd = pw->pw_dir;
		}
		if (chdir(cwd ? cwd : default_cwd))
			_exit(126);
		int in = open("/dev/null", O_RDONLY);
		if (in >= 0) {
			dup2(in, STDIN_FILENO);
			close(in);
		}
		execvp(path, av);
		_exit(127);
	}
	int st;
	if (waitpid(p, &st, 0) < 0)
		return -1;
	if (WIFEXITED(st))
		return WEXITSTATUS(st);
	return 128 + WTERMSIG(st);
}

static int run_capture(const char *path,
		       char *const av[],
		       const config_t *c,
		       int yay,
		       const char *cwd,
		       char *output,
		       size_t output_size)
{
	int pipefd[2];
	pid_t pid;
	size_t used = 0;
	int status;

	if (pipe(pipefd))
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (!pid) {
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(126);
		close(pipefd[1]);
		if (chdir(cwd ? cwd : "/"))
			_exit(126);
		if (yay) {
			struct passwd *pw = getpwnam(c->aur_user);

			if (!pw || initgroups(pw->pw_name, pw->pw_gid)
			    || setgid(pw->pw_gid) || setuid(pw->pw_uid))
				_exit(125);
			setenv("HOME", pw->pw_dir, 1);
			setenv("USER", pw->pw_name, 1);
			setenv("LOGNAME", pw->pw_name, 1);
		}
		int input = open("/dev/null", O_RDONLY);
		if (input >= 0) {
			dup2(input, STDIN_FILENO);
			close(input);
		}
		execvp(path, av);
		_exit(127);
	}
	close(pipefd[1]);
	while (used + 1 < output_size) {
		ssize_t count =
		    read(pipefd[0], output + used, output_size - used - 1);

		if (count <= 0)
			break;
		used += (size_t)count;
	}
	output[used] = '\0';
	close(pipefd[0]);
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return 128 + WTERMSIG(status);
}

struct custom_build_context {
	const config_t *config;
	const char *build_root;
	const char *installed;
	int result;
};

static struct custom_build_context *active_custom_context;

static int installed_package(const char *installed, const char *package)
{
	const char *line = installed;
	size_t package_length = strlen(package);

	while (*line) {
		const char *end = strchr(line, '\n');
		const char *separator = strchr(line, ' ');
		size_t line_length = end ? (size_t)(end - line) : strlen(line);

		if (separator && (size_t)(separator - line) == package_length
		    && (size_t)(separator - line) <= line_length
		    && !strncmp(line, package, package_length))
			return 1;
		line = end ? end + 1 : line + line_length;
	}
	return 0;
}

static int source_info_has_installed_package(const char *source_info,
					     const char *installed)
{
	const char *line = source_info;

	while (*line) {
		const char *end = strchr(line, '\n');
		const char *value;
		size_t length = end ? (size_t)(end - line) : strlen(line);

		while (*line == ' ' || *line == '\t')
			line++;
		if (!strncmp(line, "pkgname = ", 10)) {
			value = line + 10;
			if (value < line + length
			    && (size_t)((end ? end : line + strlen(line)) - value)
				   < 256) {
				char package[256];
				size_t package_length =
				    (size_t)((end ? end : line + strlen(line)) - value);

				memcpy(package, value, package_length);
				package[package_length] = '\0';
				if (installed_package(installed, package))
					return 1;
			}
		}
		line = end ? end + 1 : line + strlen(line);
	}
	return 0;
}

static int arch_version_newer(const config_t *config,
			      const char *available,
			      const char *installed)
{
	char result[32];
	char *args[] = {"vercmp", (char *)available, (char *)installed, NULL};
	char *end;
	long comparison;

	if (run_capture(
		"vercmp", args, config, 0, NULL, result, sizeof(result)))
		return -1;
	errno = 0;
	comparison = strtol(result, &end, 10);
	if (errno || end == result)
		return -1;
	return comparison > 0;
}

static int package_list_version(const char *package_list,
				const char *package,
				char *version,
				size_t version_size)
{
	const char *line = package_list;
	size_t package_length = strlen(package);

	while (*line) {
		const char *end = strchr(line, '\n');
		size_t length = end ? (size_t)(end - line) : strlen(line);
		char path[PATH_MAX];
		char *base;
		char *suffix;
		char *arch_separator;
		size_t value_length;

		if (length >= sizeof(path))
			return -1;
		memcpy(path, line, length);
		path[length] = '\0';
		base = strrchr(path, '/');
		base = base ? base + 1 : path;
		if (strncmp(base, package, package_length)
		    || base[package_length] != '-')
			goto next_line;
		suffix = strstr(base, ".pkg.tar");
		if (!suffix || (suffix[8] != '\0' && suffix[8] != '.'))
			goto next_line;
		arch_separator = suffix;
		while (arch_separator > base && arch_separator[-1] != '-')
			arch_separator--;
		if (arch_separator == base)
			return -1;
		/* arch_separator points just after the dash before the arch. */
		value_length =
		    (size_t)(arch_separator - 1 - (base + package_length + 1));
		if (!value_length || value_length >= version_size)
			return -1;
		memcpy(version, base + package_length + 1, value_length);
		version[value_length] = '\0';
		return 1;
	next_line:
		line = end ? end + 1 : line + strlen(line);
	}
	return 0;
}

static int package_list_has_update(const char *package_list,
				   const char *installed,
				   const config_t *config)
{
	const char *line = installed;
	int found_update = 0;

	while (*line) {
		const char *end = strchr(line, '\n');
		char package[256];
		char installed_value[256];
		char available[384];
		size_t length = end ? (size_t)(end - line) : strlen(line);
		size_t installed_length;
		const char *separator = strchr(line, ' ');

		if (!separator || (size_t)(separator - line) >= sizeof(package))
			goto next_line;
		if ((size_t)(separator - line) >= length)
			return -1;
		memcpy(package, line, (size_t)(separator - line));
		package[separator - line] = '\0';
		while (*separator == ' ' || *separator == '\t')
			separator++;
		if (!*separator || (end && separator >= end))
			return -1;
		installed_length = end ? (size_t)(end - separator) : strlen(separator);
		if (!installed_length || installed_length >= sizeof(installed_value))
			return -1;
		memcpy(installed_value, separator, installed_length);
		installed_value[installed_length] = '\0';
		{
			int result = package_list_version(
				package_list, package, available, sizeof(available));

			if (result < 0)
				return -1;
			if (result) {
				logmsg(1,
				       "custom package %s: installed version %s; "
				       "version to compile %s",
				       package,
				       installed_value,
				       available);
				int newer = arch_version_newer(
					config, available, installed_value);

				if (newer < 0)
					return -1;
				logmsg(1,
				       "custom package %s: comparison result: %s",
				       package,
				       newer ? "update available" : "already current");
				if (newer)
					found_update = 1;
			}
		}
	next_line:
		line = end ? end + 1 : line + strlen(line);
	}
	return found_update;
}

static int custom_build_callback(const char *path,
				 const struct stat *sb,
				 int type,
				 struct FTW *ftwbuf)
{
	struct custom_build_context *context = active_custom_context;
	char package_list[65536];
	char source_info[262144];
	char source_directory[PATH_MAX];
	char build_directory[PATH_MAX];
	char package_paths[64][PATH_MAX];
	char *prepare[] = {
	    "makepkg", "--cleanbuild", "--nobuild", "--nodeps", NULL};
	char *makepkg_info[] = {"makepkg", "--printsrcinfo", NULL};
	char *makepkg_list[] = {"makepkg", "--packagelist", NULL};
	char *build[] = {"makepkg", "--syncdeps", "--noconfirm", NULL};
	char *copy[] = {"cp", "-a", "--", NULL, NULL, NULL};
	const char *pacman;
	char *install[65];
	char *line;
	int count = 0;
	int status;

	(void)sb;
	(void)ftwbuf;
	if (type != FTW_F || strlen(path) < 8
	    || strcmp(path + strlen(path) - 8, "PKGBUILD"))
		return 0;
	if (!context)
		return 1;
	strncpy(source_directory, path, sizeof(source_directory) - 1);
	source_directory[sizeof(source_directory) - 1] = '\0';
	line = strrchr(source_directory, '/');
	if (!line)
		return 0;
	*line = '\0';
	status = run_capture("makepkg",
			     makepkg_info,
			     context->config,
			     1,
			     source_directory,
			     source_info,
			     sizeof(source_info));
	if (status) {
		logmsg(0,
		       "cannot inspect %s (makepkg status %d)",
		       path,
		       status);
		context->result = status;
		return 1;
	}
	status = source_info_has_installed_package(
	    source_info, context->installed);
	if (status < 0) {
		logmsg(0, "cannot identify installed packages for %s", path);
		context->result = EINVAL;
		return 1;
	}
	if (!status) {
		logmsg(1,
		       "skipping %s; no output package is installed",
		       path);
		return 0;
	}
	/*
	 * VCS PKGBUILDs may calculate pkgver() from the checked-out source.
	 * Prepare the sources first so the following --packagelist observes
	 * the current pkgver() result rather than only the literal pkgver in
	 * PKGBUILD.
	 */
	status = runprog("makepkg", prepare, context->config, 1, source_directory);
	if (status) {
		logmsg(0,
		       "cannot prepare sources for %s (makepkg status %d)",
		       path,
		       status);
		context->result = status;
		return 1;
	}
	status = run_capture("makepkg",
			     makepkg_list,
			     context->config,
			     1,
			     source_directory,
			     package_list,
			     sizeof(package_list));
	if (status) {
		logmsg(
		    0, "cannot inspect %s (makepkg status %d)", path, status);
		context->result = status;
		return 1;
	}
	status = package_list_has_update(
	    package_list, context->installed, context->config);
	if (status < 0) {
		logmsg(0, "cannot compare package version for %s", path);
		context->result = EINVAL;
		return 1;
	}
	if (!status) {
		logmsg(1, "skipping %s; installed package is up to date", path);
		return 0;
	}
	line = strrchr(source_directory, '/');
	if (!line || !line[1]) {
		context->result = EINVAL;
		return 1;
	}
	if (snprintf(build_directory,
		     sizeof(build_directory),
		     "%s/%s",
		     context->build_root,
		     line + 1)
	    >= (int)sizeof(build_directory)) {
		context->result = ENAMETOOLONG;
		return 1;
	}
	copy[3] = source_directory;
	copy[4] = (char *)context->build_root;
	status = runprog("cp", copy, context->config, 1, NULL);
	if (status) {
		logmsg(0, "cannot copy custom recipe %s", source_directory);
		context->result = status;
		return 1;
	}
	logmsg(1, "building installed package recipe %s", build_directory);
	status = runprog("makepkg", build, context->config, 1, build_directory);
	if (status) {
		logmsg(0,
		       "custom PKGBUILD failed in %s with status %d",
		       build_directory,
		       status);
		context->result = status;
		return 1;
	}
	status = run_capture("makepkg",
			     makepkg_list,
			     context->config,
			     1,
			     build_directory,
			     package_list,
			     sizeof(package_list));
	if (status) {
		logmsg(0, "cannot list custom packages in %s", build_directory);
		context->result = status;
		return 1;
	}
	line = strtok(package_list, "\n");
	while (line) {
		if (*line) {
			if (count == 64) {
				logmsg(0,
				       "too many packages from %s",
				       build_directory);
				context->result = E2BIG;
				return 1;
			}
			if (line[0] == '/') {
				if (strlen(line)
				    >= sizeof(package_paths[count])) {
					context->result = ENAMETOOLONG;
					return 1;
				}
				strcpy(package_paths[count], line);
			} else {
				size_t directory_length =
				    strlen(build_directory);
				size_t package_length = strlen(line);

				if (directory_length + 1 + package_length
				    >= sizeof(package_paths[count])) {
					context->result = ENAMETOOLONG;
					return 1;
				}
				memcpy(package_paths[count],
				       build_directory,
				       directory_length);
				package_paths[count][directory_length] = '/';
				memcpy(package_paths[count] + directory_length
					   + 1,
				       line,
				       package_length + 1);
			}
			count++;
		}
		line = strtok(NULL, "\n");
	}
	if (!count) {
		logmsg(
		    0, "makepkg produced no packages in %s", build_directory);
		context->result = ENOENT;
		return 1;
	}
	pacman = getenv("PACMAN_UPDATER_PACMAN");
	if (!pacman)
		pacman = "pacman";
	install[0] = (char *)pacman;
	install[1] = "-U";
	install[2] = "--noconfirm";
	for (int i = 0; i < count; i++)
		install[i + 3] = package_paths[i];
	install[count + 3] = NULL;
	status = runprog(pacman, install, context->config, 0, NULL);
	if (status) {
		logmsg(0,
		       "custom package installation failed with status %d",
		       status);
		context->result = status;
		return 1;
	}
	return 0;
}

static int custom_tmp_dir(const config_t *c, char *directory, size_t size)
{
	struct passwd *pw = getpwnam(c->aur_user);
	char template[] = "/tmp/pacman-updater-XXXXXX";
	char *result;

	if (!pw)
		return -1;
	result = mkdtemp(template);
	if (!result)
		return -1;
	if (strlen(result) >= size) {
		rmdir(result);
		return -1;
	}
	strcpy(directory, result);
	if (chown(directory, pw->pw_uid, pw->pw_gid)) {
		rmdir(directory);
		return -1;
	}
	return 0;
}

static int custom_build_root(const config_t *c, char *directory, size_t size)
{
	struct passwd *pw = getpwnam(c->aur_user);
	char path[PATH_MAX];
	char *remove[] = {"rm", "-rf", "--", NULL, NULL};

	if (!pw)
		return -1;
	if (snprintf(path, sizeof(path), "%s/.cache", pw->pw_dir)
		>= (int)sizeof(path)
	    || (mkdir(path, 0700) && errno != EEXIST)
	    || chown(path, pw->pw_uid, pw->pw_gid))
		return -1;
	if (snprintf(directory, size, "%s/.cache/pacman-updater", pw->pw_dir)
	    >= (int)size)
		return -1;
	remove[3] = directory;
	if (runprog("/usr/bin/rm", remove, c, 1, NULL))
		return -1;
	if (mkdir(directory, 0700) || chown(directory, pw->pw_uid, pw->pw_gid))
		return -1;
	return 0;
}

static int update_custom_repository(const config_t *c, const char *directory)
{
	char git_directory[PATH_MAX];
	char *clone[] = {
	    "git", "clone", "--depth", "1", NULL, NULL, "--branch", NULL, NULL};
	char *pull[] = {
	    "git", "-C", (char *)directory, "pull", "--ff-only", NULL, NULL};
	char *fetch[] = {"git",
			 "-C",
			 (char *)directory,
			 "fetch",
			 "--depth",
			 "1",
			 "origin",
			 NULL,
			 NULL};
	char *checkout[] = {"git",
			    "-C",
			    (char *)directory,
			    "checkout",
			    "--detach",
			    "FETCH_HEAD",
			    NULL};
	char *git = "git";
	int status;

	if (snprintf(git_directory, sizeof(git_directory), "%s/.git", directory)
	    >= (int)sizeof(git_directory))
		return ENAMETOOLONG;
	if (access(git_directory, F_OK)) {
		if (c->custom_repo_ref[0]) {
			clone[4] = "--branch";
			clone[5] = (char *)c->custom_repo_ref;
			clone[6] = (char *)c->custom_repo_url;
			clone[7] = (char *)directory;
		} else {
			clone[4] = (char *)c->custom_repo_url;
			clone[5] = (char *)directory;
			clone[6] = NULL;
		}
		return runprog(git, clone, c, 1, NULL);
	}
	if (c->custom_repo_ref[0]) {
		fetch[7] = (char *)c->custom_repo_ref;
		status = runprog(git, fetch, c, 1, NULL);
		if (status)
			return status;
		return runprog(git, checkout, c, 1, NULL);
	}
	pull[5] = "origin";
	return runprog(git, pull, c, 1, NULL);
}

static int run_custom_stage(const config_t *c)
{
	char temporary_directory[PATH_MAX];
	char repository_directory[PATH_MAX];
	char build_root[PATH_MAX] = "";
	char installed[262144];
	const char *pacman;
	char *query[3];
	struct custom_build_context context;
	int build_root_ready = 0;
	int status;

	if (!c->custom_repo_url[0])
		return 0;
	if (custom_tmp_dir(
		c, temporary_directory, sizeof(temporary_directory))) {
		logmsg(0, "cannot prepare temporary custom PKGBUILD directory");
		return EACCES;
	}
	if (snprintf(repository_directory,
		     sizeof(repository_directory),
		     "%s/repository",
		     temporary_directory)
		>= (int)sizeof(repository_directory)
	    || custom_build_root(c, build_root, sizeof(build_root))) {
		status = ENAMETOOLONG;
		goto cleanup;
	}
	build_root_ready = 1;
	status = update_custom_repository(c, repository_directory);
	if (status) {
		logmsg(
		    0, "custom PKGBUILD repository update failed: %d", status);
		goto cleanup;
	}
	pacman = getenv("PACMAN_UPDATER_PACMAN");
	if (!pacman)
		pacman = "pacman";
	query[0] = (char *)pacman;
	query[1] = "-Q";
	query[2] = NULL;
	status = run_capture(
	    pacman, query, c, 0, NULL, installed, sizeof(installed));
	if (status) {
		logmsg(0, "cannot enumerate installed packages: %d", status);
		goto cleanup;
	}
	context.config = c;
	context.build_root = build_root;
	context.installed = installed;
	context.result = 0;
	active_custom_context = &context;
	status =
	    nftw(repository_directory, custom_build_callback, 16, FTW_PHYS);
	active_custom_context = NULL;
	if (status < 0 || context.result) {
		status = context.result ? context.result : errno;
		goto cleanup;
	}
	logmsg(1, "custom PKGBUILD stage completed");
	status = 0;

cleanup:
	{
		char *remove[] = {"rm", "-rf", "--", temporary_directory, NULL};
		char *remove_build[] = {"rm", "-rf", "--", build_root, NULL};

		if (build_root_ready
		    && runprog("/usr/bin/rm", remove_build, c, 1, NULL))
			logmsg(0, "could not remove custom build workspace");

		if (runprog("/usr/bin/rm", remove, c, 0, NULL))
			logmsg(0, "could not remove temporary custom checkout");
	}
	return status;
}

static int run_update(config_t *c, state_t *s)
{
	sd_bus *b = NULL;
	int fd = -1, r = inhibit(&b, &fd);
	if (r < 0) {
		logmsg(0,
		       "cannot acquire sleep/shutdown inhibitor: %s",
		       strerror(-r));
		return 1;
	}
	logmsg(1, "inhibitor acquired; starting package update");
	notify_user("Update started", "System and user package updates are running.");
	const char *pac = getenv("PACMAN_UPDATER_PACMAN");
	if (!pac)
		pac = "pacman";
	char *pa[] = {(char *)pac, "-Syu", "--noconfirm", NULL};
	r = runprog(pac, pa, c, 0, NULL);
	s->last_pacman = time(NULL);
	if (r) {
		s->last_result = r;
		save_state_atomic(state_file, s, (char[128]){0}, 128);
		logmsg(0, "pacman failed with status %d; yay will not run", r);
		notify_user("Update failed", "System package update failed; see the journal for details.");
		close(fd);
		sd_bus_unref(b);
		return r;
	}
	save_state_atomic(state_file, s, (char[128]){0}, 128);
	const char *yay = getenv("PACMAN_UPDATER_YAY");
	if (!yay)
		yay = "yay";
	char par[32], *ya[8];
	int j = 0;
	ya[j++] = (char *)yay;
	ya[j++] = "--sudo";
	ya[j++] = (char *)PACMAN_UPDATER_SUDO;
	/* Repository packages were already updated by the root pacman stage. */
	ya[j++] = "-Sua";
	ya[j++] = "--noconfirm";
	if (c->parallelism) {
		snprintf(par, sizeof par, "-j%d", c->parallelism);
		ya[j++] = "--mflags";
		ya[j++] = par;
	}
	ya[j] = NULL;
	r = runprog(yay, ya, c, 1, NULL);
	s->last_yay = time(NULL);
	s->last_result = r;
	if (r)
		logmsg(0, "yay failed with status %d", r);
	if (!r) {
		r = run_custom_stage(c);
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
		notify_user("Update finished", "System and user package updates completed successfully.");
	} else {
		notify_user("Update failed", "One or more package update stages failed; see the journal for details.");
	}
	close(fd);
	sd_bus_unref(b);
	return r;
}

static int self_test(config_t *c)
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
	if (runprog("id", id, c, 1, NULL)) {
		fprintf(stderr, "self-test: cannot execute as aur_user\n");
		failures++;
	}
	if (runprog("pacman", pacman_version, c, 0, NULL)) {
		fprintf(stderr, "self-test: pacman --version failed\n");
		failures++;
	}
	if (runprog("yay", yay_version, c, 1, NULL)) {
		fprintf(stderr,
			"self-test: yay --version failed as aur_user\n");
		failures++;
	}
	if (runprog("makepkg", makepkg_version, c, 1, NULL)) {
		fprintf(stderr,
			"self-test: makepkg --version failed as aur_user\n");
		failures++;
	}
	if (runprog("git", git_version, c, 1, NULL)) {
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
		return self_test(&c);
	if (force_update && geteuid() != 0) {
		fprintf(stderr,
			"--force-update must be run as root, for example with "
			"sudo\n");
		return 2;
	}
	verbose = c.verbosity;
	if (mkdir(c.state_dir, 0700) && errno != EEXIST) {
		fprintf(stderr,
			"cannot create state directory: %s\n",
			strerror(errno));
		return 2;
	}
	char lockp[512], statep[512];
	snprintf(lockp, sizeof lockp, "%s/worker.lock", c.state_dir);
	snprintf(statep, sizeof statep, "%s/state", c.state_dir);
	snprintf(state_file, sizeof state_file, "%s", statep);
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
	if (!force_update && !due)
		return 0;
	if (force_update) {
		logmsg(0,
		       "WARNING: forcing update; eligibility conditions are "
		       "being bypassed");
		s.last_attempt = time(NULL);
		if (save_state_atomic(statep, &s, e, sizeof e)) {
			logmsg(0, "could not persist forced attempt: %s", e);
			return 1;
		}
		int rc = run_update(&c, &s);
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
		return 0;
	}
	time_t end = midnight();
	cpu_monitor_t cpu_monitor = {0};
	for (;;) {
		char why[256];
		if (time(NULL) >= end)
			return 0;
		if (conditions_check(&c, why, sizeof why, &cpu_monitor, 0)) {
			if (time(NULL) >= end)
				return 0;
			logmsg(1, "eligibility passed: %s", why);
			s.last_attempt = time(NULL);
			save_state_atomic(statep, &s, e, sizeof e);
			int rc = run_update(&c, &s);
			if (save_state_atomic(statep, &s, e, sizeof e))
				logmsg(0, "could not persist state: %s", e);
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

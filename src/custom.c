#define _GNU_SOURCE
#include "custom.h"
#include "logging.h"
#include "process.h"
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

	if (process_capture(
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
	status = process_capture("makepkg",
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
	status = process_run("makepkg", prepare, context->config, 1, source_directory);
	if (status) {
		logmsg(0,
		       "cannot prepare sources for %s (makepkg status %d)",
		       path,
		       status);
		context->result = status;
		return 1;
	}
	status = process_capture("makepkg",
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
	status = process_run("cp", copy, context->config, 1, NULL);
	if (status) {
		logmsg(0, "cannot copy custom recipe %s", source_directory);
		context->result = status;
		return 1;
	}
	logmsg(1, "building installed package recipe %s", build_directory);
	status = process_run("makepkg", build, context->config, 1, build_directory);
	if (status) {
		logmsg(0,
		       "custom PKGBUILD failed in %s with status %d",
		       build_directory,
		       status);
		context->result = status;
		return 1;
	}
	status = process_capture("makepkg",
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
	status = process_run(pacman, install, context->config, 0, NULL);
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
	if (process_run("/usr/bin/rm", remove, c, 1, NULL))
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
		return process_run(git, clone, c, 1, NULL);
	}
	if (c->custom_repo_ref[0]) {
		fetch[7] = (char *)c->custom_repo_ref;
		status = process_run(git, fetch, c, 1, NULL);
		if (status)
			return status;
		return process_run(git, checkout, c, 1, NULL);
	}
	pull[5] = "origin";
	return process_run(git, pull, c, 1, NULL);
}

int custom_stage_run(const config_t *c)
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
	status = process_capture(
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
		    && process_run("/usr/bin/rm", remove_build, c, 1, NULL))
			logmsg(0, "could not remove custom build workspace");

		if (process_run("/usr/bin/rm", remove, c, 0, NULL))
			logmsg(0, "could not remove temporary custom checkout");
	}
	return status;
}


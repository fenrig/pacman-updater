#define _GNU_SOURCE

#include "process.h"
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static void set_aur_environment(const struct passwd *user)
{
	setenv("HOME", user->pw_dir, 1);
	setenv("USER", user->pw_name, 1);
	setenv("LOGNAME", user->pw_name, 1);
}

static int drop_to_aur_user(const config_t *config, const char **home)
{
	struct passwd *user = getpwnam(config->aur_user);

	if (!user
	    || initgroups(user->pw_name, user->pw_gid)
	    || setgid(user->pw_gid) || setuid(user->pw_uid))
		return -1;
	set_aur_environment(user);
	if (home)
		*home = user->pw_dir;
	return 0;
}

static int redirect_stdin(void)
{
	int input = open("/dev/null", O_RDONLY);

	if (input < 0)
		return -1;
	if (dup2(input, STDIN_FILENO) < 0) {
		close(input);
		return -1;
	}
	close(input);
	return 0;
}

int process_run(const char *path,
		char *const argv[],
		const config_t *config,
		int as_aur_user,
		const char *cwd)
{
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (!pid) {
		const char *default_cwd = "/";

		if (as_aur_user) {
			if (drop_to_aur_user(config, &default_cwd))
				_exit(125);
		}
		if (chdir(cwd ? cwd : default_cwd) || redirect_stdin())
			_exit(126);
		execvp(path, argv);
		_exit(127);
	}
	int status;
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return 128 + WTERMSIG(status);
}

int process_capture(const char *path,
			char *const argv[],
			const config_t *config,
			int as_aur_user,
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
		if (chdir(cwd ? cwd : "/") || redirect_stdin())
			_exit(126);
		if (as_aur_user && drop_to_aur_user(config, NULL))
			_exit(125);
		execvp(path, argv);
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

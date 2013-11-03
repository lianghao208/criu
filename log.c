#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <fcntl.h>

#include "compiler.h"
#include "asm/types.h"
#include "util.h"
#include "crtools.h"

#define DEFAULT_LOGFD		STDERR_FILENO

static unsigned int current_loglevel = DEFAULT_LOGLEVEL;

static char buffer[PAGE_SIZE];
static char buf_off = 0;

static struct timeval start;
/*
 * Manual buf len as sprintf will _always_ put '\0' at the
 * and, but we want a "constant" pid to be there on restore
 */
#define TS_BUF_OFF	12

static void timediff(struct timeval *from, struct timeval *to)
{
	to->tv_sec -= from->tv_sec;
	if (to->tv_usec >= from->tv_usec)
		to->tv_usec -= from->tv_usec;
	else {
		to->tv_sec--;
		to->tv_usec += 1000000 - from->tv_usec;
	}
}

static void print_ts(void)
{
	struct timeval t;

	gettimeofday(&t, NULL);
	timediff(&start, &t);
	snprintf(buffer, TS_BUF_OFF,
			"(%02u.%06u)", (unsigned)t.tv_sec, (unsigned)t.tv_usec);
	buffer[TS_BUF_OFF - 1] = ' '; /* kill the '\0' produced by snprintf */
}

int log_get_fd(void)
{
	int fd = get_service_fd(LOG_FD_OFF);

	return fd < 0 ? DEFAULT_LOGFD : fd;
}

int log_init(const char *output)
{
	int new_logfd, dfd, fd;

	gettimeofday(&start, NULL);
	buf_off = TS_BUF_OFF;

	dfd = get_service_fd(LOG_DIR_FD_OFF);
	if (dfd < 0) {
		int tmp;
		tmp = open(".", O_RDONLY);
		if (tmp == -1) {
			pr_perror("Can't open a current directory");
			return -1;
		}

		dfd = install_service_fd(LOG_DIR_FD_OFF, tmp);
		close(tmp);
		if (dfd < 0)
			return -1;
	}

	if (output) {
		new_logfd = openat(dfd, output,
					O_CREAT | O_TRUNC | O_WRONLY | O_APPEND, 0600);
		if (new_logfd < 0) {
			pr_perror("Can't create log file %s", output);
			return -1;
		}
	} else {
		new_logfd = dup(DEFAULT_LOGFD);
		if (new_logfd < 0) {
			pr_perror("Can't dup log file");
			return -1;
		}
	}

	fd = install_service_fd(LOG_FD_OFF, new_logfd);
	close(new_logfd);
	if (fd < 0)
		goto err;

	return 0;

err:
	pr_perror("Log engine failure, can't duplicate descriptor");
	return -1;
}

int log_init_by_pid(void)
{
	char path[PATH_MAX];

	/*
	 * reset buf_off as this fn is called on each fork while
	 * restoring process tree
	 */

	buf_off = TS_BUF_OFF;

	if (!opts.log_file_per_pid) {
		buf_off += snprintf(buffer + buf_off, PAGE_SIZE - buf_off, "%6d: ", getpid());
		return 0;
	}

	if (!opts.output)
		return 0;

	snprintf(path, PATH_MAX, "%s.%d", opts.output, getpid());

	return log_init(path);
}

void log_fini(void)
{
	close_service_fd(LOG_FD_OFF);
	log_closedir();
}

void log_closedir(void)
{
	close_service_fd(LOG_DIR_FD_OFF);
}

void log_set_loglevel(unsigned int level)
{
	if (!level)
		current_loglevel = DEFAULT_LOGLEVEL;
	else
		current_loglevel = level;
}

unsigned int log_get_loglevel(void)
{
	return current_loglevel;
}

static void __print_on_level(unsigned int loglevel, const char *format, va_list params)
{
	int fd, size, ret, off;

	if (unlikely(loglevel == LOG_MSG)) {
		fd = STDOUT_FILENO;
		off = buf_off;
	} else {
		if (loglevel > current_loglevel)
			return;
		fd = log_get_fd();
		print_ts();
		off = 0;
	}

	size  = vsnprintf(buffer + buf_off, PAGE_SIZE - buf_off, format, params);
	size += buf_off;

	while (off < size) {
		ret = write(fd, buffer + off, size - off);
		if (ret <= 0)
			break;
		off += ret;
	}
}

void print_on_level(unsigned int loglevel, const char *format, ...)
{
	va_list params;

	va_start(params, format);
	__print_on_level(loglevel, format, params);
	va_end(params);
}

int write_pidfile(char *pfname, int pid)
{
	int fd;

	fd = open(pfname, O_WRONLY | O_TRUNC | O_CREAT, 0600);
	if (fd == -1) {
		pr_perror("Can't open %s", pfname);
		return -1;
	}

	dprintf(fd, "%d", pid);
	close(fd);
	return 0;
}

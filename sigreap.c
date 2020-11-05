#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#define LOG_TAG "SigReap"
#define LOG_FLAGS LOG_PID | LOG_PERROR
#define LOG_FACILITY LOG_LOCAL4

#ifdef OUTPUT_TO_SYSLOG
#	define err(...) syslog(LOG_ERR, __VA_ARGS__)
#	define info(...) syslog(LOG_INFO, __VA_ARGS__)
#else
#	define err(...) dprintf(STDERR_FILENO, __VA_ARGS__)
#	define info(...) dprintf(STDOUT_FILENO, __VA_ARGS__)
#endif

#define NMAXPIDS 64

static pid_t childpids[NMAXPIDS+1];
static int lastexitcode;

__attribute__((noreturn))
static void die(const char *what) {
	int e = errno;
#ifdef OUTPUT_TO_SYSLOG
	syslog(LOG_ERR, "%s", what);
#else
	perror(what);
#endif
	exit(-e);
}

static inline void block() {
	sigset_t all;
	sigfillset(&all);
	if (0 > sigprocmask(SIG_BLOCK, &all, NULL)) {
		die("action=exit reason=\"sigprocmask(SIG_BLOCK)\"");
	}
}

static inline void unblock() {
	sigset_t all;
	sigfillset(&all);
	if (0 > sigprocmask(SIG_UNBLOCK, &all, NULL)) {
		die("action=exit reason=\"sigprocmask(SIG_UNBLOCK)\"");
	}
}

static void reap() {
	int wstatus;
	pid_t wpid;

	do {
		wpid = waitpid(-1, &wstatus, WNOHANG | WUNTRACED | WCONTINUED);
		if (0 < wpid) {
			if (WIFSTOPPED(wstatus) || WIFCONTINUED(wstatus)) {
				info("action=reap childPid=%ld reason=\"stop/cont\"\n", (long) wpid);
			} else if (WIFEXITED(wstatus) || WIFSIGNALED(wstatus)) {
				lastexitcode = WEXITSTATUS(wstatus);
				info("action=reap childPid=%ld reason=\"exit/kill\" exitCode=%d\n", (long) wpid, lastexitcode);
			}
		}
	} while (0 < wpid);
}

static bool active(const char *procfs) {
	int i = 0, cfd;
	ssize_t siz;
	char children[NMAXPIDS*32] = "";
	char *ptr = children, *end = ptr, *stop;

	block();
	reap();

	cfd = open(procfs, O_RDONLY);
	if (0 > cfd) {
		die("action=exit reason=\"open(procfs)\"");
	}
	siz = read(cfd, &children, sizeof(children)-1);
	close(cfd);

	if (1 > siz) {
		err("action=unblock reason=\"no children\"\n");
		unblock();
		return false;
	}

	stop = ptr + siz - 1;
	while (end < stop && i < NMAXPIDS) {
		childpids[i++] = strtoul(ptr, &end, 10);
		ptr = end;
	}
	childpids[i] = 0;

	info("childNumber=%d children=\"%s\"\n", i, children);
	if (i >= NMAXPIDS) {
		err("action=ignoreChildren reasion=\"too many children (>NMAXPIDS)!\"\n");
	}
	unblock();

	return true;
}

static void handler(int signo) {
	info("action=handleSignal signal=%d\n", signo); /* yes, this is not async-signal-safe */
	if (signo != SIGCHLD) {
		for (int i = 0; i < NMAXPIDS && childpids[i]; ++i) {
			if (0 > kill(childpids[i], signo)) {
				perror("kill(childpid, signo)");
			}
		}
	}
}

static void setup(pid_t child) {
	struct sigaction action = {
		.sa_handler = handler,
		.sa_flags = SA_NOCLDSTOP | SA_RESTART
	};

	childpids[0] = child;

	for (int signo = 1; signo < NSIG; ++signo) {
		if (0 > sigaction(signo, &action, NULL)) {
			if (errno != EINVAL) {
				perror("sigaction(*)");
			}
		}
	}

	if (0 > prctl(PR_SET_CHILD_SUBREAPER, 1)) {
		die("action=exit reason=\"prctl(SET_CHILD_SUBREAPER)\"");
	}
}

static void loop() {
	char procfs[PATH_MAX+1] = "";
	int siz = snprintf(procfs, PATH_MAX,
			"/proc/%1$u/task/%1$u/children", (unsigned) getpid());

	switch (siz) {
	case -1:
	case PATH_MAX:
		die("action=exit reason=\"snprintf(/proc/.../children)\"");
	}

	while (active(procfs)) {
		pause();
		errno = 0;
	}
}

int main(int argc, char *argv[]) {
	pid_t child;

	if (argc <= 1) {
		err("Usage: %s <program> [<args> ...]\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	openlog(LOG_TAG, LOG_FLAGS, LOG_FACILITY);

	block();

	switch ((child = fork())) {
	case 0:
		unblock();
		execvp(argv[1], &argv[1]); /* fallthrough */
	case -1:
		die("action=exit reason=\"fork/exec\"");

	default:
		setup(child);
		unblock();
		loop();
		errno = lastexitcode;
		die("action=exit reason=done");
	}
}

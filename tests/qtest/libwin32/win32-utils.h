#ifndef _QTEST_WIN32_UTILS_H
#define _QTEST_WIN32_UTILS_H

#include <winsock2.h>
#include <winsock.h>
#include <afunix.h>
#include <stdint.h>
#include <sys/types.h>

/* macros for analyzing process status */

#define WIFSIGNALED(status) ((int)((status) & 0xFF00) != 0)
#define WIFSTOPPED(status)  ((int)((status) & 0xFF0000) != 0)
#define WTERMSIG(status)    ((int)(((status) >> 8) & 0xFF))
#define WSTOPSIG(status)    ((int)(((status) >> 16) & 0xFF))

#define SA_RESETHAND    0x0010      /* Reset the handler */
#define WNOHANG         0x01        /* do not hang if no status is */
#define SIGSTOP         17          /* sendable stop signal not from tty */

typedef _sigset_t       sigset_t;

typedef struct _Siginfo {
    int             si_signo;   /* Signal number. */
    int             si_code;    /* Signal code. */
    int             si_errno;   /* errno value associated with this signal */
    int             si_status;  /* Exit value or signal. */
    void            *si_addr;   /* Address of faulting instruction. */
    pid_t           si_pid;     /* Sending process ID. */
} siginfo_t;

struct sigaction {
    union {
        void        (*__sa_handler)(int);
        void        (*__sa_sigaction)(int, siginfo_t *, void *);
    } sa_u;
#define sa_handler      sa_u.__sa_handler
#define sa_sigaction    sa_u.__sa_sigaction
    sigset_t     sa_mask;
    int          sa_flags;
};

char *mkdtemp(char *template);
int mkfifo(const char *path, mode_t mode);
pid_t waitpid(pid_t pid, int *stat_loc, int options);
int kill(pid_t pid, int sig);
int getuid(void);

char *strsignal(int signum);
int sigemptyset(sigset_t *set);
int sigaction(int sig, const struct sigaction *restrict act,
            struct sigaction *restrict oact);

int setenv(const char *envname, const char *envval, int overwrite);

pid_t qemu_process_create(char *cmd);
void str_replace(GString *haystack, const char *find,
                 const char *replace_fmt, ...);
#endif /* _QTEST_WIN32_UTILS_H */

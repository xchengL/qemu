#include "qemu/osdep.h"
#include <pthread.h>
#include <signal.h>
#include <windows.h>
#include "win32-utils.h"


char *mkdtemp(char *template)
{
    return g_mkdtemp(template);
}

pid_t waitpid(pid_t pid, int *stat_loc, int options)
{
    int ret = WaitForSingleObject((HANDLE)pid, INFINITE);
    if (ret == 0) {
        return pid;
    } else {
        fprintf(stderr, "%s : %s\n", __func__, strerror(GetLastError()));
        return -1;
    }
}

int kill(pid_t pid, int sig)
{
    if (TerminateProcess((HANDLE)pid, sig) != 0) {
        return 0;
    } else {
        fprintf(stderr, "%s : %s\n", __func__, strerror(GetLastError()));
        return -1;
    }
}

int getuid(void)
{
    /* Windows user is always a superuser */
    return 0;
}

char *strsignal(int signum)
{
    fprintf(stderr, "%s is not implemented\n", __func__);
    errno = ENOSYS;
    return NULL;
}

int sigemptyset(sigset_t *set)
{
    *set = 0;
    return 0;
}

int sigaction(int sig,
              const struct sigaction *restrict act,
              struct sigaction *restrict oact)
{
    int ret = 0;
    void (*phandler)(int);

    phandler = signal(sig, act->sa_handler);

    if (oact != NULL) {
        oact->sa_handler = phandler;
    }

    if (phandler == SIG_ERR) {
        ret = -1;
    }
    return ret;
}

int setenv(const char *envname, const char *envval, int overwrite)
{
    fprintf(stderr, "%s is not implemented\n", __func__);
    errno = ENOSYS;
    return -1;
}

int mkfifo(const char *path, mode_t mode)
{
    fprintf(stderr, "%s is not implemented\n", __func__);
    errno = ENOSYS;
    return -1;
}

pid_t qemu_process_create(char *cmd)
{
    bool ret = 0;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    ret = CreateProcess(NULL,  /* module name */
                        cmd,   /* command line */
                        NULL,  /* process handle not inheritable */
                        NULL,  /* thread handle not inheritable */
                        FALSE, /* set handle inheritance to FALSE */
                        0,     /* No creation flags */
                        NULL,  /* use parent's environment block */
                        NULL,  /* use parent's starting directory */
                        &si,   /* pointer to STARTUPINFO structure */
                        &pi    /* pointer to PROCESS_INFORMATION structure */
                        );
    if (ret == 0) {
        fprintf(stderr, "%s : %s\n", __func__, strerror(GetLastError()));
        return -1;
    }
    return (pid_t)pi.hProcess;
}

#if !GLIB_CHECK_VERSION(2, 68, 0)
/*
 * This implementation refers
 * https://github.com/GNOME/glib/blob/main/glib/gstring.c#L968
 */
static guint g_string_replace_compat(GString *string, const gchar *find,
                                     const gchar *replace, guint limit)
{
  gsize f_len, r_len, pos;
  gchar *cur, *next;
  guint n = 0;

  g_return_val_if_fail(string != NULL, 0);
  g_return_val_if_fail(find != NULL, 0);
  g_return_val_if_fail(replace != NULL, 0);

  f_len = strlen(find);
  r_len = strlen(replace);
  cur = string->str;

    while ((next = strstr(cur, find)) != NULL) {
        pos = next - string->str;
        g_string_erase(string, pos, f_len);
        g_string_insert(string, pos, replace);
        cur = string->str + pos + r_len;
        n++;
        if (f_len == 0) {
            if (cur[0] == '\0') {
                break;
            } else {
                cur++;
            }
        }
        if (n == limit) {
            break;
        }
    }
  return n;
}
#endif /* !GLIB_CHECK_VERSION(2, 68, 0) */

void str_replace(GString *haystack, const char *find,
                 const char *replace_fmt, ...)
{
    g_autofree char *replace = NULL, *s = NULL;
    va_list argp;

    va_start(argp, replace_fmt);
    replace = g_strdup_vprintf(replace_fmt, argp);
    va_end(argp);
#if GLIB_CHECK_VERSION(2, 68, 0)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    g_string_replace(haystack, find, replace, 0);
#pragma GCC diagnostic pop
#else
    g_string_replace_compat(haystack, find, replace, 0);
#endif
}

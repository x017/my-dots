/* Runs a helper and captures its output without a shell, so names and
 * addresses can never become commands. Shared by the dashboard, the standalone
 * bar and the application launcher. */
#ifndef DWM_COMMAND_RUN_H
#define DWM_COMMAND_RUN_H
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_OUTPUT 32768

#ifndef DASHBOARD_STOPPING
static bool dashboard_stopping(void) { return false; }
#define DASHBOARD_STOPPING() dashboard_stopping()
#endif

/* No shell: names and addresses cannot become commands. Read output with a
 * timeout, and send secrets only through a pipe, never in argv or a file. */
static int run_command(char *const argv[], const char *input, char *out, size_t cap, int timeout_ms) {
    int result = -1, fds[2], in[2], status = 0, flags, done = 0;
    size_t used = 0;
    pid_t pid, waited;
    struct timespec start, now;
    if (cap) out[0] = '\0';
    if (pipe(fds) < 0) return -1;
    if (pipe(in) < 0) { close(fds[0]); close(fds[1]); return -1; }
    pid = fork();
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        dup2(in[0], STDIN_FILENO); dup2(fds[1], STDOUT_FILENO); dup2(fds[1], STDERR_FILENO);
        close(fds[0]); close(fds[1]); close(in[0]); close(in[1]);
        execvp(argv[0], argv); _exit(127);
    }
    close(fds[1]); close(in[0]);
    if (pid < 0) { close(fds[0]); close(in[1]); return -1; }
    if (input && *input) {
        size_t n = strlen(input), written = 0;
        while (written < n) {
            ssize_t w = write(in[1], input + written, n - written);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) break;
            written += (size_t)w;
        }
    }
    close(in[1]);
    flags = fcntl(fds[0], F_GETFL, 0); if (flags >= 0) fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!done) {
        char buf[2048]; ssize_t n;
        struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
        poll(&pfd, 1, 100);
        while ((n = read(fds[0], buf, sizeof buf)) > 0) {
            size_t take = cap && used < cap - 1 ? (size_t)n : 0;
            if (take > cap - 1 - used) take = cap - 1 - used;
            if (take) { memcpy(out + used, buf, take); used += take; out[used] = '\0'; }
        }
        waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) { done = 1; result = WIFEXITED(status) ? WEXITSTATUS(status) : -1; }
        else if (waited < 0 && errno != EINTR) done = 1;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!done && (DASHBOARD_STOPPING() || (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000 >= timeout_ms)) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) ;
            done = 1;
        }
    }
    /* The child can exit between the last read and waitpid. Drain bytes it
     * wrote before exit so short-lived commands do not lose their output. */
    for (;;) {
        char buf[2048];
        ssize_t n = read(fds[0], buf, sizeof buf);
        size_t take;
        if (n <= 0) break;
        take = cap && used < cap - 1 ? (size_t)n : 0;
        if (take > cap - 1 - used) take = cap - 1 - used;
        if (take) { memcpy(out + used, buf, take); used += take; out[used] = '\0'; }
    }
    close(fds[0]);
    return result;
}

/* nmcli -t escapes colons and backslashes; decode each field in place. */

#endif

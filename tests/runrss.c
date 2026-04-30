/*
 * runrss — fork+exec a child, wait, report peak RSS in KB on stderr.
 *
 * Portable across macOS and Linux:
 *   - macOS: getrusage returns ru_maxrss in BYTES
 *   - Linux: getrusage returns ru_maxrss in KILOBYTES
 *
 * We normalise to KB and emit a single line:
 *     RSS_KB=12345
 *     EXIT=0
 *     WALL_MS=42
 *
 * to stderr, after the child has finished. The child's stdout/stderr
 * pass through unchanged so existing parsers still work.
 *
 * Build: cc -O2 -Wall -o tests/runrss tests/runrss.c
 * Use:   tests/runrss ./tardygrada verify-doc /tmp/somedoc.md
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

static long ru_maxrss_to_kb(long ru_maxrss)
{
#if defined(__APPLE__) || defined(__MACH__)
    /* macOS reports bytes */
    return ru_maxrss / 1024;
#else
    /* Linux reports kilobytes */
    return ru_maxrss;
#endif
}

static long now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <command> [args...]\n", argv[0]);
        return 2;
    }

    long t0 = now_ms();

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "runrss: fork failed: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        /* Child: exec the requested command. */
        execvp(argv[1], &argv[1]);
        fprintf(stderr, "runrss: execvp(%s) failed: %s\n",
                argv[1], strerror(errno));
        _exit(127);
    }

    int wstatus = 0;
    if (waitpid(pid, &wstatus, 0) < 0) {
        fprintf(stderr, "runrss: waitpid failed: %s\n", strerror(errno));
        return 1;
    }
    long t1 = now_ms();

    struct rusage ru;
    long peak_kb = -1;
    if (getrusage(RUSAGE_CHILDREN, &ru) == 0) {
        peak_kb = ru_maxrss_to_kb(ru.ru_maxrss);
    }

    int exit_code = 0;
    if (WIFEXITED(wstatus))   exit_code = WEXITSTATUS(wstatus);
    else if (WIFSIGNALED(wstatus)) exit_code = 128 + WTERMSIG(wstatus);

    fprintf(stderr, "RSS_KB=%ld\n", peak_kb);
    fprintf(stderr, "EXIT=%d\n", exit_code);
    fprintf(stderr, "WALL_MS=%ld\n", t1 - t0);

    return exit_code;
}

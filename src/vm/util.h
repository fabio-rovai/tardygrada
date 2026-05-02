/*
 * Tardygrada — Utility helpers
 */

#ifndef TARDY_UTIL_H
#define TARDY_UTIL_H

#include <unistd.h>
#include <string.h>
#include <sys/mman.h>

/* Flags for mmap'ing structures whose committed footprint is far smaller
 * than their virtual size — e.g. tardy_vm_t at ~24 GB virtual but only a
 * few MB actually touched. On Linux MAP_NORESERVE skips the swap-reservation
 * check so heuristic overcommit (the GitHub Actions default) doesn't refuse
 * the allocation. macOS/BSD don't have MAP_NORESERVE; pages there are lazy
 * by default. */
#if defined(__linux__) && defined(MAP_NORESERVE)
#  define TARDY_MAP_LAZY (MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE)
#else
#  define TARDY_MAP_LAZY (MAP_PRIVATE | MAP_ANONYMOUS)
#endif

/* Suppress GCC warn_unused_result for write() */
static inline void tardy_write(int fd, const void *buf, size_t len)
{
    ssize_t r = write(fd, buf, len);
    (void)r;
}

static inline void tardy_print(const char *s)
{
    tardy_write(STDOUT_FILENO, s, strlen(s));
}

static inline void tardy_eprint(const char *s)
{
    tardy_write(STDERR_FILENO, s, strlen(s));
}

#endif /* TARDY_UTIL_H */

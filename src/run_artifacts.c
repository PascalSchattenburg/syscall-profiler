/* ===================================================================
 * run_artifacts.c                                          PHASE-002
 * Dynamic Artifact Detection
 * ===================================================================
 *
 * Read-only filesystem inspection of a run directory. See
 * run_artifacts.h for the full rationale: artifact state is mutable, so
 * it is always recomputed from disk and never stored in the registry,
 * which keeps runs_index.json metadata-only and impossible to leave
 * stale.
 *
 * Implementation notes:
 *   - Uses stat() only. No JSON parsing, no registry lookups, no
 *     external libraries, no caching.
 *   - The canonical artifact filenames match what the rest of the
 *     project writes into a run directory:
 *         profile.json        (C profiler)
 *         benchmark.json      (C profiler, --benchmark / --benchmark-run)
 *         syscall_report.png  (visualize.py, combined report)
 * =================================================================== */

#include "run_artifacts.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Canonical artifact filenames within a run directory. */
#define ARTIFACT_PROFILE        "profile.json"
#define ARTIFACT_BENCHMARK      "benchmark.json"
#define ARTIFACT_VISUALIZATION  "syscall_report.png"

/*
 * Return 1 if "<run_dir>/<filename>" exists and is a regular file, else 0.
 *
 * A trailing slash on run_dir is tolerated. If the joined path would not
 * fit in the buffer, or run_dir is NULL/empty, the artifact is treated as
 * absent (0) rather than risking a truncated, misleading path.
 */
static int artifact_exists(const char *run_dir, const char *filename)
{
    char        path[4096];
    size_t      len;
    int         n;
    struct stat st;

    if (run_dir == NULL || run_dir[0] == '\0')
        return 0;

    /* Drop a single trailing '/' so we don't produce "dir//file". */
    len = strlen(run_dir);
    if (run_dir[len - 1] == '/')
        len--;

    n = snprintf(path, sizeof(path), "%.*s/%s", (int)len, run_dir, filename);
    if (n < 0 || (size_t)n >= sizeof(path))
        return 0;   /* path too long to represent safely */

    if (stat(path, &st) != 0)
        return 0;   /* does not exist (or not accessible) */

    return S_ISREG(st.st_mode) ? 1 : 0;
}

/*
 * Detect all artifacts in a single self-contained pass. Each artifact is
 * an independent stat() of the run directory; nothing is cached between
 * calls. A NULL/empty/missing run_dir yields an all-zero result via
 * artifact_exists().
 */
RunArtifacts run_detect_artifacts(const char *run_dir)
{
    RunArtifacts a;

    a.has_profile       = artifact_exists(run_dir, ARTIFACT_PROFILE);
    a.has_benchmark     = artifact_exists(run_dir, ARTIFACT_BENCHMARK);
    a.has_visualization = artifact_exists(run_dir, ARTIFACT_VISUALIZATION);

    return a;
}

/* ---- Convenience single-artifact queries -------------------------------
 * These delegate to run_detect_artifacts() so the existence rules are
 * defined in exactly one place. When more than one answer is needed,
 * call run_detect_artifacts() directly to avoid repeating the scan.
 * ----------------------------------------------------------------------- */

int run_has_profile(const char *run_dir)
{
    return run_detect_artifacts(run_dir).has_profile;
}

int run_has_benchmark(const char *run_dir)
{
    return run_detect_artifacts(run_dir).has_benchmark;
}

int run_has_visualization(const char *run_dir)
{
    return run_detect_artifacts(run_dir).has_visualization;
}

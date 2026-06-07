#ifndef RUN_ARTIFACTS_H
#define RUN_ARTIFACTS_H

/* ===================================================================
 * run_artifacts.h                                          PHASE-002
 * Dynamic Artifact Detection
 * ===================================================================
 *
 * This module answers one question about a profiling run directory:
 * which artifacts currently exist on disk?
 *
 *     - profile.json          -> has_profile
 *     - benchmark.json        -> has_benchmark
 *     - syscall_report.png    -> has_visualization
 *
 * WHY THIS IS COMPUTED DYNAMICALLY (AND NOT STORED)
 * -------------------------------------------------
 * Artifact state is *mutable over the life of a run*. A directory may
 * start with only profile.json, gain benchmark.json hours later via
 * `./profiler --benchmark-run <dir>`, and gain syscall_report.png later
 * still when the visualizer is run. If we recorded "has_benchmark" /
 * "has_visualization" flags anywhere (for example in the runs_index.json
 * registry), every one of those later actions would have to remember to
 * update the stored copy — and any path that forgot would leave the
 * registry stale and lying about what is on disk.
 *
 * We avoid that entire class of bugs by never persisting artifact state.
 * The registry (runs_index.json) stays strictly metadata-only and
 * immutable after a run is appended: run_id, program, timestamp, path,
 * and the syscall counts — facts that are fixed at creation time and do
 * not change. Anything that *can* change is derived from the filesystem
 * at the moment it is asked for. The filesystem is the single source of
 * truth for "what exists right now"; there is no second copy to drift.
 *
 * HOW THIS PREPARES THE PROJECT FOR A FUTURE WebUI / API
 * ------------------------------------------------------
 * A future WebUI or API will list runs from the registry (cheap, no
 * directory scan) and then, for a run it is actually displaying, call
 * run_detect_artifacts() to learn which artifacts are available right
 * now — for example to enable or grey out a "View charts" button, or to
 * decide whether a "Run benchmark" action is still offered. Because the
 * answer is always recomputed from disk, it is always correct, even if
 * artifacts were added or removed by another process between requests.
 *
 * SCOPE / CONSTRAINTS (Phase 002)
 * -------------------------------
 *   - Read-only. This module never creates, moves, or deletes anything.
 *   - Filesystem checks only (stat). No JSON parsing, no registry
 *     lookups, no external libraries, and no caching — every call
 *     inspects the run directory directly.
 *   - It is infrastructure for future callers; Phase 002 does not wire
 *     it into any user-facing command.
 * =================================================================== */

/*
 * Aggregate artifact state for a single run directory.
 *
 * Each field is a boolean (0 / 1). The aggregate exists because a caller
 * (especially a future WebUI/API) usually wants all three states at once,
 * and computing them together lets run_detect_artifacts() resolve a run
 * in a single, self-contained pass instead of three independent ones.
 */
typedef struct {
    int has_profile;        /* profile.json exists in the run directory     */
    int has_benchmark;      /* benchmark.json exists in the run directory   */
    int has_visualization;  /* syscall_report.png exists (see caveat below) */
} RunArtifacts;

/*
 * Detect all artifacts for a run directory in one pass.
 *
 * `run_dir` is a path like "results/ls/2026-06-06_11-24-36" (a trailing
 * slash is tolerated). The directory is inspected directly every call;
 * nothing is cached.
 *
 * Returns a RunArtifacts with each field set to 1 if the corresponding
 * file exists and is a regular file, else 0. If `run_dir` is NULL, empty,
 * or does not exist, every field is 0 (a missing run simply has no
 * artifacts).
 *
 * has_visualization caveat:
 *   "Visualized" is defined as the presence of syscall_report.png, the
 *   single canonical artifact of a completed visualization run. A run
 *   produced with the visualizer's `--no-combined` option generates the
 *   individual charts but NOT syscall_report.png, and will therefore
 *   report has_visualization = 0. This is intentional for Phase 002:
 *   detection keys on the one canonical combined report, not on the
 *   presence of arbitrary PNG files.
 */
RunArtifacts run_detect_artifacts(const char *run_dir);

/*
 * Convenience single-artifact queries.
 *
 * Each returns 1 if the artifact exists, else 0. They delegate to
 * run_detect_artifacts() so the existence rules live in exactly one
 * place. Prefer run_detect_artifacts() when you need more than one
 * answer, to avoid repeating the directory scan.
 */
int run_has_profile(const char *run_dir);
int run_has_benchmark(const char *run_dir);
int run_has_visualization(const char *run_dir);

#endif /* RUN_ARTIFACTS_H */

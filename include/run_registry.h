#ifndef RUN_REGISTRY_H
#define RUN_REGISTRY_H

#include <stddef.h>      /* size_t */
#include "run_artifacts.h"  /* RunArtifacts, for the Phase 002 bridge */

/* ===================================================================
 * run_registry.h                                          PHASE-003
 * Run Registry Access Layer
 * ===================================================================
 *
 * This module is the single owner of registry access. It loads the
 * Phase 001 catalog (results/runs_index.json) once and exposes the runs
 * as plain C structs, so future components (a CLI tool, an API, a WebUI)
 * can ask "what runs exist?" and "give me run X" through code instead of
 * each re-implementing JSON parsing and filesystem layout knowledge.
 *
 * WHY A DEDICATED ACCESS LAYER
 * ----------------------------
 * The registry already exists, but until now nothing centrally loads it.
 * Without this layer, every future consumer would open runs_index.json,
 * parse the same JSON, and hard-code the same paths — duplicated, fragile,
 * and easy to get subtly wrong. Concentrating that knowledge here means
 * the on-disk format is known in exactly one place; if it ever changes,
 * only this module changes.
 *
 * WHAT IT IS NOT (Phase 003 scope)
 * --------------------------------
 * This is a purely internal, in-process access layer: no CLI flag, no
 * server, no HTTP, no database, no daemon. It is additive — it changes
 * nothing about tracing, profiling, benchmarking, the export formats, or
 * the registry's on-disk format. It only *reads* runs_index.json.
 *
 * RELATIONSHIP TO PHASE 001 / PHASE 002
 * -------------------------------------
 *   - Phase 001 writes immutable per-run metadata into runs_index.json.
 *     This module reads exactly that metadata into RunInfo records and
 *     never adds, removes, or rewrites anything in the registry.
 *   - Phase 002 detects mutable artifact state from the filesystem.
 *     registry_get_artifacts() is the bridge: given a RunInfo (Phase 001
 *     metadata), it returns live RunArtifacts (Phase 002 detection) by
 *     calling run_detect_artifacts(run->path). Artifact state therefore
 *     stays dynamic and is still never stored in the registry.
 * =================================================================== */

/*
 * One run's metadata, mirroring exactly the fields stored per entry in
 * runs_index.json. This is metadata only — it intentionally does NOT
 * contain syscall details; the full data still lives only in the run
 * directory's profile.json. Fixed-size buffers keep the struct flat and
 * free of nested allocations (see the memory-ownership note below).
 */
typedef struct {
    char run_id[32];       /* e.g. "run_a8f3d21c"                    */
    char program[256];     /* program name as stored (basename)      */
    char timestamp[64];    /* e.g. "2026-06-06_11-24-36"             */
    char path[512];        /* e.g. "results/ls/2026-06-06_11-24-36"  */

    long total_syscalls;   /* from the registry entry                */
    long unique_syscalls;  /* from the registry entry                */
} RunInfo;

/*
 * The loaded registry: a heap-allocated array of `count` RunInfo records.
 * When count == 0, runs is NULL.
 */
typedef struct {
    RunInfo *runs;
    size_t   count;
} RunRegistry;

/* -------------------------------------------------------------------
 * MEMORY OWNERSHIP CONTRACT  (read before using this module)
 * -------------------------------------------------------------------
 *  - registry_load() returns a RunRegistry that OWNS its `runs` array.
 *  - The caller OWNS that returned RunRegistry and MUST eventually pass
 *    it to registry_free() to release the array. registry_free() is the
 *    only place that frees registry memory.
 *  - registry_find_by_id() and registry_find_by_program() return a
 *    BORROWED pointer into the registry's `runs` array. It is valid only
 *    until registry_free() is called on that registry. The caller MUST
 *    NOT free a returned RunInfo*, and must not use it after the registry
 *    is freed.
 *  - RunInfo uses fixed-size buffers and holds no pointers, so there are
 *    no per-record allocations to free — registry_free() frees the single
 *    `runs` block and nothing else.
 * ------------------------------------------------------------------- */

/*
 * Load the run registry from <results_dir>/runs_index.json.
 *
 * `results_dir` is the base results directory, e.g. registry_load("results")
 * reads "results/runs_index.json". Only metadata already stored in the
 * registry is loaded; no run directory is scanned.
 *
 * On success returns a RunRegistry owning a `runs` array of `count`
 * records. If the file is missing, empty, or contains no valid entries,
 * returns { runs = NULL, count = 0 } without error output — a fresh
 * install with no runs is a normal, quiet case. (An allocation failure
 * likewise yields an empty registry.)
 *
 * The returned registry must be released with registry_free().
 */
RunRegistry registry_load(const char *results_dir);

/*
 * Find the single run with the given run_id (exact match).
 * Returns a BORROWED pointer into registry->runs (do not free; valid
 * until registry_free), or NULL if not found / on NULL arguments.
 */
RunInfo *registry_find_by_id(RunRegistry *registry, const char *run_id);

/*
 * Find the first run whose `program` field matches `program` EXACTLY
 * (no substring/partial/fuzzy matching). Note the registry stores the
 * program basename (e.g. "ls", "find"), so match against that.
 * Returns a BORROWED pointer into registry->runs (do not free; valid
 * until registry_free), or NULL if not found / on NULL arguments.
 */
RunInfo *registry_find_by_program(RunRegistry *registry, const char *program);

/*
 * Release all memory owned by a registry returned from registry_load().
 * Frees registry->runs, then sets runs = NULL and count = 0 so the struct
 * is safe to reuse or free again (idempotent). Safe to call with NULL.
 */
void registry_free(RunRegistry *registry);

/*
 * Phase 001 <-> Phase 002 bridge: detect the live artifact state for a
 * run by its registry metadata. Internally calls
 * run_detect_artifacts(run->path), so artifact state stays dynamic and is
 * never persisted. Returns an all-zero RunArtifacts if `run` is NULL.
 */
RunArtifacts registry_get_artifacts(const RunInfo *run);

#endif /* RUN_REGISTRY_H */

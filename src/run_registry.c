/* ===================================================================
 * run_registry.c                                          PHASE-003
 * Run Registry Access Layer
 * ===================================================================
 *
 * Loads results/runs_index.json (written by Phase 001) into an array of
 * RunInfo records and provides lookups plus the Phase 002 artifact
 * bridge. See run_registry.h for the API, the design rationale, and the
 * memory-ownership contract.
 *
 * The parser is a small, dependency-free reader tailored to the exact
 * format the Phase 001 writer (registry_write_entry) produces — the same
 * hand-rolled style as read_json_string_field() elsewhere in the project.
 * It is NOT a general-purpose JSON parser: it relies on the registry
 * being machine-written in the known layout. It is tolerant (a malformed
 * entry is skipped rather than aborting the whole load) and never writes
 * to the file.
 *
 * This module only READS the registry. It scans no run directories and
 * changes nothing about the registry format.
 * =================================================================== */

#include "run_registry.h"
#include "run_artifacts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small parsing helpers                                              */
/* ------------------------------------------------------------------ */

/*
 * Copy a JSON string value into `out`, un-escaping the only escapes the
 * Phase 001 writer emits (\" and \\). `src` must point at the first
 * character *after* the opening quote. Stops at the closing unescaped
 * quote or the NUL terminator. Always NUL-terminates within `outsz`.
 */
static void copy_json_string(const char *src, char *out, size_t outsz)
{
    size_t oi = 0;

    if (outsz == 0)
        return;

    while (*src != '\0' && *src != '"' && oi + 1 < outsz) {
        if (*src == '\\' && (src[1] == '"' || src[1] == '\\')) {
            out[oi++] = src[1];
            src += 2;
        } else {
            out[oi++] = *src++;
        }
    }
    out[oi] = '\0';
}

/*
 * Within [start, end), find the first occurrence of the key token
 * "\"<key>\"" and return a pointer just past the following ':' (skipping
 * spaces), i.e. at the first character of the value. Returns NULL if the
 * key is not present in the range.
 */
static const char *find_value(const char *start, const char *end,
                              const char *key)
{
    char   token[64];
    size_t toklen;
    const char *p;

    toklen = (size_t)snprintf(token, sizeof(token), "\"%s\"", key);
    if (toklen >= sizeof(token))
        return NULL;

    for (p = start; p + toklen <= end; p++) {
        if (strncmp(p, token, toklen) == 0) {
            p += toklen;
            while (p < end && (*p == ' ' || *p == '\t' ||
                               *p == '\n' || *p == '\r'))
                p++;
            if (p < end && *p == ':') {
                p++;
                while (p < end && (*p == ' ' || *p == '\t' ||
                                   *p == '\n' || *p == '\r'))
                    p++;
                return p;
            }
        }
    }
    return NULL;
}

/* Read a string field <key> from object slice [start,end) into out. */
static void read_string_field(const char *start, const char *end,
                              const char *key, char *out, size_t outsz)
{
    const char *v = find_value(start, end, key);

    if (outsz > 0)
        out[0] = '\0';
    if (v != NULL && v < end && *v == '"')
        copy_json_string(v + 1, out, outsz);
}

/* Read a numeric field <key> from object slice [start,end). */
static long read_long_field(const char *start, const char *end,
                            const char *key)
{
    const char *v = find_value(start, end, key);

    if (v == NULL || v >= end)
        return 0;
    return strtol(v, NULL, 10);
}

/* Read the whole file at `path` into a malloc'd, NUL-terminated buffer.
 * Returns NULL if the file cannot be opened/read (caller treats this as
 * "no registry"). On success the caller must free the buffer. */
static char *read_whole_file(const char *path)
{
    FILE  *f;
    long   size;
    char  *buf;

    f = fopen(path, "rb");
    if (f == NULL)
        return NULL;

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }

    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) { fclose(f); return NULL; }

    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

RunRegistry registry_load(const char *results_dir)
{
    RunRegistry reg = { NULL, 0 };
    char        idx_path[1024];
    char       *buf;
    const char *p;
    const char *file_end;
    size_t      capacity = 0;

    if (results_dir == NULL || results_dir[0] == '\0')
        return reg;

    if ((size_t)snprintf(idx_path, sizeof(idx_path),
                         "%s/runs_index.json", results_dir) >= sizeof(idx_path))
        return reg;

    buf = read_whole_file(idx_path);
    if (buf == NULL)
        return reg;   /* missing/unreadable -> empty registry, no noise */

    file_end = buf + strlen(buf);

    /*
     * Each entry begins with the "run_id" key, so we treat every
     * "run_id" occurrence as the start of a record. The record's other
     * fields lie between this "run_id" and the next one (or end of file).
     * This is robust even if a program/path value happens to contain
     * braces, since we never rely on '{' / '}' matching.
     */
    p = buf;
    for (;;) {
        const char *rec   = strstr(p, "\"run_id\"");
        const char *next;
        const char *rec_end;

        if (rec == NULL)
            break;

        next    = strstr(rec + 8, "\"run_id\"");
        rec_end = (next != NULL) ? next : file_end;

        /* Grow the array if needed (simple doubling). */
        if (reg.count == capacity) {
            size_t   newcap = (capacity == 0) ? 8 : capacity * 2;
            RunInfo *tmp    = (RunInfo *)realloc(reg.runs,
                                                 newcap * sizeof(RunInfo));
            if (tmp == NULL) {
                /* Out of memory: return what we have so far, consistent. */
                break;
            }
            reg.runs = tmp;
            capacity = newcap;
        }

        {
            RunInfo *r = &reg.runs[reg.count];

            read_string_field(rec, rec_end, "run_id",    r->run_id,    sizeof(r->run_id));
            read_string_field(rec, rec_end, "program",   r->program,   sizeof(r->program));
            read_string_field(rec, rec_end, "timestamp", r->timestamp, sizeof(r->timestamp));
            read_string_field(rec, rec_end, "path",      r->path,      sizeof(r->path));
            r->total_syscalls  = read_long_field(rec, rec_end, "total_syscalls");
            r->unique_syscalls = read_long_field(rec, rec_end, "unique_syscalls");

            /* Only accept an entry that has at least a run_id. */
            if (r->run_id[0] != '\0')
                reg.count++;
        }

        p = rec_end;
    }

    free(buf);

    /* No valid entries -> normalize to the empty result. */
    if (reg.count == 0) {
        free(reg.runs);
        reg.runs = NULL;
    }

    return reg;
}

RunInfo *registry_find_by_id(RunRegistry *registry, const char *run_id)
{
    size_t i;

    if (registry == NULL || run_id == NULL)
        return NULL;

    for (i = 0; i < registry->count; i++) {
        if (strcmp(registry->runs[i].run_id, run_id) == 0)
            return &registry->runs[i];
    }
    return NULL;
}

RunInfo *registry_find_by_program(RunRegistry *registry, const char *program)
{
    size_t i;

    if (registry == NULL || program == NULL)
        return NULL;

    /* Exact match only — no substring/partial/fuzzy matching. */
    for (i = 0; i < registry->count; i++) {
        if (strcmp(registry->runs[i].program, program) == 0)
            return &registry->runs[i];
    }
    return NULL;
}

void registry_free(RunRegistry *registry)
{
    if (registry == NULL)
        return;

    free(registry->runs);
    registry->runs  = NULL;
    registry->count = 0;
}

RunArtifacts registry_get_artifacts(const RunInfo *run)
{
    if (run == NULL) {
        RunArtifacts empty = { 0, 0, 0 };
        return empty;
    }
    /* Bridge to Phase 002: artifact state stays dynamic, never stored. */
    return run_detect_artifacts(run->path);
}

/*
 * profiler.c
 *
 * Aggregates raw syscall trace events into statistical profiles.
 *
 * The key concept here is pairing: every syscall ENTRY must be matched
 * with a corresponding EXIT to calculate execution time.
 */

#include <stdio.h>
#include <string.h>
#include "../include/profiler.h"
#include "../include/syscall_table.h"

/* ---------------------------------------------------------------
 * Internal state
 * --------------------------------------------------------------- */

/* Our flat array of per-syscall statistics */
static syscall_stat_t stats[PROFILER_MAX_ENTRIES];

/* How many unique syscalls we've seen so far */
static int stats_count = 0;

/* Running total of all syscall invocations */
static uint64_t total_call_count = 0;

/* ---------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------- */

/*
 * find_or_create_stat()
 *
 * Look up (or create) the stats entry for a given syscall number.
 * Returns a pointer to the syscall_stat_t, or NULL if we've exceeded
 * our maximum capacity .
 */
static syscall_stat_t *find_or_create_stat(long syscall_num)
{
    int i;

    /* Linear search through existing entries */
    for (i = 0; i < stats_count; i++) {
        if (stats[i].number == syscall_num) {
            return &stats[i];  /* Found it */
        }
    }

    /* Not found: create a new entry if we have room */
    if (stats_count >= PROFILER_MAX_ENTRIES) {
        /* This shouldn't happen with 450 slots on current Linux */
        fprintf(stderr, "[profiler] Warning: stats table full, dropping syscall %ld\n",
                syscall_num);
        return NULL;
    }

    /* Initialize the new entry */
    stats[stats_count].number       = syscall_num;
    stats[stats_count].name         = get_syscall_name(syscall_num);
    stats[stats_count].call_count   = 0;
    stats[stats_count].total_time_ns = 0.0;
    stats[stats_count].entry_time_ns = 0.0;
    stats[stats_count].in_progress  = 0;

    stats_count++;
    return &stats[stats_count - 1];
}

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */

/*
 * profiler_init()
 *
 * Zero out all state. Call this once before tracing begins.
 */
void profiler_init(void)
{
    memset(stats, 0, sizeof(stats));
    stats_count = 0;
    total_call_count = 0;
}

/*
 * profiler_record_entry()
 *
 * Called when we detect a syscall ENTRY (before the kernel handles it).
 * We just save the timestamp; we don't increment call_count yet because
 * the syscall hasn't completed — we wait for the matching exit.
 *
 * Parameters:
 *   syscall_num  - the syscall number (from RAX register)
 *   timestamp_ns - current time in nanoseconds
 */
void profiler_record_entry(long syscall_num, double timestamp_ns)
{
    syscall_stat_t *stat = find_or_create_stat(syscall_num);
    if (stat == NULL) return;

    /*
     * Save the entry timestamp.
     * We'll use it in profiler_record_exit() to compute duration.
     */
    stat->entry_time_ns = timestamp_ns;
    stat->in_progress   = 1;
}

/*
 * profiler_record_exit()
 *
 * Called when we detect a syscall EXIT (after the kernel handled it).
 * Computes the duration and updates statistics.
 */
void profiler_record_exit(long syscall_num, double timestamp_ns)
{
    double duration_ns;
    syscall_stat_t *stat = find_or_create_stat(syscall_num);
    if (stat == NULL) return;

    /* Guard: ignore orphaned exits (no matching entry) */
    if (!stat->in_progress) {
        return;
    }

    /* Calculate how long this syscall took */
    duration_ns = timestamp_ns - stat->entry_time_ns;

    /*
     * Sanity check: duration should be positive.
     * If it's negative, something went wrong with the clock
     */
    if (duration_ns < 0.0) {
        duration_ns = 0.0;
    }

    /* Update statistics */
    stat->call_count++;
    stat->total_time_ns += duration_ns;
    stat->in_progress    = 0;

    /* Update global counter */
    total_call_count++;
}

/*
 * profiler_get_stats()
 *
 * Returns the internal stats array and its current size.
 * Used by the output module to format the report.
 */
syscall_stat_t *profiler_get_stats(int *count_out)
{
    if (count_out != NULL) {
        *count_out = stats_count;
    }
    return stats;
}

/*
 * profiler_total_syscalls()
 *
 * Returns the total number of syscall completions recorded.
 */
uint64_t profiler_total_syscalls(void)
{
    return total_call_count;
}

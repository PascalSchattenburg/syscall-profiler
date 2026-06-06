#ifndef PROFILER_H
#define PROFILER_H

/*
 * profiler.h
 *
 * The Profiler module collects and aggregates syscall statistics.
 *
 * For each unique syscall observed, we track:
 *   - How many times it was called
 *   - Total accumulated execution time
 *   - Average execution time per call
 *
 * TRACING vs PROFILING (educational note):
 * ------------------------------------------
 * Tracing:   recording every individual syscall event (like a log)
 * Profiling: aggregating statistics across all events (like a summary)
 *
 * This project does BOTH:
 *   - The tracer module produces the raw trace events
 *   - The profiler module aggregates them into statistics
 */

#include <stdint.h>

/* Maximum unique syscalls we can profile simultaneously */
#define PROFILER_MAX_ENTRIES 450

/*
 * struct syscall_stat
 *
 * Statistics for a single syscall type (e.g., all "read" calls)
 */
typedef struct {
    long        number;          /* Syscall number */
    const char *name;            /* Syscall name string */
    uint64_t    call_count;      /* How many times this syscall was called */
    double      total_time_ns;   /* Total execution time in nanoseconds */
    double      entry_time_ns;   /* Temporary: timestamp of last entry (internal use) */
    int         in_progress;     /* 1 if we've seen entry but not yet exit */
} syscall_stat_t;

/*
 * profiler_init()
 *
 * Initialize the profiler. Must be called before recording any events.
 */
void profiler_init(void);

/*
 * profiler_record_entry()
 *
 * Record the entry (start) of a syscall.
 * Captures the timestamp so we can calculate duration on exit.
 *
 * Parameters:
 *   syscall_num  - the syscall number
 *   timestamp_ns - current time in nanoseconds
 */
void profiler_record_entry(long syscall_num, double timestamp_ns);

/*
 * profiler_record_exit()
 *
 * Record the exit (end) of a syscall.
 * Calculates and stores the execution duration.
 *
 * Parameters:
 *   syscall_num  - the syscall number (should match the last entry)
 *   timestamp_ns - current time in nanoseconds
 */
void profiler_record_exit(long syscall_num, double timestamp_ns);

/*
 * profiler_get_stats()
 *
 * Returns a pointer to the internal stats array.
 * Used by the output module to print results.
 *
 * Parameters:
 *   count_out - will be set to the number of valid entries
 *
 * Returns:
 *   pointer to syscall_stat_t array
 */
syscall_stat_t *profiler_get_stats(int *count_out);

/*
 * profiler_total_syscalls()
 *
 * Returns the total number of syscalls recorded (sum of all call_counts).
 */
uint64_t profiler_total_syscalls(void);

#endif /* PROFILER_H */

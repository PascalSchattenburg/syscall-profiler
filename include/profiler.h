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
 */

#include <stdint.h>

/* Maximum unique syscalls we can profile simultaneously */
#define PROFILER_MAX_ENTRIES 450

/*
 * struct syscall_stat
 *
 * Statistics for a single syscall type
 */
typedef struct {
    long        number;          
    const char *name;            
    uint64_t    call_count;      
    double      total_time_ns;   
    double      entry_time_ns;  
    int         in_progress;     
} syscall_stat_t;

/*
 * profiler_init()
 *
 * Initialize the profiler.
 */
void profiler_init(void);

/*
 * profiler_record_entry()
 *
 * Record the entry of a syscall.
 * Captures the timestamp so we can calculate duration on exit.
 */
void profiler_record_entry(long syscall_num, double timestamp_ns);

/*
 * profiler_record_exit()
 *
 * Record the exit of a syscall.
 * Calculates and stores the execution duration.
 */
void profiler_record_exit(long syscall_num, double timestamp_ns);

/*
 * profiler_get_stats()
 *
 * Returns a pointer to the internal stats array.
 * Used by the output module to print results.
 */
syscall_stat_t *profiler_get_stats(int *count_out);

/*
 * profiler_total_syscalls()
 *
 * Returns the total number of syscalls recorded.
 */
uint64_t profiler_total_syscalls(void);

#endif 

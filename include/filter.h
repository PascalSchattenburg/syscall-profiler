#ifndef FILTER_H
#define FILTER_H

/*
 * filter.h
 *
 * Filtering system for the System Call Profiler.
 *
 * Supports three CLI options:
 *
 *   --only=read,write,openat
 *       Show ONLY these syscalls in the trace and report.
 *
 *   --exclude=mmap,mprotect,brk
 *       Hide these syscalls from trace and report.
 *       Cannot be combined with --only.
 *
 *   --top=10
 *       In the final report, show only the top N syscalls by call count.
 *       Does not affect the live trace.
 */

#define FILTER_MAX_NAMES 64

extern int filter_top_n;

/*
 * filter_set_only()
 *
 * Parse a comma-separated list of syscall names and configure
 * the filter to show ONLY those syscalls.
 *
 * Example input: "read,write,openat"
 */
int filter_set_only(const char *list);

/*
 * filter_set_exclude()
 *
 * Parse a comma-separated list of syscall names and configure
 * the filter to hide those syscalls.
 *
 * Example input: "mmap,mprotect,brk"
 */
int filter_set_exclude(const char *list);

int filter_should_show(long syscall_num);

/*
 * filter_is_active()
 *
 * Returns 1 if any filter (--only or --exclude) is configured.
 * Used to print a filter summary line in the report header.
 */
int filter_is_active(void);

/*
 * filter_describe()
 *
 * Write a short human-readable description of the active filter
 * into buf:
 *   "only: read, write, openat"
 *   "exclude: mmap, mprotect"
 */
void filter_describe(char *buf, int bufsz);

#endif 

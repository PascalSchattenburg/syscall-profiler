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
 *
 * Filtering affects BOTH:
 *   - The live real-time trace lines
 *   - The final profile report
 *
 * DESIGN:
 *   The filter module owns a small internal list of name strings.
 *   filter_should_show(syscall_num) is the single decision point —
 *   called from the tracer (live trace) and from the output (report).
 */

/* Maximum number of syscall names in an --only or --exclude list */
#define FILTER_MAX_NAMES 64

/* Top-N limit for the report (0 = show all) */
extern int filter_top_n;

/*
 * filter_set_only()
 *
 * Parse a comma-separated list of syscall names and configure
 * the filter to show ONLY those syscalls.
 *
 * Example input: "read,write,openat"
 *
 * Returns 0 on success, -1 if the list is malformed or too long.
 */
int filter_set_only(const char *list);

/*
 * filter_set_exclude()
 *
 * Parse a comma-separated list of syscall names and configure
 * the filter to hide those syscalls.
 *
 * Example input: "mmap,mprotect,brk"
 *
 * Returns 0 on success, -1 if the list is malformed or too long.
 */
int filter_set_exclude(const char *list);

/*
 * filter_should_show()
 *
 * The single decision function. Returns 1 if the given syscall
 * should be shown (in trace and report), 0 if it should be hidden.
 *
 * Called from:
 *   - output_trace_entry() / output_trace_exit()  [live trace]
 *   - output_print_report()                        [final report]
 */
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
 * into buf, e.g.:
 *   "only: read, write, openat"
 *   "exclude: mmap, mprotect"
 */
void filter_describe(char *buf, int bufsz);

#endif /* FILTER_H */

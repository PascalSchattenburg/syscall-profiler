#ifndef OUTPUT_H
#define OUTPUT_H

/*
 * output.h
 *
 * All formatted output for the profiler.
 */

#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/user.h>
#include "profiler.h"

/* ANSI color codes */
#define COLOR_RESET   "\033[0m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"

/* Global flags */
extern int use_color;
extern int show_trace;
extern int export_csv;
extern const char *csv_filename;

/* Convenience macro */
#define CPRINT(color, fmt, ...) \
    do { \
        if (use_color) printf(color fmt COLOR_RESET, ##__VA_ARGS__); \
        else           printf(fmt, ##__VA_ARGS__); \
    } while (0)

/*  Public functions  */
void output_print_header(void);

void output_trace_entry(pid_t child_pid, long syscall_num,
                        const struct user_regs_struct *regs);

void output_trace_exit(long syscall_num, long retval);

void output_print_report(syscall_stat_t *stats, int count, uint64_t total_calls);
void output_export_csv(syscall_stat_t *stats, int count, const char *filename);

void output_export_json(syscall_stat_t *stats, int count,
                        uint64_t total_calls, const char *program,
                        const char *run_id, const char *timestamp,
                        const char *filename);

void output_export_benchmark_json(const char *program,
                                  double untraced_ms, double traced_ms,
                                  const char *run_id, const char *timestamp,
                                  const char *filename);

#endif 

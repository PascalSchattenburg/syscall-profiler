#define _POSIX_C_SOURCE 200809L
/*
 * benchmark.c
 *
 * Measures the overhead that ptrace tracing adds to a program's runtime.
 *
 * MEASUREMENT STRATEGY:
 * We measure wall-clock time from the parent's perspective:
 *
 *   t_start = clock_gettime(CLOCK_MONOTONIC)
 *   fork() + exec() child
 *   waitpid() until child exits
 *   t_end   = clock_gettime(CLOCK_MONOTONIC)
 *   elapsed = t_end - t_start
 *
 * For the UNTRACED run:
 *   - Child calls execvp() directly, no ptrace
 *   - This is the baseline: how long the program takes normally
 *
 * For the TRACED run:
 *   - Child calls ptrace(PTRACE_TRACEME) then execvp()
 *   - Parent runs a simplified ptrace loop (just PTRACE_SYSCALL + waitpid)
 *   - This measures the real cost of tracing
 *
 * MULTIPLE RUNS:
 * We run each mode BENCHMARK_RUNS times and take the MINIMUM.
 * Minimum is more reliable than average for this kind of benchmark:
 *   - Average is skewed by occasional scheduler delays
 *   - Minimum represents the "best case"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>

#include "../include/benchmark.h"
#include "../include/output.h"

/* Number of times to run each mode (min is taken) */
#define BENCHMARK_RUNS 3

/*
 * get_time_ns()  - nanosecond timestamp
*/
static double get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/*
 * run_untraced()
 *
 * Fork and exec the target program without any ptrace.
 * Returns elapsed nanoseconds, or -1.0 on error.
 */
static double run_untraced(char *argv[])
{
    pid_t  child;
    int    status;
    double t_start, t_end;

    t_start = get_time_ns();

    child = fork();
    if (child == -1) { perror("benchmark: fork (untraced)"); return -1.0; }

    if (child == 0) {
    
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        exit(EXIT_FAILURE);
    }

    if (waitpid(child, &status, 0) == -1) {
        perror("benchmark: waitpid (untraced)");
        return -1.0;
    }

    t_end = get_time_ns();
    return t_end - t_start;
}

/* 
 * run_traced()
 *
 * Fork and exec the target program with ptrace tracing.
 * Runs a minimal ptrace loop (PTRACE_SYSCALL only — no decoding,
 * no output) to measure pure tracing overhead.
 */
static double run_traced(char *argv[])
{
    pid_t  child;
    int    status;
    double t_start, t_end;

    t_start = get_time_ns();

    child = fork();
    if (child == -1) { perror("benchmark: fork (traced)"); return -1.0; }

    if (child == 0) {
        /* Child: enable tracing then exec */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1) exit(EXIT_FAILURE);
        execvp(argv[0], argv);
        exit(EXIT_FAILURE);
    }

    /* Parent: wait for initial stop after exec */
    if (waitpid(child, &status, 0) == -1) {
        perror("benchmark: waitpid initial (traced)");
        return -1.0;
    }

    /* Set options */
    ptrace(PTRACE_SETOPTIONS, child, 0,
           PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL);

    /* Minimal tracing loop — intercept every syscall but do nothing */
    for (;;) {
        if (ptrace(PTRACE_SYSCALL, child, NULL, NULL) == -1) break;
        if (waitpid(child, &status, 0) == -1)               break;
        if (WIFEXITED(status) || WIFSIGNALED(status))        break;
    }

    t_end = get_time_ns();
    return t_end - t_start;
}

/* 
 * ns_to_ms()  — convert nanoseconds to milliseconds
*/
static double ns_to_ms(double ns) { return ns / 1e6; }

/* 
 * print_bar()
 *
 * Print a simple ASCII bar scaled to a reference width.
 * bar_chars = (value / max) * BAR_WIDTH
 */
#define BAR_WIDTH 30

static void print_bar(double value, double max_value, const char *color)
{
    int filled = (max_value > 0.0)
                 ? (int)((value / max_value) * BAR_WIDTH)
                 : 0;
    int i;

    if (filled > BAR_WIDTH) filled = BAR_WIDTH;

    if (use_color) printf("%s", color);
    printf("  [");
    for (i = 0; i < filled;      i++) printf("█");
    for (i = filled; i < BAR_WIDTH; i++) printf("░");
    printf("]");
    if (use_color) printf("%s", COLOR_RESET);
}

/*
 * benchmark_run()
 *
 * Main entry point. Runs both modes, collects results, prints table.
*/
int benchmark_run(int argc, char *argv[],
                  double *out_untraced_ms, double *out_traced_ms)
{
    double untraced_min = 1e18;
    double traced_min   = 1e18;
    double overhead_pct;
    double overhead_x;
    int    i;

    (void)argc;

    /* Print benchmark header  */
    printf("\n");
    CPRINT(COLOR_BOLD COLOR_YELLOW,
           "══════════════════════════════════════════════════════════════\n");
    CPRINT(COLOR_BOLD COLOR_YELLOW,
           "  OVERHEAD ANALYSIS (BENCHMARK MODE)\n");
    CPRINT(COLOR_BOLD COLOR_YELLOW,
           "══════════════════════════════════════════════════════════════\n");
    printf("\n");
    CPRINT(COLOR_WHITE,
           "  Running %d iterations of each mode, reporting minimum time.\n",
           BENCHMARK_RUNS);
    CPRINT(COLOR_WHITE,
           "  Program output is suppressed during measurement.\n\n");

    /* Untraced runs  */
    CPRINT(COLOR_CYAN, "  Measuring untraced execution...\n");
    for (i = 0; i < BENCHMARK_RUNS; i++) {
        double t = run_untraced(argv);
        if (t < 0.0) {
            fprintf(stderr, "benchmark: untraced run %d failed\n", i + 1);
            return -1;
        }
        if (t < untraced_min) untraced_min = t;
        CPRINT(COLOR_WHITE, "    run %d: %.3f ms\n", i + 1, ns_to_ms(t));
    }

    printf("\n");

    /* Traced runs  */
    CPRINT(COLOR_MAGENTA, "  Measuring traced execution...\n");
    for (i = 0; i < BENCHMARK_RUNS; i++) {
        double t = run_traced(argv);
        if (t < 0.0) {
            fprintf(stderr, "benchmark: traced run %d failed\n", i + 1);
            return -1;
        }
        if (t < traced_min) traced_min = t;
        CPRINT(COLOR_WHITE, "    run %d: %.3f ms\n", i + 1, ns_to_ms(t));
    }

    printf("\n");

    /*  Calculate overhead  */
    if (untraced_min > 0.0) {
        overhead_pct = ((traced_min - untraced_min) / untraced_min) * 100.0;
        overhead_x   = traced_min / untraced_min;
    } else {
        overhead_pct = 0.0;
        overhead_x   = 1.0;
    }

    /* Results table */
    CPRINT(COLOR_BOLD COLOR_YELLOW, "  RESULTS\n");
    CPRINT(COLOR_WHITE, "  %-24s  %10s\n", "────────────────────────", "──────────");
    CPRINT(COLOR_WHITE, "  %-24s  %10s\n", "MODE",                     "TIME (ms)");
    CPRINT(COLOR_WHITE, "  %-24s  %10s\n", "────────────────────────", "──────────");

    /* Untraced row */
    CPRINT(COLOR_CYAN,  "  %-24s", "Normal (untraced)");
    CPRINT(COLOR_WHITE, "  %10.3f ms\n", ns_to_ms(untraced_min));
    print_bar(untraced_min, traced_min, COLOR_CYAN);
    printf("\n");

    /* Traced row */
    CPRINT(COLOR_MAGENTA, "  %-24s", "Traced (with ptrace)");
    CPRINT(COLOR_WHITE,   "  %10.3f ms\n", ns_to_ms(traced_min));
    print_bar(traced_min, traced_min, COLOR_MAGENTA);
    printf("\n\n");

    /* Overhead summary */
    CPRINT(COLOR_BOLD COLOR_WHITE, "  %-24s  ", "Overhead:");
    if (overhead_pct > 200.0) {
        CPRINT(COLOR_RED,    "+%.0f%%  (%.1fx slower)\n",
               overhead_pct, overhead_x);
    } else if (overhead_pct > 50.0) {
        CPRINT(COLOR_YELLOW, "+%.0f%%  (%.1fx slower)\n",
               overhead_pct, overhead_x);
    } else {
        CPRINT(COLOR_GREEN,  "+%.0f%%  (%.1fx slower)\n",
               overhead_pct, overhead_x);
    }

    printf("\n");
    CPRINT(COLOR_WHITE,
           "  Note: ptrace overhead scales with syscall frequency.\n");
    CPRINT(COLOR_WHITE,
           "  Timings include process startup and scheduler noise.\n");
    CPRINT(COLOR_BOLD COLOR_YELLOW,
           "══════════════════════════════════════════════════════════════\n");
    printf("\n");

    if (out_untraced_ms != NULL) *out_untraced_ms = ns_to_ms(untraced_min);
    if (out_traced_ms   != NULL) *out_traced_ms   = ns_to_ms(traced_min);

    return 0;
}

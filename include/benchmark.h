#ifndef BENCHMARK_H
#define BENCHMARK_H

/*
 * benchmark.h
 *
 * Overhead analysis for the System Call Profiler (P8).
 *
 * PURPOSE:
 * --------
 * ptrace-based tracing is not free. Every syscall the child makes
 * causes the kernel to stop the child, wake the tracer, let the tracer
 * read/write registers, then resume the child. This creates:
 *
 *   - Two context switches per syscall (entry + exit)
 *   - A ptrace() call overhead in the tracer
 *   - Scheduling latency between child stop and tracer wakeup
 *
 * Typical overhead: 3x–20x slowdown depending on syscall frequency.
 * This module measures that overhead empirically.
 *
 * HOW IT WORKS:
 * -------------
 * 1. Run the target program WITHOUT ptrace, measure wall-clock time.
 * 2. Run the target program WITH ptrace tracing, measure wall-clock time.
 * 3. Print the comparison: normal time, traced time, overhead %.
 *
 * Both runs use clock_gettime(CLOCK_MONOTONIC) in the PARENT process,
 * measuring from fork() to waitpid() completion. This is wall-clock
 * time as seen by the parent — the most honest measure of overhead.
 *
 * LIMITATIONS (important for the report):
 * ----------------------------------------
 * - Results vary by system load (scheduler noise)
 * - Short programs (< 1ms) have high variance
 * - ptrace overhead scales with syscall count, not just program length
 * - Results are not reproducible to nanosecond precision
 */

/*
 * benchmark_run()
 *
 * Run the full benchmark: untraced vs traced.
 * Prints the results table to stdout.
 *
 * Parameters:
 *   argc  - number of arguments in the target command
 *   argv  - the target command (e.g. {"ls", "-la", NULL})
 *   out_untraced_ms - optional (may be NULL): receives the minimum
 *                     untraced time in milliseconds, for persisting as
 *                     an artifact. Does not affect measurement or output.
 *   out_traced_ms   - optional (may be NULL): receives the minimum
 *                     traced time in milliseconds.
 *
 * Returns 0 on success, -1 on error.
 */
int benchmark_run(int argc, char *argv[],
                  double *out_untraced_ms, double *out_traced_ms);

#endif /* BENCHMARK_H */

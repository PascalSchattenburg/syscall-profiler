#ifndef BENCHMARK_H
#define BENCHMARK_H

/*
 * benchmark.h
 *
 * Overhead analysis for the System Call Profiler.
 */
int benchmark_run(int argc, char *argv[],
                  double *out_untraced_ms, double *out_traced_ms);

#endif 

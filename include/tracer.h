#ifndef TRACER_H
#define TRACER_H

/*
 * tracer.h
 *
 * The Tracer module is the heart of this project.
 *
 * It uses ptrace() to intercept system calls made by a child process.
#include <sys/types.h>

/* Maximum number of unique syscalls we'll track in profiling */
#define MAX_UNIQUE_SYSCALLS 450

/*
 * struct syscall_event
 *
 * Represents a single syscall intercept event.
 * We capture this on both entry and exit.
 */
typedef struct {
    long   number;       /* Syscall number (from RAX register) */
    long   retval;       /* Return value (only valid on exit) */
    double timestamp_ns; /* Wall-clock time in nanoseconds */
} syscall_event_t;

int tracer_run(int argc, char *argv[]);

#endif 

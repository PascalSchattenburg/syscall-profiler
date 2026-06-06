#ifndef TRACER_H
#define TRACER_H

/*
 * tracer.h
 *
 * The Tracer module is the heart of this project.
 *
 * It uses ptrace() to intercept system calls made by a child process.
 *
 * HOW PTRACE WORKS (educational overview):
 * -----------------------------------------
 * ptrace() is a Linux kernel facility that allows one process (the "tracer")
 * to observe and control the execution of another process (the "tracee").
 *
 * When a traced process makes a system call:
 *   1. The kernel pauses the tracee at the syscall ENTRY point
 *   2. The kernel delivers SIGTRAP to the tracer
 *   3. The tracer reads registers (RAX = syscall number)
 *   4. The tracer calls ptrace(PTRACE_SYSCALL) to resume
 *   5. The kernel pauses the tracee again at syscall EXIT
 *   6. The tracer reads the return value (RAX after syscall)
 *   7. Repeat
 *
 * USER SPACE vs KERNEL SPACE:
 * ----------------------------
 * - User space: where normal programs run (restricted access)
 * - Kernel space: where the OS kernel runs (full hardware access)
 * - System calls are the controlled "bridge" between the two
 * - ptrace() lets us intercept this bridge
 *
 * IMPORTANT: ptrace introduces overhead. Each syscall requires:
 *   - Two context switches (entry + exit)
 *   - Two signals delivered to the tracer
 *   - Register reads via ptrace
 * This is why strace slows programs down noticeably.
 */

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

/*
 * tracer_run()
 *
 * Main tracing loop. Forks a child process to run `argv`,
 * then traces all its system calls until it exits.
 *
 * Parameters:
 *   argc - number of arguments for the target program
 *   argv - argument vector for the target program (e.g. {"ls", "-l", NULL})
 *
 * Returns:
 *   0 on success, -1 on error
 */
int tracer_run(int argc, char *argv[]);

#endif /* TRACER_H */

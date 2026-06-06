#ifndef ARGS_H
#define ARGS_H

/*
 * args.h
 *
 * Syscall argument decoding for the System Call Profiler.
 *
 * On x86-64 Linux, syscall arguments are passed in registers:
 *   arg1 = RDI
 *   arg2 = RSI
 *   arg3 = RDX
 *   arg4 = R10
 *   arg5 = R8
 *   arg6 = R9
 *
 * This module reads those registers and formats them into a
 * human-readable string, e.g.:
 *   openat(AT_FDCWD, "/etc/passwd", O_RDONLY)
 *   read(fd=3, buf, count=4096)
 *   write(fd=1, buf, count=12)
 *
 * We only decode the most common/important syscalls — not every one.
 * Unknown syscalls fall back to showing raw hex arguments.
 *
 * SAFETY:
 * -------
 * Reading strings from the child's memory uses PTRACE_PEEKDATA,
 * which can fail if the pointer is invalid. All reads are bounds-
 * checked and gracefully fall back to "<??>" on error.
 */

#include <sys/types.h>
#include <sys/user.h>   /* struct user_regs_struct */

/* Maximum length of a decoded argument string */
#define ARGS_BUF_SIZE 256

/*
 * decode_args()
 *
 * Decode the arguments of a syscall into a human-readable string.
 * The string is written into `buf` (which must be ARGS_BUF_SIZE bytes).
 *
 * Parameters:
 *   child_pid - PID of the traced child (needed for PTRACE_PEEKDATA)
 *   syscall_num - the syscall number
 *   regs - the full register state captured at syscall entry
 *   buf  - output buffer (ARGS_BUF_SIZE bytes)
 */
void decode_args(pid_t child_pid, long syscall_num,
                 const struct user_regs_struct *regs,
                 char *buf);

/*
 * decode_retval()
 *
 * Format the return value of a syscall into a human-readable string.
 * Negative values are decoded as errno names (e.g. -2 -> ENOENT).
 *
 * Parameters:
 *   syscall_num - the syscall number (context for the return value)
 *   retval      - the value in RAX after the syscall completed
 *   buf         - output buffer (ARGS_BUF_SIZE bytes)
 */
void decode_retval(long syscall_num, long retval, char *buf);

#endif /* ARGS_H */

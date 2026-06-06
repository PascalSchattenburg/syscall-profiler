#ifndef DECODER_H
#define DECODER_H

/*
 * decoder.h
 *
 * Syscall argument decoding for the System Call Profiler.
 *
 * On x86-64 Linux, syscall arguments are passed in registers:
 *   arg1 = rdi
 *   arg2 = rsi
 *   arg3 = rdx
 *   arg4 = r10
 *   arg5 = r8
 *   arg6 = r9
 *
 * This module reads those registers and turns raw numbers into
 * human-readable strings like:
 *   openat(AT_FDCWD, "/etc/passwd", O_RDONLY)
 *   read(fd=3, buf, size=4096)
 *   mmap(length=4096, PROT_READ|PROT_WRITE)
 *
 * We only decode the most common/interesting syscalls — attempting
 * to decode all ~450 would be out of scope for a university project.
 */

#include <sys/types.h>
#include <sys/user.h>   /* struct user_regs_struct */

/* Maximum length of a decoded argument string */
#define DECODER_BUF_SIZE 256

/*
 * decode_syscall_args()
 *
 * Decode the arguments of a syscall at entry time.
 * Reads string arguments from the child process memory via PTRACE_PEEKDATA.
 *
 * Parameters:
 *   pid   - PID of the traced child process
 *   regs  - register snapshot taken at syscall entry
 *   buf   - output buffer (will be filled with decoded string)
 *   bufsz - size of output buffer
 *
 * The result is written into buf as a human-readable string, e.g.:
 *   "AT_FDCWD, \"/etc/passwd\", O_RDONLY"
 *
 * If a syscall is not decoded, buf is set to "".
 */
void decode_syscall_args(pid_t pid,
                         const struct user_regs_struct *regs,
                         char *buf, int bufsz);

/*
 * decode_retval()
 *
 * Format the return value of a syscall (from RAX after exit).
 * Negative values are errno codes and are shown with their name.
 *
 * Parameters:
 *   syscall_num - the syscall number
 *   retval      - raw value from RAX register at syscall exit
 *   buf         - output buffer
 *   bufsz       - size of output buffer
 *
 * Examples:
 *   retval=3        -> "= 3"
 *   retval=-1       -> "= -1 EPERM"
 *   retval=-2       -> "= -2 ENOENT"
 */
void decode_retval(long syscall_num, long retval, char *buf, int bufsz);

#endif /* DECODER_H */

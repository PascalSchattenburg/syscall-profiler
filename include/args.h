#ifndef ARGS_H
#define ARGS_H

/*
 * args.h
 *
 * Syscall argument decoding for the System Call Profiler.
 */

#include <sys/types.h>
#include <sys/user.h>   /* struct user_regs_struct */

/* Maximum length of a decoded argument string */
#define ARGS_BUF_SIZE 256


void decode_args(pid_t child_pid, long syscall_num,
                 const struct user_regs_struct *regs,
                 char *buf);

void decode_retval(long syscall_num, long retval, char *buf);

#endif 

#ifndef DECODER_H
#define DECODER_H

/*
 * decoder.h
 */

#include <sys/types.h>
#include <sys/user.h>   

/* Maximum length of a decoded argument string */
#define DECODER_BUF_SIZE 256

/*
 * decode_syscall_args()
 *
 * Decode the arguments of a syscall at entry time.
 *
 * Parameters:
 *   pid   - PID of the traced child process
 *   regs  - register snapshot taken at syscall entry
 *   buf   - output buffer
 *   bufsz - size of output buffer
 */
void decode_syscall_args(pid_t pid,
                         const struct user_regs_struct *regs,
                         char *buf, int bufsz);

/*
 * decode_retval()
 *
 * Format the return value of a syscall.
 */
void decode_retval(long syscall_num, long retval, char *buf, int bufsz);

#endif 

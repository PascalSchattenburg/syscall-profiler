#ifndef SYSCALL_TABLE_H
#define SYSCALL_TABLE_H

#define MAX_SYSCALL_NUM 450

/*
 * Syscall categories — used for colored prefix tags in the trace output.
 * Each syscall belongs to exactly one category.
 */
typedef enum {
    CAT_FILE    = 0,   /* File I/O: open, read, write, close, stat, ... */
    CAT_MEMORY,        /* Memory:   mmap, brk, munmap, mprotect, ...    */
    CAT_NETWORK,       /* Network:  socket, connect, bind, send, ...    */
    CAT_PROCESS,       /* Process:  fork, clone, execve, exit, wait, ...*/
    CAT_SIGNAL,        /* Signals:  rt_sigaction, kill, ...             */
    CAT_IPC,           /* IPC:      pipe, futex, semget, msgget, ...    */
    CAT_TIME,          /* Time:     clock_gettime, nanosleep, ...       */
    CAT_OTHER          /* Everything else                               */
} syscall_category_t;

/*
 * get_syscall_name()
 *
 * Given a syscall number, return its string name.
 * Returns "unknown" if the number is out of range or unmapped.
 */
const char *get_syscall_name(long syscall_num);

/*
 * get_syscall_category()
 *
 * Given a syscall number, return its category enum.
 * Returns CAT_OTHER for unknown syscalls.
 */
syscall_category_t get_syscall_category(long syscall_num);

/*
 * get_category_label()
 *
 * Return a short 4-char label string for the category, e.g. "FILE".
 * Used for the [FILE] prefix in trace output.
 */
const char *get_category_label(syscall_category_t cat);

/*
 * get_category_color()
 *
 * Return the ANSI color code for the given category.
 * Used to colorize the [CATG] prefix.
 */
const char *get_category_color(syscall_category_t cat);

#endif /* SYSCALL_TABLE_H */

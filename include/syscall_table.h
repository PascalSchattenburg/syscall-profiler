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
    CAT_OTHER          
} syscall_category_t;

/*
 * get_syscall_name()
 *
 * Given a syscall number, return its string name.
 */
const char *get_syscall_name(long syscall_num);

/*
 * get_syscall_category()
 *
 * Given a syscall number, return its category enum.
 */
syscall_category_t get_syscall_category(long syscall_num);

/*
 * get_category_label()
 *
 * Return a string for the category, e.g. "FILE".
 */
const char *get_category_label(syscall_category_t cat);

/*
 * get_category_color()
 *
 * Return the ANSI color code for the given category.
 */
const char *get_category_color(syscall_category_t cat);

#endif 

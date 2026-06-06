/*
 * syscall_table.c
 *
 * Maps x86-64 Linux syscall numbers to names and categories.
 *
 * HOW SYSCALL NUMBERS WORK:
 * --------------------------
 * On x86-64 Linux, syscall numbers are defined in:
 *   /usr/include/asm/unistd_64.h
 *
 * When a program calls read(), glibc places:
 *   - syscall number (0 for read) in register RAX
 *   - arguments in RDI, RSI, RDX, R10, R8, R9
 * Then executes the "syscall" CPU instruction.
 * The kernel performs the work, puts the return value in RAX,
 * and returns control to user space.
 *
 * ptrace() lets us intercept this at both entry and exit points.
 *
 * CATEGORIES:
 * -----------
 * Each syscall is assigned to one of 8 categories (FILE, MEMORY,
 * NETWORK, PROCESS, SIGNAL, IPC, TIME, OTHER). This helps users
 * quickly understand what kind of work a program is doing.
 *
 * Source: Linux kernel arch/x86/entry/syscalls/syscall_64.tbl
 */

#include "../include/syscall_table.h"
#include "../include/output.h"   /* for COLOR_* defines */
#include <stddef.h>

/* ---------------------------------------------------------------
 * Internal entry: name + category for each syscall
 * --------------------------------------------------------------- */
typedef struct {
    const char        *name;
    syscall_category_t category;
} syscall_entry_t;

/*
 * syscall_table[]
 *
 * Index = syscall number.
 * Contains name string + category.
 * NULL name = undefined/unused slot.
 */
static const syscall_entry_t syscall_table[] = {
    /* 0  */ { "read",                   CAT_FILE    },
    /* 1  */ { "write",                  CAT_FILE    },
    /* 2  */ { "open",                   CAT_FILE    },
    /* 3  */ { "close",                  CAT_FILE    },
    /* 4  */ { "stat",                   CAT_FILE    },
    /* 5  */ { "fstat",                  CAT_FILE    },
    /* 6  */ { "lstat",                  CAT_FILE    },
    /* 7  */ { "poll",                   CAT_IPC     },
    /* 8  */ { "lseek",                  CAT_FILE    },
    /* 9  */ { "mmap",                   CAT_MEMORY  },
    /* 10 */ { "mprotect",               CAT_MEMORY  },
    /* 11 */ { "munmap",                 CAT_MEMORY  },
    /* 12 */ { "brk",                    CAT_MEMORY  },
    /* 13 */ { "rt_sigaction",           CAT_SIGNAL  },
    /* 14 */ { "rt_sigprocmask",         CAT_SIGNAL  },
    /* 15 */ { "rt_sigreturn",           CAT_SIGNAL  },
    /* 16 */ { "ioctl",                  CAT_FILE    },
    /* 17 */ { "pread64",                CAT_FILE    },
    /* 18 */ { "pwrite64",               CAT_FILE    },
    /* 19 */ { "readv",                  CAT_FILE    },
    /* 20 */ { "writev",                 CAT_FILE    },
    /* 21 */ { "access",                 CAT_FILE    },
    /* 22 */ { "pipe",                   CAT_IPC     },
    /* 23 */ { "select",                 CAT_IPC     },
    /* 24 */ { "sched_yield",            CAT_PROCESS },
    /* 25 */ { "mremap",                 CAT_MEMORY  },
    /* 26 */ { "msync",                  CAT_MEMORY  },
    /* 27 */ { "mincore",                CAT_MEMORY  },
    /* 28 */ { "madvise",                CAT_MEMORY  },
    /* 29 */ { "shmget",                 CAT_IPC     },
    /* 30 */ { "shmat",                  CAT_IPC     },
    /* 31 */ { "shmctl",                 CAT_IPC     },
    /* 32 */ { "dup",                    CAT_FILE    },
    /* 33 */ { "dup2",                   CAT_FILE    },
    /* 34 */ { "pause",                  CAT_SIGNAL  },
    /* 35 */ { "nanosleep",              CAT_TIME    },
    /* 36 */ { "getitimer",              CAT_TIME    },
    /* 37 */ { "alarm",                  CAT_TIME    },
    /* 38 */ { "setitimer",              CAT_TIME    },
    /* 39 */ { "getpid",                 CAT_PROCESS },
    /* 40 */ { "sendfile",               CAT_FILE    },
    /* 41 */ { "socket",                 CAT_NETWORK },
    /* 42 */ { "connect",                CAT_NETWORK },
    /* 43 */ { "accept",                 CAT_NETWORK },
    /* 44 */ { "sendto",                 CAT_NETWORK },
    /* 45 */ { "recvfrom",               CAT_NETWORK },
    /* 46 */ { "sendmsg",                CAT_NETWORK },
    /* 47 */ { "recvmsg",                CAT_NETWORK },
    /* 48 */ { "shutdown",               CAT_NETWORK },
    /* 49 */ { "bind",                   CAT_NETWORK },
    /* 50 */ { "listen",                 CAT_NETWORK },
    /* 51 */ { "getsockname",            CAT_NETWORK },
    /* 52 */ { "getpeername",            CAT_NETWORK },
    /* 53 */ { "socketpair",             CAT_NETWORK },
    /* 54 */ { "setsockopt",             CAT_NETWORK },
    /* 55 */ { "getsockopt",             CAT_NETWORK },
    /* 56 */ { "clone",                  CAT_PROCESS },
    /* 57 */ { "fork",                   CAT_PROCESS },
    /* 58 */ { "vfork",                  CAT_PROCESS },
    /* 59 */ { "execve",                 CAT_PROCESS },
    /* 60 */ { "exit",                   CAT_PROCESS },
    /* 61 */ { "wait4",                  CAT_PROCESS },
    /* 62 */ { "kill",                   CAT_SIGNAL  },
    /* 63 */ { "uname",                  CAT_OTHER   },
    /* 64 */ { "semget",                 CAT_IPC     },
    /* 65 */ { "semop",                  CAT_IPC     },
    /* 66 */ { "semctl",                 CAT_IPC     },
    /* 67 */ { "shmdt",                  CAT_IPC     },
    /* 68 */ { "msgget",                 CAT_IPC     },
    /* 69 */ { "msgsnd",                 CAT_IPC     },
    /* 70 */ { "msgrcv",                 CAT_IPC     },
    /* 71 */ { "msgctl",                 CAT_IPC     },
    /* 72 */ { "fcntl",                  CAT_FILE    },
    /* 73 */ { "flock",                  CAT_FILE    },
    /* 74 */ { "fsync",                  CAT_FILE    },
    /* 75 */ { "fdatasync",              CAT_FILE    },
    /* 76 */ { "truncate",               CAT_FILE    },
    /* 77 */ { "ftruncate",              CAT_FILE    },
    /* 78 */ { "getdents",               CAT_FILE    },
    /* 79 */ { "getcwd",                 CAT_FILE    },
    /* 80 */ { "chdir",                  CAT_FILE    },
    /* 81 */ { "fchdir",                 CAT_FILE    },
    /* 82 */ { "rename",                 CAT_FILE    },
    /* 83 */ { "mkdir",                  CAT_FILE    },
    /* 84 */ { "rmdir",                  CAT_FILE    },
    /* 85 */ { "creat",                  CAT_FILE    },
    /* 86 */ { "link",                   CAT_FILE    },
    /* 87 */ { "unlink",                 CAT_FILE    },
    /* 88 */ { "symlink",                CAT_FILE    },
    /* 89 */ { "readlink",               CAT_FILE    },
    /* 90 */ { "chmod",                  CAT_FILE    },
    /* 91 */ { "fchmod",                 CAT_FILE    },
    /* 92 */ { "chown",                  CAT_FILE    },
    /* 93 */ { "fchown",                 CAT_FILE    },
    /* 94 */ { "lchown",                 CAT_FILE    },
    /* 95 */ { "umask",                  CAT_FILE    },
    /* 96 */ { "gettimeofday",           CAT_TIME    },
    /* 97 */ { "getrlimit",              CAT_PROCESS },
    /* 98 */ { "getrusage",              CAT_PROCESS },
    /* 99 */ { "sysinfo",                CAT_OTHER   },
    /* 100*/ { "times",                  CAT_TIME    },
    /* 101*/ { "ptrace",                 CAT_PROCESS },
    /* 102*/ { "getuid",                 CAT_PROCESS },
    /* 103*/ { "syslog",                 CAT_OTHER   },
    /* 104*/ { "getgid",                 CAT_PROCESS },
    /* 105*/ { "setuid",                 CAT_PROCESS },
    /* 106*/ { "setgid",                 CAT_PROCESS },
    /* 107*/ { "geteuid",                CAT_PROCESS },
    /* 108*/ { "getegid",                CAT_PROCESS },
    /* 109*/ { "setpgid",                CAT_PROCESS },
    /* 110*/ { "getppid",                CAT_PROCESS },
    /* 111*/ { "getpgrp",                CAT_PROCESS },
    /* 112*/ { "setsid",                 CAT_PROCESS },
    /* 113*/ { "setreuid",               CAT_PROCESS },
    /* 114*/ { "setregid",               CAT_PROCESS },
    /* 115*/ { "getgroups",              CAT_PROCESS },
    /* 116*/ { "setgroups",              CAT_PROCESS },
    /* 117*/ { "setresuid",              CAT_PROCESS },
    /* 118*/ { "getresuid",              CAT_PROCESS },
    /* 119*/ { "setresgid",              CAT_PROCESS },
    /* 120*/ { "getresgid",              CAT_PROCESS },
    /* 121*/ { "getpgid",                CAT_PROCESS },
    /* 122*/ { "setfsuid",               CAT_PROCESS },
    /* 123*/ { "setfsgid",               CAT_PROCESS },
    /* 124*/ { "getsid",                 CAT_PROCESS },
    /* 125*/ { "capget",                 CAT_PROCESS },
    /* 126*/ { "capset",                 CAT_PROCESS },
    /* 127*/ { "rt_sigpending",          CAT_SIGNAL  },
    /* 128*/ { "rt_sigtimedwait",        CAT_SIGNAL  },
    /* 129*/ { "rt_sigqueueinfo",        CAT_SIGNAL  },
    /* 130*/ { "rt_sigsuspend",          CAT_SIGNAL  },
    /* 131*/ { "sigaltstack",            CAT_SIGNAL  },
    /* 132*/ { "utime",                  CAT_TIME    },
    /* 133*/ { "mknod",                  CAT_FILE    },
    /* 134*/ { "uselib",                 CAT_OTHER   },
    /* 135*/ { "personality",            CAT_OTHER   },
    /* 136*/ { "ustat",                  CAT_FILE    },
    /* 137*/ { "statfs",                 CAT_FILE    },
    /* 138*/ { "fstatfs",                CAT_FILE    },
    /* 139*/ { "sysfs",                  CAT_OTHER   },
    /* 140*/ { "getpriority",            CAT_PROCESS },
    /* 141*/ { "setpriority",            CAT_PROCESS },
    /* 142*/ { "sched_setparam",         CAT_PROCESS },
    /* 143*/ { "sched_getparam",         CAT_PROCESS },
    /* 144*/ { "sched_setscheduler",     CAT_PROCESS },
    /* 145*/ { "sched_getscheduler",     CAT_PROCESS },
    /* 146*/ { "sched_get_priority_max", CAT_PROCESS },
    /* 147*/ { "sched_get_priority_min", CAT_PROCESS },
    /* 148*/ { "sched_rr_get_interval",  CAT_PROCESS },
    /* 149*/ { "mlock",                  CAT_MEMORY  },
    /* 150*/ { "munlock",                CAT_MEMORY  },
    /* 151*/ { "mlockall",               CAT_MEMORY  },
    /* 152*/ { "munlockall",             CAT_MEMORY  },
    /* 153*/ { "vhangup",                CAT_OTHER   },
    /* 154*/ { "modify_ldt",             CAT_OTHER   },
    /* 155*/ { "pivot_root",             CAT_OTHER   },
    /* 156*/ { "_sysctl",                CAT_OTHER   },
    /* 157*/ { "prctl",                  CAT_PROCESS },
    /* 158*/ { "arch_prctl",             CAT_PROCESS },
    /* 159*/ { "adjtimex",               CAT_TIME    },
    /* 160*/ { "setrlimit",              CAT_PROCESS },
    /* 161*/ { "chroot",                 CAT_FILE    },
    /* 162*/ { "sync",                   CAT_FILE    },
    /* 163*/ { "acct",                   CAT_OTHER   },
    /* 164*/ { "settimeofday",           CAT_TIME    },
    /* 165*/ { "mount",                  CAT_FILE    },
    /* 166*/ { "umount2",                CAT_FILE    },
    /* 167*/ { "swapon",                 CAT_OTHER   },
    /* 168*/ { "swapoff",                CAT_OTHER   },
    /* 169*/ { "reboot",                 CAT_OTHER   },
    /* 170*/ { "sethostname",            CAT_OTHER   },
    /* 171*/ { "setdomainname",          CAT_OTHER   },
    /* 172*/ { "iopl",                   CAT_OTHER   },
    /* 173*/ { "ioperm",                 CAT_OTHER   },
    /* 174*/ { NULL,                     CAT_OTHER   }, /* create_module - removed */
    /* 175*/ { "init_module",            CAT_OTHER   },
    /* 176*/ { "delete_module",          CAT_OTHER   },
    /* 177*/ { NULL,                     CAT_OTHER   }, /* get_kernel_syms */
    /* 178*/ { NULL,                     CAT_OTHER   }, /* query_module */
    /* 179*/ { "quotactl",               CAT_OTHER   },
    /* 180*/ { NULL,                     CAT_OTHER   }, /* nfsservctl - removed */
    /* 181*/ { NULL,                     CAT_OTHER   }, /* getpmsg - obsolete */
    /* 182*/ { NULL,                     CAT_OTHER   }, /* putpmsg - obsolete */
    /* 183*/ { NULL,                     CAT_OTHER   }, /* afs_syscall */
    /* 184*/ { NULL,                     CAT_OTHER   }, /* tuxcall */
    /* 185*/ { NULL,                     CAT_OTHER   }, /* security */
    /* 186*/ { "gettid",                 CAT_PROCESS },
    /* 187*/ { "readahead",              CAT_FILE    },
    /* 188*/ { "setxattr",               CAT_FILE    },
    /* 189*/ { "lsetxattr",              CAT_FILE    },
    /* 190*/ { "fsetxattr",              CAT_FILE    },
    /* 191*/ { "getxattr",               CAT_FILE    },
    /* 192*/ { "lgetxattr",              CAT_FILE    },
    /* 193*/ { "fgetxattr",              CAT_FILE    },
    /* 194*/ { "listxattr",              CAT_FILE    },
    /* 195*/ { "llistxattr",             CAT_FILE    },
    /* 196*/ { "flistxattr",             CAT_FILE    },
    /* 197*/ { "removexattr",            CAT_FILE    },
    /* 198*/ { "lremovexattr",           CAT_FILE    },
    /* 199*/ { "fremovexattr",           CAT_FILE    },
    /* 200*/ { "tkill",                  CAT_SIGNAL  },
    /* 201*/ { "time",                   CAT_TIME    },
    /* 202*/ { "futex",                  CAT_IPC     },
    /* 203*/ { "sched_setaffinity",      CAT_PROCESS },
    /* 204*/ { "sched_getaffinity",      CAT_PROCESS },
    /* 205*/ { NULL,                     CAT_OTHER   }, /* set_thread_area */
    /* 206*/ { "io_setup",               CAT_FILE    },
    /* 207*/ { "io_destroy",             CAT_FILE    },
    /* 208*/ { "io_getevents",           CAT_FILE    },
    /* 209*/ { "io_submit",              CAT_FILE    },
    /* 210*/ { "io_cancel",              CAT_FILE    },
    /* 211*/ { NULL,                     CAT_OTHER   }, /* get_thread_area */
    /* 212*/ { "lookup_dcookie",         CAT_FILE    },
    /* 213*/ { "epoll_create",           CAT_IPC     },
    /* 214*/ { NULL,                     CAT_OTHER   }, /* epoll_ctl_old */
    /* 215*/ { NULL,                     CAT_OTHER   }, /* epoll_wait_old */
    /* 216*/ { "remap_file_pages",       CAT_MEMORY  },
    /* 217*/ { "getdents64",             CAT_FILE    },
    /* 218*/ { "set_tid_address",        CAT_PROCESS },
    /* 219*/ { "restart_syscall",        CAT_OTHER   },
    /* 220*/ { "semtimedop",             CAT_IPC     },
    /* 221*/ { "fadvise64",              CAT_FILE    },
    /* 222*/ { "timer_create",           CAT_TIME    },
    /* 223*/ { "timer_settime",          CAT_TIME    },
    /* 224*/ { "timer_gettime",          CAT_TIME    },
    /* 225*/ { "timer_getoverrun",       CAT_TIME    },
    /* 226*/ { "timer_delete",           CAT_TIME    },
    /* 227*/ { "clock_settime",          CAT_TIME    },
    /* 228*/ { "clock_gettime",          CAT_TIME    },
    /* 229*/ { "clock_getres",           CAT_TIME    },
    /* 230*/ { "clock_nanosleep",        CAT_TIME    },
    /* 231*/ { "exit_group",             CAT_PROCESS },
    /* 232*/ { "epoll_wait",             CAT_IPC     },
    /* 233*/ { "epoll_ctl",              CAT_IPC     },
    /* 234*/ { "tgkill",                 CAT_SIGNAL  },
    /* 235*/ { "utimes",                 CAT_TIME    },
    /* 236*/ { NULL,                     CAT_OTHER   }, /* vserver */
    /* 237*/ { "mbind",                  CAT_MEMORY  },
    /* 238*/ { "set_mempolicy",          CAT_MEMORY  },
    /* 239*/ { "get_mempolicy",          CAT_MEMORY  },
    /* 240*/ { "mq_open",                CAT_IPC     },
    /* 241*/ { "mq_unlink",              CAT_IPC     },
    /* 242*/ { "mq_timedsend",           CAT_IPC     },
    /* 243*/ { "mq_timedreceive",        CAT_IPC     },
    /* 244*/ { "mq_notify",              CAT_IPC     },
    /* 245*/ { "mq_getsetattr",          CAT_IPC     },
    /* 246*/ { "kexec_load",             CAT_OTHER   },
    /* 247*/ { "waitid",                 CAT_PROCESS },
    /* 248*/ { "add_key",                CAT_OTHER   },
    /* 249*/ { "request_key",            CAT_OTHER   },
    /* 250*/ { "keyctl",                 CAT_OTHER   },
    /* 251*/ { "ioprio_set",             CAT_OTHER   },
    /* 252*/ { "ioprio_get",             CAT_OTHER   },
    /* 253*/ { "inotify_init",           CAT_FILE    },
    /* 254*/ { "inotify_add_watch",      CAT_FILE    },
    /* 255*/ { "inotify_rm_watch",       CAT_FILE    },
    /* 256*/ { "migrate_pages",          CAT_MEMORY  },
    /* 257*/ { "openat",                 CAT_FILE    },
    /* 258*/ { "mkdirat",                CAT_FILE    },
    /* 259*/ { "mknodat",                CAT_FILE    },
    /* 260*/ { "fchownat",               CAT_FILE    },
    /* 261*/ { "futimesat",              CAT_TIME    },
    /* 262*/ { "newfstatat",             CAT_FILE    },
    /* 263*/ { "unlinkat",               CAT_FILE    },
    /* 264*/ { "renameat",               CAT_FILE    },
    /* 265*/ { "linkat",                 CAT_FILE    },
    /* 266*/ { "symlinkat",              CAT_FILE    },
    /* 267*/ { "readlinkat",             CAT_FILE    },
    /* 268*/ { "fchmodat",               CAT_FILE    },
    /* 269*/ { "faccessat",              CAT_FILE    },
    /* 270*/ { "pselect6",               CAT_IPC     },
    /* 271*/ { "ppoll",                  CAT_IPC     },
    /* 272*/ { "unshare",                CAT_PROCESS },
    /* 273*/ { "set_robust_list",        CAT_IPC     },
    /* 274*/ { "get_robust_list",        CAT_IPC     },
    /* 275*/ { "splice",                 CAT_FILE    },
    /* 276*/ { "tee",                    CAT_FILE    },
    /* 277*/ { "sync_file_range",        CAT_FILE    },
    /* 278*/ { "vmsplice",               CAT_FILE    },
    /* 279*/ { "move_pages",             CAT_MEMORY  },
    /* 280*/ { "utimensat",              CAT_TIME    },
    /* 281*/ { "epoll_pwait",            CAT_IPC     },
    /* 282*/ { "signalfd",               CAT_SIGNAL  },
    /* 283*/ { "timerfd_create",         CAT_TIME    },
    /* 284*/ { "eventfd",                CAT_IPC     },
    /* 285*/ { "fallocate",              CAT_FILE    },
    /* 286*/ { "timerfd_settime",        CAT_TIME    },
    /* 287*/ { "timerfd_gettime",        CAT_TIME    },
    /* 288*/ { "accept4",                CAT_NETWORK },
    /* 289*/ { "signalfd4",              CAT_SIGNAL  },
    /* 290*/ { "eventfd2",               CAT_IPC     },
    /* 291*/ { "epoll_create1",          CAT_IPC     },
    /* 292*/ { "dup3",                   CAT_FILE    },
    /* 293*/ { "pipe2",                  CAT_IPC     },
    /* 294*/ { "inotify_init1",          CAT_FILE    },
    /* 295*/ { "preadv",                 CAT_FILE    },
    /* 296*/ { "pwritev",                CAT_FILE    },
    /* 297*/ { "rt_tgsigqueueinfo",      CAT_SIGNAL  },
    /* 298*/ { "perf_event_open",        CAT_OTHER   },
    /* 299*/ { "recvmmsg",               CAT_NETWORK },
    /* 300*/ { "fanotify_init",          CAT_FILE    },
    /* 301*/ { "fanotify_mark",          CAT_FILE    },
    /* 302*/ { "prlimit64",              CAT_PROCESS },
    /* 303*/ { "name_to_handle_at",      CAT_FILE    },
    /* 304*/ { "open_by_handle_at",      CAT_FILE    },
    /* 305*/ { "clock_adjtime",          CAT_TIME    },
    /* 306*/ { "syncfs",                 CAT_FILE    },
    /* 307*/ { "sendmmsg",               CAT_NETWORK },
    /* 308*/ { "setns",                  CAT_PROCESS },
    /* 309*/ { "getcpu",                 CAT_OTHER   },
    /* 310*/ { "process_vm_readv",       CAT_PROCESS },
    /* 311*/ { "process_vm_writev",      CAT_PROCESS },
    /* 312*/ { "kcmp",                   CAT_PROCESS },
    /* 313*/ { "finit_module",           CAT_OTHER   },
    /* 314*/ { "sched_setattr",          CAT_PROCESS },
    /* 315*/ { "sched_getattr",          CAT_PROCESS },
    /* 316*/ { "renameat2",              CAT_FILE    },
    /* 317*/ { "seccomp",                CAT_OTHER   },
    /* 318*/ { "getrandom",              CAT_OTHER   },
    /* 319*/ { "memfd_create",           CAT_MEMORY  },
    /* 320*/ { "kexec_file_load",        CAT_OTHER   },
    /* 321*/ { "bpf",                    CAT_OTHER   },
    /* 322*/ { "execveat",               CAT_PROCESS },
    /* 323*/ { "userfaultfd",            CAT_MEMORY  },
    /* 324*/ { "membarrier",             CAT_MEMORY  },
    /* 325*/ { "mlock2",                 CAT_MEMORY  },
    /* 326*/ { "copy_file_range",        CAT_FILE    },
    /* 327*/ { "preadv2",                CAT_FILE    },
    /* 328*/ { "pwritev2",               CAT_FILE    },
    /* 329*/ { "pkey_mprotect",          CAT_MEMORY  },
    /* 330*/ { "pkey_alloc",             CAT_MEMORY  },
    /* 331*/ { "pkey_free",              CAT_MEMORY  },
    /* 332*/ { "statx",                  CAT_FILE    },
    /* 333*/ { "io_pgetevents",          CAT_FILE    },
    /* 334*/ { "rseq",                   CAT_OTHER   },
    /* 335*/ { NULL,                     CAT_OTHER   },
    /* 336*/ { NULL,                     CAT_OTHER   },
    /* 337*/ { NULL,                     CAT_OTHER   },
    /* 338*/ { NULL,                     CAT_OTHER   },
    /* 339*/ { NULL,                     CAT_OTHER   },
    /* 340*/ { NULL,                     CAT_OTHER   },
    /* 341*/ { NULL,                     CAT_OTHER   },
    /* 342*/ { NULL,                     CAT_OTHER   },
    /* 343*/ { NULL,                     CAT_OTHER   },
    /* 344*/ { NULL,                     CAT_OTHER   },
    /* 345*/ { NULL,                     CAT_OTHER   },
    /* 346*/ { NULL,                     CAT_OTHER   },
    /* 347*/ { NULL,                     CAT_OTHER   },
    /* 348*/ { NULL,                     CAT_OTHER   },
    /* 349*/ { NULL,                     CAT_OTHER   },
    /* 350*/ { NULL,                     CAT_OTHER   },
    /* 351*/ { NULL,                     CAT_OTHER   },
    /* 352*/ { NULL,                     CAT_OTHER   },
    /* 353*/ { NULL,                     CAT_OTHER   },
    /* 354*/ { NULL,                     CAT_OTHER   },
    /* 355*/ { NULL,                     CAT_OTHER   },
    /* 356*/ { NULL,                     CAT_OTHER   },
    /* 357*/ { NULL,                     CAT_OTHER   },
    /* 358*/ { NULL,                     CAT_OTHER   },
    /* 359*/ { "pidfd_send_signal",      CAT_SIGNAL  },
    /* 360*/ { "io_uring_setup",         CAT_FILE    },
    /* 361*/ { "io_uring_enter",         CAT_FILE    },
    /* 362*/ { "io_uring_register",      CAT_FILE    },
    /* 363*/ { "open_tree",              CAT_FILE    },
    /* 364*/ { "move_mount",             CAT_FILE    },
    /* 365*/ { "fsopen",                 CAT_FILE    },
    /* 366*/ { "fsconfig",               CAT_FILE    },
    /* 367*/ { "fsmount",                CAT_FILE    },
    /* 368*/ { "fspick",                 CAT_FILE    },
    /* 369*/ { "pidfd_open",             CAT_PROCESS },
    /* 370*/ { "clone3",                 CAT_PROCESS },
    /* 371*/ { "close_range",            CAT_FILE    },
    /* 372*/ { "openat2",                CAT_FILE    },
    /* 373*/ { "pidfd_getfd",            CAT_PROCESS },
    /* 374*/ { "faccessat2",             CAT_FILE    },
    /* 375*/ { "process_madvise",        CAT_MEMORY  },
    /* 376*/ { "epoll_pwait2",           CAT_IPC     },
    /* 377*/ { "mount_setattr",          CAT_FILE    },
    /* 378*/ { "quotactl_fd",            CAT_OTHER   },
    /* 379*/ { "landlock_create_ruleset",CAT_OTHER   },
    /* 380*/ { "landlock_add_rule",      CAT_OTHER   },
    /* 381*/ { "landlock_restrict_self", CAT_OTHER   },
    /* 382*/ { "memfd_secret",           CAT_MEMORY  },
    /* 383*/ { "process_mrelease",       CAT_PROCESS },
    /* 384*/ { "futex_waitv",            CAT_IPC     },
    /* 385*/ { "set_mempolicy_home_node",CAT_MEMORY  },
};

#define SYSCALL_TABLE_SIZE (sizeof(syscall_table) / sizeof(syscall_table[0]))

/* ---------------------------------------------------------------
 * Category metadata
 * --------------------------------------------------------------- */
typedef struct {
    const char *label;   /* Short 4-char label, e.g. "FILE" */
    const char *color;   /* ANSI color code                 */
} category_meta_t;

static const category_meta_t category_meta[] = {
    /* CAT_FILE    */ { "FILE", "\033[32m"  }, /* green  */
    /* CAT_MEMORY  */ { "MEM ", "\033[33m"  }, /* yellow */
    /* CAT_NETWORK */ { "NET ", "\033[34m"  }, /* blue   */
    /* CAT_PROCESS */ { "PROC", "\033[35m"  }, /* magenta*/
    /* CAT_SIGNAL  */ { "SIG ", "\033[31m"  }, /* red    */
    /* CAT_IPC     */ { "IPC ", "\033[36m"  }, /* cyan   */
    /* CAT_TIME    */ { "TIME", "\033[37m"  }, /* white  */
    /* CAT_OTHER   */ { "SYS ", "\033[90m"  }, /* dark   */
};

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */

const char *get_syscall_name(long syscall_num)
{
    if (syscall_num < 0 || (unsigned long)syscall_num >= SYSCALL_TABLE_SIZE)
        return "unknown";
    if (syscall_table[syscall_num].name == NULL)
        return "unknown";
    return syscall_table[syscall_num].name;
}

syscall_category_t get_syscall_category(long syscall_num)
{
    if (syscall_num < 0 || (unsigned long)syscall_num >= SYSCALL_TABLE_SIZE)
        return CAT_OTHER;
    return syscall_table[syscall_num].category;
}

const char *get_category_label(syscall_category_t cat)
{
    if ((unsigned)cat >= (unsigned)(CAT_OTHER + 1))
        return "SYS ";
    return category_meta[cat].label;
}

const char *get_category_color(syscall_category_t cat)
{
    if ((unsigned)cat >= (unsigned)(CAT_OTHER + 1))
        return "\033[90m";
    return category_meta[cat].color;
}

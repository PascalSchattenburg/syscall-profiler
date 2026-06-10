#define _POSIX_C_SOURCE 200809L
/*
 * decoder.c
 *
 * Decodes syscall arguments and return values into human-readable strings.
 *
 * ARCHITECTURE: x86-64 SYSCALL ABI
 * ----------------------------------
 * When a user-space program executes the "syscall" instruction:
 *   RAX  = syscall number
 *   RDI  = argument 1
 *   RSI  = argument 2
 *   RDX  = argument 3
 *   R10  = argument 4
 *   R8   = argument 5
 *   R9   = argument 6
 *
 * We read these from struct user_regs_struct (captured via PTRACE_GETREGS).
 *
 * READING STRINGS FROM THE CHILD PROCESS:
 * ----------------------------------------
 * The child's string pointers (e.g. the filename in openat) point into
 * the CHILD's virtual address space, not ours. We can't dereference them
 * directly. Instead we use ptrace(PTRACE_PEEKDATA) to read the child's
 * memory word by word.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/socket.h>   /* AF_INET, SOCK_STREAM, etc. */
#include <sys/mman.h>     /* PROT_READ, PROT_WRITE, PROT_EXEC */
#include <fcntl.h>        /* O_RDONLY, O_WRONLY, O_RDWR, etc. */

#include "../include/decoder.h"

/* ---------------------------------------------------------------
 * read_string_from_child()
 *
 * Read a null-terminated string from the child's address space.
 * Uses ptrace(PTRACE_PEEKDATA) to read one word (8 bytes) at a time.
 *
 * Parameters:
 *   pid    - child PID
 *   addr   - address in child's memory to read from
 *   buf    - destination buffer
 *   bufsz  - max bytes to read (including null terminator)
 *
 * Returns:
 *   1 on success, 0 if address is NULL or unreadable
 * --------------------------------------------------------------- */
static int read_string_from_child(pid_t pid, unsigned long addr,
                                   char *buf, int bufsz)
{
    int     i    = 0;
    int     done = 0;

    if (addr == 0) {
        snprintf(buf, bufsz, "(null)");
        return 0;
    }

    /*
     * PTRACE_PEEKDATA reads one word (sizeof(long) = 8 bytes on x86-64)
     * from the child's address space. We copy byte by byte until we hit
     * a null terminator or fill our buffer.
     */
    while (!done && i < bufsz - 1) {
        long    word;
        char   *bytes = (char *)&word;
        int     j;

        errno = 0;
        word  = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + i), NULL);

        if (errno != 0) {
            /* Can't read this address — child may have exited or address invalid */
            if (i == 0) {
                snprintf(buf, bufsz, "???");
                return 0;
            }
            break;
        }

        /* Copy bytes from this word into our buffer */
        for (j = 0; j < (int)sizeof(long) && i < bufsz - 1; j++, i++) {
            buf[i] = bytes[j];
            if (bytes[j] == '\0') {
                done = 1;
                break;
            }
        }
    }

    buf[i] = '\0';
    return 1;
}

/* ---------------------------------------------------------------
 * decode_open_flags()
 *
 * Convert O_RDONLY / O_WRONLY / O_RDWR / O_CREAT / O_TRUNC / ...
 * into a readable string like "O_RDWR|O_CREAT|O_TRUNC".
 * --------------------------------------------------------------- */
static void decode_open_flags(int flags, char *buf, int bufsz)
{
    char tmp[128] = "";

    /* Access mode — lowest 2 bits */
    switch (flags & O_ACCMODE) {
        case O_RDONLY: strncat(tmp, "O_RDONLY", sizeof(tmp) - strlen(tmp) - 1); break;
        case O_WRONLY: strncat(tmp, "O_WRONLY", sizeof(tmp) - strlen(tmp) - 1); break;
        case O_RDWR:   strncat(tmp, "O_RDWR",   sizeof(tmp) - strlen(tmp) - 1); break;
        default:       strncat(tmp, "O_?",       sizeof(tmp) - strlen(tmp) - 1); break;
    }

/* Helper to append a flag name if the bit is set */
#define ADD_FLAG(f, name) \
    if (flags & (f)) { strncat(tmp, "|" name, sizeof(tmp) - strlen(tmp) - 1); }

    ADD_FLAG(O_CREAT,    "O_CREAT")
    ADD_FLAG(O_TRUNC,    "O_TRUNC")
    ADD_FLAG(O_APPEND,   "O_APPEND")
    ADD_FLAG(O_NONBLOCK, "O_NONBLOCK")
    ADD_FLAG(O_CLOEXEC,  "O_CLOEXEC")
    ADD_FLAG(O_DIRECTORY,"O_DIRECTORY")
#undef ADD_FLAG

    snprintf(buf, bufsz, "%s", tmp);
}

/* ---------------------------------------------------------------
 * decode_socket_domain()
 *
 * Convert AF_INET / AF_INET6 / AF_UNIX / ... to a string.
 * --------------------------------------------------------------- */
static const char *decode_socket_domain(int domain)
{
    switch (domain) {
        case AF_UNIX:    return "AF_UNIX";
        case AF_INET:    return "AF_INET";
        case AF_INET6:   return "AF_INET6";
        case AF_NETLINK: return "AF_NETLINK";
        case AF_PACKET:  return "AF_PACKET";
        default:         return "AF_?";
    }
}

/* ---------------------------------------------------------------
 * decode_socket_type()
 *
 * Convert SOCK_STREAM / SOCK_DGRAM / ... to a string.
 * Masks out SOCK_NONBLOCK and SOCK_CLOEXEC flags first.
 * --------------------------------------------------------------- */
static const char *decode_socket_type(int type)
{
    /* SOCK_NONBLOCK and SOCK_CLOEXEC can be OR'd in — strip them */
    switch (type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) {
        case SOCK_STREAM:    return "SOCK_STREAM";
        case SOCK_DGRAM:     return "SOCK_DGRAM";
        case SOCK_RAW:       return "SOCK_RAW";
        case SOCK_SEQPACKET: return "SOCK_SEQPACKET";
        default:             return "SOCK_?";
    }
}

/* ---------------------------------------------------------------
 * decode_mmap_prot()
 *
 * Convert PROT_READ | PROT_WRITE | PROT_EXEC into a string.
 * --------------------------------------------------------------- */
static void decode_mmap_prot(int prot, char *buf, int bufsz)
{
    if (prot == 0) {
        snprintf(buf, bufsz, "PROT_NONE");
        return;
    }

    char tmp[64] = "";
    if (prot & PROT_READ)  strncat(tmp, "PROT_READ",  sizeof(tmp) - strlen(tmp) - 1);
    if (prot & PROT_WRITE) {
        if (strlen(tmp)) strncat(tmp, "|", sizeof(tmp) - strlen(tmp) - 1);
        strncat(tmp, "PROT_WRITE", sizeof(tmp) - strlen(tmp) - 1);
    }
    if (prot & PROT_EXEC) {
        if (strlen(tmp)) strncat(tmp, "|", sizeof(tmp) - strlen(tmp) - 1);
        strncat(tmp, "PROT_EXEC", sizeof(tmp) - strlen(tmp) - 1);
    }
    snprintf(buf, bufsz, "%s", tmp);
}

/* ---------------------------------------------------------------
 * decode_futex_op()
 *
 * Show the futex operation type.
 * --------------------------------------------------------------- */
static const char *decode_futex_op(int op)
{
    /* Mask out FUTEX_PRIVATE_FLAG (128) and FUTEX_CLOCK_REALTIME (256) */
    switch (op & 0x7f) {
        case 0:  return "FUTEX_WAIT";
        case 1:  return "FUTEX_WAKE";
        case 2:  return "FUTEX_FD";
        case 3:  return "FUTEX_REQUEUE";
        case 4:  return "FUTEX_CMP_REQUEUE";
        case 5:  return "FUTEX_WAKE_OP";
        case 9:  return "FUTEX_WAIT_BITSET";
        case 10: return "FUTEX_WAKE_BITSET";
        default: return "FUTEX_?";
    }
}

/* ---------------------------------------------------------------
 * decode_syscall_args()
 *
 * Main dispatch function. Reads registers and decodes arguments
 * for the most important syscalls.
 * --------------------------------------------------------------- */
void decode_syscall_args(pid_t pid,
                         const struct user_regs_struct *regs,
                         char *buf, int bufsz)
{
    long syscall_num = (long)regs->orig_rax;

    /* Convenience aliases for argument registers (x86-64 ABI) */
    unsigned long rdi = regs->rdi;   /* arg1 */
    unsigned long rsi = regs->rsi;   /* arg2 */
    unsigned long rdx = regs->rdx;   /* arg3 */
    /* r10 = arg4, r8 = arg5, r9 = arg6 — used selectively below */

    char path[128];
    char flags_str[64];
    char prot_str[64];

    /* Default: no decoding for this syscall */
    buf[0] = '\0';

    switch (syscall_num) {

    /* ---- read(fd, buf, count) ---- */
    case 0:
        snprintf(buf, bufsz, "fd=%ld, size=%lu", (long)rdi, (unsigned long)rdx);
        break;

    /* ---- write(fd, buf, count) ---- */
    case 1:
        snprintf(buf, bufsz, "fd=%ld, size=%lu", (long)rdi, (unsigned long)rdx);
        break;

    /* ---- open(pathname, flags) ---- */
    case 2:
        read_string_from_child(pid, rdi, path, sizeof(path));
        decode_open_flags((int)rsi, flags_str, sizeof(flags_str));
        snprintf(buf, bufsz, "\"%s\", %s", path, flags_str);
        break;

    /* ---- close(fd) ---- */
    case 3:
        snprintf(buf, bufsz, "fd=%ld", (long)rdi);
        break;

    /* ---- stat(pathname, statbuf) ---- */
    case 4:
    /* ---- lstat(pathname, statbuf) ---- */
    case 6:
        read_string_from_child(pid, rdi, path, sizeof(path));
        snprintf(buf, bufsz, "\"%s\"", path);
        break;

    /* ---- fstat(fd, statbuf) ---- */
    case 5:
        snprintf(buf, bufsz, "fd=%ld", (long)rdi);
        break;

    /* ---- lseek(fd, offset, whence) ---- */
    case 8: {
        const char *whence = "SEEK_?";
        if      (rdx == 0) whence = "SEEK_SET";
        else if (rdx == 1) whence = "SEEK_CUR";
        else if (rdx == 2) whence = "SEEK_END";
        snprintf(buf, bufsz, "fd=%ld, offset=%ld, %s",
                 (long)rdi, (long)rsi, whence);
        break;
    }

    /* ---- mmap(addr, length, prot, flags, fd, offset) ---- */
    case 9:
        decode_mmap_prot((int)rdx, prot_str, sizeof(prot_str));
        snprintf(buf, bufsz, "length=%lu, %s", (unsigned long)rsi, prot_str);
        break;

    /* ---- mprotect(addr, len, prot) ---- */
    case 10:
        decode_mmap_prot((int)rdx, prot_str, sizeof(prot_str));
        snprintf(buf, bufsz, "length=%lu, %s", (unsigned long)rsi, prot_str);
        break;

    /* ---- munmap(addr, length) ---- */
    case 11:
        snprintf(buf, bufsz, "length=%lu", (unsigned long)rsi);
        break;

    /* ---- brk(addr) ---- */
    case 12:
        if (rdi == 0)
            snprintf(buf, bufsz, "0 (query)");
        else
            snprintf(buf, bufsz, "0x%lx", rdi);
        break;

    /* ---- ioctl(fd, request, ...) ---- */
    case 16:
        snprintf(buf, bufsz, "fd=%ld, req=0x%lx", (long)rdi, rsi);
        break;

    /* ---- access(pathname, mode) ---- */
    case 21: {
        const char *mode = "?";
        if      (rsi == 0) mode = "F_OK";
        else if (rsi == 4) mode = "R_OK";
        else if (rsi == 2) mode = "W_OK";
        else if (rsi == 1) mode = "X_OK";
        read_string_from_child(pid, rdi, path, sizeof(path));
        snprintf(buf, bufsz, "\"%s\", %s", path, mode);
        break;
    }

    /* ---- dup(oldfd) ---- */
    case 32:
        snprintf(buf, bufsz, "oldfd=%ld", (long)rdi);
        break;

    /* ---- dup2(oldfd, newfd) ---- */
    case 33:
        snprintf(buf, bufsz, "oldfd=%ld, newfd=%ld", (long)rdi, (long)rsi);
        break;

    /* ---- socket(domain, type, protocol) ---- */
    case 41:
        snprintf(buf, bufsz, "%s, %s",
                 decode_socket_domain((int)rdi),
                 decode_socket_type((int)rsi));
        break;

    /* ---- connect(sockfd, addr, addrlen) ---- */
    case 42:
        snprintf(buf, bufsz, "fd=%ld, addrlen=%lu", (long)rdi, (unsigned long)rdx);
        break;

    /* ---- accept(sockfd, addr, addrlen) ---- */
    case 43:
        snprintf(buf, bufsz, "fd=%ld", (long)rdi);
        break;

    /* ---- sendto(fd, buf, len, flags, ...) ---- */
    case 44:
        snprintf(buf, bufsz, "fd=%ld, size=%lu", (long)rdi, (unsigned long)rdx);
        break;

    /* ---- recvfrom(fd, buf, len, flags, ...) ---- */
    case 45:
        snprintf(buf, bufsz, "fd=%ld, size=%lu", (long)rdi, (unsigned long)rdx);
        break;

    /* ---- shutdown(sockfd, how) ---- */
    case 48: {
        const char *how = "?";
        if      (rsi == 0) how = "SHUT_RD";
        else if (rsi == 1) how = "SHUT_WR";
        else if (rsi == 2) how = "SHUT_RDWR";
        snprintf(buf, bufsz, "fd=%ld, %s", (long)rdi, how);
        break;
    }

    /* ---- bind(fd, addr, addrlen) ---- */
    case 49:
        snprintf(buf, bufsz, "fd=%ld, addrlen=%lu", (long)rdi, (unsigned long)rdx);
        break;

    /* ---- listen(fd, backlog) ---- */
    case 50:
        snprintf(buf, bufsz, "fd=%ld, backlog=%ld", (long)rdi, (long)rsi);
        break;

    /* ---- clone(flags, stack, ...) ---- */
    case 56:
        snprintf(buf, bufsz, "flags=0x%lx", rdi);
        break;

    /* ---- fork() / vfork() — no args ---- */
    case 57:
    case 58:
        /* No arguments to decode */
        break;

    /* ---- execve(pathname, argv, envp) ---- */
    case 59:
        read_string_from_child(pid, rdi, path, sizeof(path));
        snprintf(buf, bufsz, "\"%s\"", path);
        break;

    /* ---- fcntl(fd, cmd, ...) ---- */
    case 72:
        snprintf(buf, bufsz, "fd=%ld, cmd=%ld", (long)rdi, (long)rsi);
        break;

    /* ---- truncate(path, length) ---- */
    case 76:
        read_string_from_child(pid, rdi, path, sizeof(path));
        snprintf(buf, bufsz, "\"%s\", length=%lu", path, (unsigned long)rsi);
        break;

    /* ---- ftruncate(fd, length) ---- */
    case 77:
        snprintf(buf, bufsz, "fd=%ld, length=%lu", (long)rdi, (unsigned long)rsi);
        break;

    /* ---- getcwd(buf, size) ---- */
    case 79:
        snprintf(buf, bufsz, "size=%lu", (unsigned long)rsi);
        break;

    /* ---- rename(old, new) ---- */
    case 82: {
        char newpath[64];
        read_string_from_child(pid, rdi, path, sizeof(path));
        read_string_from_child(pid, rsi, newpath, sizeof(newpath));
        snprintf(buf, bufsz, "\"%s\" -> \"%s\"", path, newpath);
        break;
    }

    /* ---- mkdir(pathname, mode) ---- */
    case 83:
        read_string_from_child(pid, rdi, path, sizeof(path));
        snprintf(buf, bufsz, "\"%s\", mode=0%lo", path, rsi);
        break;

    /* ---- unlink(pathname) ---- */
    case 87:
        read_string_from_child(pid, rdi, path, sizeof(path));
        snprintf(buf, bufsz, "\"%s\"", path);
        break;

    /* ---- readlink(pathname, buf, bufsiz) ---- */
    case 89:
        read_string_from_child(pid, rdi, path, sizeof(path));
        snprintf(buf, bufsz, "\"%s\"", path);
        break;

    /* ---- kill(pid, sig) ---- */
    case 62:
        snprintf(buf, bufsz, "pid=%ld, sig=%ld", (long)rdi, (long)rsi);
        break;

    /* ---- futex(uaddr, op, val, ...) ---- */
    case 202:
        snprintf(buf, bufsz, "%s, val=%ld",
                 decode_futex_op((int)rsi), (long)rdx);
        break;

    /* ---- pread64(fd, buf, count, offset) ---- */
    case 17:
        snprintf(buf, bufsz, "fd=%ld, size=%lu, offset=%lu",
                 (long)rdi, (unsigned long)rdx, (unsigned long)regs->r10);
        break;

    /* ---- pwrite64(fd, buf, count, offset) ---- */
    case 18:
        snprintf(buf, bufsz, "fd=%ld, size=%lu, offset=%lu",
                 (long)rdi, (unsigned long)rdx, (unsigned long)regs->r10);
        break;

    /* ---- openat(dirfd, pathname, flags) ---- */
    case 257: {
        /*
         * AT_FDCWD = -100.  In registers it may appear as:
         *   0xFFFFFFFFFFFFFF9C (64-bit -100)
         *   0x00000000FFFFFF9C (32-bit sign extension variant on some kernels)
         * We detect both forms.
         */
        long dirfd_signed = (long)rdi;
        int is_atfdcwd = (dirfd_signed == -100 ||
                          (rdi & 0xFFFFFFFF) == (unsigned long)(unsigned int)-100);
        read_string_from_child(pid, rsi, path, sizeof(path));
        decode_open_flags((int)rdx, flags_str, sizeof(flags_str));
        if (is_atfdcwd)
            snprintf(buf, bufsz, "AT_FDCWD, \"%s\", %s", path, flags_str);
        else
            snprintf(buf, bufsz, "dirfd=%ld, \"%s\", %s",
                     dirfd_signed, path, flags_str);
        break;
    }

    /* ---- unlinkat(dirfd, pathname, flags) ---- */
    case 263:
        read_string_from_child(pid, rsi, path, sizeof(path));
        snprintf(buf, bufsz, "\"%s\"", path);
        break;

    /* ---- faccessat(dirfd, pathname, mode) ---- */
    case 269:
        read_string_from_child(pid, rsi, path, sizeof(path));
        snprintf(buf, bufsz, "\"%s\"", path);
        break;

    /* ---- statx(dirfd, pathname, ...) ---- */
    case 332:
        read_string_from_child(pid, rsi, path, sizeof(path));
        snprintf(buf, bufsz, "\"%s\"", path);
        break;

    /* ---- nanosleep(req, rem) ---- */
    case 35:
        snprintf(buf, bufsz, "...");
        break;

    /* ---- clock_gettime(clockid, tp) ---- */
    case 228: {
        const char *clk = "CLOCK_?";
        if      (rdi == 0) clk = "CLOCK_REALTIME";
        else if (rdi == 1) clk = "CLOCK_MONOTONIC";
        else if (rdi == 4) clk = "CLOCK_MONOTONIC_RAW";
        snprintf(buf, bufsz, "%s", clk);
        break;
    }

    /* ---- getrandom(buf, buflen, flags) ---- */
    case 318:
        snprintf(buf, bufsz, "size=%lu", (unsigned long)rsi);
        break;

    /* ---- pipe2(pipefd, flags) ---- */
    case 293:
        snprintf(buf, bufsz, "flags=0x%lx", rsi);
        break;

    /* ---- epoll_create1(flags) ---- */
    case 291:
        snprintf(buf, bufsz, "flags=0x%lx", rdi);
        break;

    /* ---- epoll_ctl(epfd, op, fd, event) ---- */
    case 233: {
        const char *op_str = "EPOLL_?";
        if      (rsi == 1) op_str = "EPOLL_CTL_ADD";
        else if (rsi == 2) op_str = "EPOLL_CTL_DEL";
        else if (rsi == 3) op_str = "EPOLL_CTL_MOD";
        snprintf(buf, bufsz, "epfd=%ld, %s, fd=%ld",
                 (long)rdi, op_str, (long)rdx);
        break;
    }

    /* ---- getdents64(fd, dirp, count) ---- */
    case 217:
        snprintf(buf, bufsz, "fd=%ld, size=%lu", (long)rdi, (unsigned long)rdx);
        break;

    default:
        /* Not decoded — leave buf empty, caller will show just "()" */
        break;
    }
}

/* ---------------------------------------------------------------
 * errno name table
 *
 * Maps negative return values to their errno symbol names.
 * Negative RAX at syscall exit = -(errno).
 * --------------------------------------------------------------- */
static const struct { int num; const char *name; } errno_names[] = {
    {  1, "EPERM"          },
    {  2, "ENOENT"         },
    {  3, "ESRCH"          },
    {  4, "EINTR"          },
    {  5, "EIO"            },
    {  6, "ENXIO"          },
    {  7, "E2BIG"          },
    {  8, "ENOEXEC"        },
    {  9, "EBADF"          },
    { 10, "ECHILD"         },
    { 11, "EAGAIN"         },
    { 12, "ENOMEM"         },
    { 13, "EACCES"         },
    { 14, "EFAULT"         },
    { 16, "EBUSY"          },
    { 17, "EEXIST"         },
    { 18, "EXDEV"          },
    { 19, "ENODEV"         },
    { 20, "ENOTDIR"        },
    { 21, "EISDIR"         },
    { 22, "EINVAL"         },
    { 23, "ENFILE"         },
    { 24, "EMFILE"         },
    { 25, "ENOTTY"         },
    { 28, "ENOSPC"         },
    { 29, "ESPIPE"         },
    { 32, "EPIPE"          },
    { 33, "EDOM"           },
    { 34, "ERANGE"         },
    { 35, "EDEADLK"        },
    { 36, "ENAMETOOLONG"   },
    { 37, "ENOLCK"         },
    { 38, "ENOSYS"         },
    { 39, "ENOTEMPTY"      },
    { 40, "ELOOP"          },
    { 42, "ENOMSG"         },
    { 61, "ENODATA"        },
    { 75, "EOVERFLOW"      },
    { 93, "ENOTSUP"        },
    { 95, "EOPNOTSUPP"     },
    { 97, "EAFNOSUPPORT"   },
    { 98, "EADDRINUSE"     },
    { 99, "EADDRNOTAVAIL"  },
    {100, "ENETDOWN"       },
    {101, "ENETUNREACH"    },
    {104, "ECONNRESET"     },
    {107, "ENOTCONN"       },
    {110, "ETIMEDOUT"      },
    {111, "ECONNREFUSED"   },
    {113, "EHOSTUNREACH"   },
    {115, "EINPROGRESS"    },
    {116, "ESTALE"         },
    {  0, NULL             }   /* sentinel */
};

static const char *lookup_errno(int err)
{
    int i;
    for (i = 0; errno_names[i].name != NULL; i++) {
        if (errno_names[i].num == err)
            return errno_names[i].name;
    }
    return NULL;
}

/* ---------------------------------------------------------------
 * decode_retval()
 * --------------------------------------------------------------- */
void decode_retval(long syscall_num, long retval, char *buf, int bufsz)
{
    (void)syscall_num;  /* reserved for future per-syscall return decoding */

    if (retval >= 0) {
        snprintf(buf, bufsz, "= %ld", retval);
    } else {
        /* Negative return = -errno */
        int          err     = (int)(-retval);
        const char  *errname = lookup_errno(err);
        if (errname)
            snprintf(buf, bufsz, "= %ld %s", retval, errname);
        else
            snprintf(buf, bufsz, "= %ld (errno %d)", retval, err);
    }
}

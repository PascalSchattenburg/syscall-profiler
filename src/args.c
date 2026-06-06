#define _POSIX_C_SOURCE 200809L
/* These truncation warnings are intentional — snprintf always writes
 * at most ARGS_BUF_SIZE bytes. Strings from child memory are capped
 * in read_string_from_child(). Safe to suppress. */
#pragma GCC diagnostic ignored "-Wformat-truncation"
/*
 * args.c
 *
 * Syscall argument decoding for the System Call Profiler.
 *
 * ═══════════════════════════════════════════════════════════
 *  HOW ARGUMENT READING WORKS
 * ═══════════════════════════════════════════════════════════
 *
 * x86-64 SYSCALL ABI:
 * -------------------
 * The Linux x86-64 calling convention for syscalls uses these
 * registers for arguments (in order):
 *
 *   RDI = arg1    RSI = arg2    RDX = arg3
 *   R10 = arg4    R8  = arg5    R9  = arg6
 *
 * (Note: R10 is used instead of RCX because the syscall instruction
 * uses RCX internally for the return address.)
 *
 * READING STRINGS FROM CHILD MEMORY:
 * ------------------------------------
 * String arguments (like filenames in openat) are pointers into the
 * child process's virtual address space. We can't dereference them
 * directly — we must use ptrace(PTRACE_PEEKDATA) to read them.
 *
 * PTRACE_PEEKDATA reads one word (8 bytes) at a time from the child.
 * We loop, reading 8 bytes at a time, until we find a null terminator.
 *
 * SAFETY:
 * -------
 * Any pointer from the child might be NULL, unmapped, or otherwise
 * invalid. We check for NULL explicitly and catch ptrace errors
 * (errno is set on failure). Bad reads fall back to "<??>" safely.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "../include/args.h"

/* ---------------------------------------------------------------
 * read_string_from_child()
 *
 * Read a null-terminated string from the child's address space
 * using PTRACE_PEEKDATA.
 *
 * PTRACE_PEEKDATA returns one machine word (8 bytes on x86-64) at
 * a time. We copy bytes into our buffer until we hit '\0' or fill up.
 *
 * Returns number of bytes written (not counting null terminator),
 * or -1 on error.
 * --------------------------------------------------------------- */
static int read_string_from_child(pid_t pid, unsigned long addr,
                                   char *buf, int maxlen)
{
    int   i   = 0;
    int   done = 0;

    if (addr == 0) {
        snprintf(buf, maxlen, "NULL");
        return 4;
    }

    while (i < maxlen - 1 && !done) {
        long   word;
        char   bytes[sizeof(long)];
        int    j;

        errno = 0;
        word  = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + i), NULL);

        if (errno != 0) {
            /* Can't read this memory — stop and mark truncated */
            if (i == 0) {
        snprintf(buf, maxlen, "<err>");
        return -1;
            }
            break;
        }

        /* Copy the 8 bytes of the word into our buffer */
        memcpy(bytes, &word, sizeof(long));
        for (j = 0; j < (int)sizeof(long) && i < maxlen - 1; j++, i++) {
            buf[i] = bytes[j];
            if (bytes[j] == '\0') {
                done = 1;
                break;
            }
        }
    }

    buf[i] = '\0';
    return i;
}

/* ---------------------------------------------------------------
 * decode_open_flags()
 *
 * Convert O_RDONLY / O_WRONLY / O_RDWR / O_CREAT etc. bitmask
 * into a readable string like "O_RDWR|O_CREAT|O_TRUNC".
 * --------------------------------------------------------------- */
static void decode_open_flags(int flags, char *buf, int maxlen)
{
    char tmp[128] = "";
    int  acc = flags & O_ACCMODE;

    /* Access mode (mutually exclusive) */
    if      (acc == O_RDONLY) strncat(tmp, "O_RDONLY", sizeof(tmp) - strlen(tmp) - 1);
    else if (acc == O_WRONLY) strncat(tmp, "O_WRONLY", sizeof(tmp) - strlen(tmp) - 1);
    else if (acc == O_RDWR)   strncat(tmp, "O_RDWR",   sizeof(tmp) - strlen(tmp) - 1);

/* Macro to append a flag name if the bit is set */
#define ADDFLAG(f, name) \
    if (flags & (f)) { \
        if (tmp[0]) strncat(tmp, "|", sizeof(tmp) - strlen(tmp) - 1); \
        strncat(tmp, (name), sizeof(tmp) - strlen(tmp) - 1); \
    }

    ADDFLAG(O_CREAT,    "O_CREAT")
    ADDFLAG(O_TRUNC,    "O_TRUNC")
    ADDFLAG(O_APPEND,   "O_APPEND")
    ADDFLAG(O_NONBLOCK, "O_NONBLOCK")
    ADDFLAG(O_CLOEXEC,  "O_CLOEXEC")
    ADDFLAG(O_DIRECTORY,"O_DIRECTORY")
    ADDFLAG(O_NOFOLLOW, "O_NOFOLLOW")

#undef ADDFLAG

    if (tmp[0] == '\0') snprintf(tmp, sizeof(tmp), "0x%x", flags);
    snprintf(buf, maxlen, "%s", tmp);
}

/* ---------------------------------------------------------------
 * decode_socket_domain()
 *
 * Convert AF_INET / AF_UNIX / AF_INET6 etc. to a name string.
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
 * Convert SOCK_STREAM / SOCK_DGRAM / etc. to a name string.
 * We mask off SOCK_NONBLOCK and SOCK_CLOEXEC before comparing.
 * --------------------------------------------------------------- */
static const char *decode_socket_type(int type)
{
    switch (type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) {
        case SOCK_STREAM:    return "SOCK_STREAM";
        case SOCK_DGRAM:     return "SOCK_DGRAM";
        case SOCK_RAW:       return "SOCK_RAW";
        case SOCK_SEQPACKET: return "SOCK_SEQPACKET";
        default:             return "SOCK_?";
    }
}

/* ---------------------------------------------------------------
 * decode_fd()
 *
 * Format a file descriptor argument.
 * fd=0,1,2 get special names; AT_FDCWD (-100) gets its constant.
 * --------------------------------------------------------------- */
static void decode_fd(long fd, char *buf, int maxlen)
{
    if      (fd == 0)    snprintf(buf, maxlen, "stdin");
    else if (fd == 1)    snprintf(buf, maxlen, "stdout");
    else if (fd == 2)    snprintf(buf, maxlen, "stderr");
    else if (fd == -100) snprintf(buf, maxlen, "AT_FDCWD");
    else                 snprintf(buf, maxlen, "fd=%ld", fd);
}

/* ---------------------------------------------------------------
 * decode_mmap_prot()
 *
 * Convert PROT_READ|PROT_WRITE|PROT_EXEC bitmask to a string.
 * --------------------------------------------------------------- */
static void decode_mmap_prot(int prot, char *buf, int maxlen)
{
    char tmp[64] = "";
    if (prot == 0) { snprintf(buf, maxlen, "PROT_NONE"); return; }

#define ADDPROT(f, name) \
    if (prot & (f)) { \
        if (tmp[0]) strncat(tmp, "|", sizeof(tmp) - strlen(tmp) - 1); \
        strncat(tmp, (name), sizeof(tmp) - strlen(tmp) - 1); \
    }
    ADDPROT(PROT_READ,  "PROT_READ")
    ADDPROT(PROT_WRITE, "PROT_WRITE")
    ADDPROT(PROT_EXEC,  "PROT_EXEC")
#undef ADDPROT

    snprintf(buf, maxlen, "%s", tmp[0] ? tmp : "PROT_?");
}

/* ---------------------------------------------------------------
 * decode_sockaddr()
 *
 * Try to read and decode a sockaddr struct from the child's memory.
 * Works for AF_INET (IPv4) and AF_INET6 (IPv6).
 * Falls back gracefully if memory can't be read.
 * --------------------------------------------------------------- */
static void decode_sockaddr(pid_t pid, unsigned long addr, char *buf, int maxlen)
{
    /* Read the sa_family field first (first 2 bytes of sockaddr) */
    long word;
    unsigned short family;

    if (addr == 0) { snprintf(buf, maxlen, "NULL"); return; }

    errno = 0;
    word  = ptrace(PTRACE_PEEKDATA, pid, (void *)addr, NULL);
    if (errno != 0) { snprintf(buf, maxlen, "<addr=err>"); return; }

    family = (unsigned short)(word & 0xFFFF);

    if (family == AF_INET) {
        /*
         * struct sockaddr_in layout:
         *   uint16_t sin_family  (bytes 0-1)
         *   uint16_t sin_port    (bytes 2-3, big-endian)
         *   uint32_t sin_addr    (bytes 4-7)
         */
        unsigned short port;
        unsigned int   ip;
        char           ip_str[INET_ADDRSTRLEN];

        port = (unsigned short)((word >> 16) & 0xFFFF);
        port = ((port & 0xFF) << 8) | ((port >> 8) & 0xFF); /* ntohs */

        errno = 0;
        word  = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + 4), NULL);
        if (errno != 0) { snprintf(buf, maxlen, "AF_INET:<addr=err>"); return; }

        ip = (unsigned int)(word & 0xFFFFFFFF);
        inet_ntop(AF_INET, &ip, ip_str, sizeof(ip_str));
        snprintf(buf, maxlen, "AF_INET %s:%u", ip_str, port);

    } else if (family == AF_INET6) {
        snprintf(buf, maxlen, "AF_INET6");
    } else if (family == AF_UNIX) {
        /* struct sockaddr_un: sun_path starts at byte 2 */
        char path[64];
        read_string_from_child(pid, addr + 2, path, sizeof(path));
        snprintf(buf, maxlen, "AF_UNIX \"%s\"", path);
    } else {
        snprintf(buf, maxlen, "family=%u", family);
    }
}

/* ---------------------------------------------------------------
 * decode_futex_op()
 *
 * Decode the futex operation number into a readable name.
 * The operation is often OR'd with FUTEX_PRIVATE_FLAG (128).
 * --------------------------------------------------------------- */
static const char *decode_futex_op(int op)
{
    switch (op & 0x7F) {   /* mask off FUTEX_PRIVATE_FLAG */
        case 0:  return "FUTEX_WAIT";
        case 1:  return "FUTEX_WAKE";
        case 2:  return "FUTEX_FD";
        case 3:  return "FUTEX_REQUEUE";
        case 4:  return "FUTEX_CMP_REQUEUE";
        case 9:  return "FUTEX_WAIT_BITSET";
        case 10: return "FUTEX_WAKE_BITSET";
        default: return "FUTEX_?";
    }
}

/* ===============================================================
 * decode_args()
 *
 * Main dispatcher: decode arguments for known syscalls.
 * Falls back to showing raw hex for unknown ones.
 * =============================================================== */
void decode_args(pid_t child_pid, long syscall_num,
                 const struct user_regs_struct *regs,
                 char *buf)
{
    /* Convenience aliases for the six argument registers */
    long a1 = (long)regs->rdi;
    long a2 = (long)regs->rsi;
    long a3 = (long)regs->rdx;
    long a4 = (long)regs->r10;

    char tmp1[512];
    char tmp2[512];

    switch (syscall_num) {

    /* ── read(fd, buf, count) ── */
    case 0:
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s, count=%ld", tmp1, a3);
        break;

    /* ── write(fd, buf, count) ── */
    case 1:
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s, count=%ld", tmp1, a3);
        break;

    /* ── open(path, flags) ── */
    case 2:
        read_string_from_child(child_pid, (unsigned long)a1,
                                tmp1, sizeof(tmp1));
        decode_open_flags((int)a2, tmp2, sizeof(tmp2));
        snprintf(buf, ARGS_BUF_SIZE, "\"%s\", %s", tmp1, tmp2);
        break;

    /* ── close(fd) ── */
    case 3:
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s", tmp1);
        break;

    /* ── stat(path, statbuf) ── */
    case 4:
    /* ── lstat(path, statbuf) ── */
    case 6:
        read_string_from_child(child_pid, (unsigned long)a1,
                                tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "\"%s\"", tmp1);
        break;

    /* ── fstat(fd, statbuf) ── */
    case 5:
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s", tmp1);
        break;

    /* ── lseek(fd, offset, whence) ── */
    case 8: {
        const char *whence = (a3 == 0) ? "SEEK_SET"
                           : (a3 == 1) ? "SEEK_CUR"
                           : (a3 == 2) ? "SEEK_END" : "SEEK_?";
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s, offset=%ld, %s", tmp1, a2, whence);
        break;
    }

    /* ── mmap(addr, length, prot, flags, fd, offset) ── */
    case 9:
        decode_mmap_prot((int)a3, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "len=%ld, %s", a2, tmp1);
        break;

    /* ── mprotect(addr, len, prot) ── */
    case 10:
        decode_mmap_prot((int)a3, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "len=%ld, %s", a2, tmp1);
        break;

    /* ── munmap(addr, len) ── */
    case 11:
        snprintf(buf, ARGS_BUF_SIZE, "len=%ld", a2);
        break;

    /* ── brk(addr) ── */
    case 12:
        if (a1 == 0) snprintf(buf, ARGS_BUF_SIZE, "0 (query)");
        else         snprintf(buf, ARGS_BUF_SIZE, "addr=0x%lx", a1);
        break;

    /* ── ioctl(fd, request, ...) ── */
    case 16:
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s, req=0x%lx", tmp1, a2);
        break;

    /* ── pread64(fd, buf, count, offset) ── */
    case 17:
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s, count=%ld, offset=%ld", tmp1, a3, a4);
        break;

    /* ── access(path, mode) ── */
    case 21: {
        const char *mode = (a2 == 0) ? "F_OK"
                         : (a2 == 4) ? "R_OK"
                         : (a2 == 2) ? "W_OK"
                         : (a2 == 1) ? "X_OK" : "?";
        read_string_from_child(child_pid, (unsigned long)a1,
                                tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "\"%s\", %s", tmp1, mode);
        break;
    }

    /* ── dup(oldfd) ── */
    case 32:
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s", tmp1);
        break;

    /* ── dup2(oldfd, newfd) ── */
    case 33:
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s -> fd=%ld", tmp1, a2);
        break;

    /* ── socket(domain, type, protocol) ── */
    case 41:
        snprintf(buf, ARGS_BUF_SIZE, "%s, %s",
                 decode_socket_domain((int)a1),
                 decode_socket_type((int)a2));
        break;

    /* ── connect(fd, addr, addrlen) ── */
    case 42:
        decode_sockaddr(child_pid, (unsigned long)a2, tmp1, sizeof(tmp1));
        decode_fd(a1, tmp2, sizeof(tmp2));
        snprintf(buf, ARGS_BUF_SIZE, "%s, %s", tmp2, tmp1);
        break;

    /* ── accept(fd, addr, addrlen) ── */
    case 43:
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s", tmp1);
        break;

    /* ── sendto(fd, buf, len, flags, ...) ── */
    case 44:
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s, len=%ld", tmp1, a3);
        break;

    /* ── recvfrom(fd, buf, len, flags, ...) ── */
    case 45:
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s, len=%ld", tmp1, a3);
        break;

    /* ── shutdown(fd, how) ── */
    case 48: {
        const char *how = (a2 == 0) ? "SHUT_RD"
                        : (a2 == 1) ? "SHUT_WR"
                        : (a2 == 2) ? "SHUT_RDWR" : "?";
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s, %s", tmp1, how);
        break;
    }

    /* ── bind(fd, addr, addrlen) ── */
    case 49:
        decode_sockaddr(child_pid, (unsigned long)a2, tmp1, sizeof(tmp1));
        decode_fd(a1, tmp2, sizeof(tmp2));
        snprintf(buf, ARGS_BUF_SIZE, "%s, %s", tmp2, tmp1);
        break;

    /* ── clone(flags, ...) ── */
    case 56:
        snprintf(buf, ARGS_BUF_SIZE, "flags=0x%lx", a1);
        break;

    /* ── execve(path, argv, envp) ── */
    case 59:
        read_string_from_child(child_pid, (unsigned long)a1,
                                tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "\"%s\"", tmp1);
        break;

    /* ── exit(status) / exit_group(status) ── */
    case 60: case 231:
        snprintf(buf, ARGS_BUF_SIZE, "status=%ld", a1);
        break;

    /* ── kill(pid, sig) ── */
    case 62:
        snprintf(buf, ARGS_BUF_SIZE, "pid=%ld, sig=%ld", a1, a2);
        break;

    /* ── fcntl(fd, cmd, ...) ── */
    case 72:
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s, cmd=%ld", tmp1, a2);
        break;

    /* ── getcwd(buf, size) ── */
    case 79:
        snprintf(buf, ARGS_BUF_SIZE, "size=%ld", a2);
        break;

    /* ── chdir(path) / mkdir(path) / rmdir(path) / unlink(path) ── */
    case 80: case 83: case 84: case 87:
        read_string_from_child(child_pid, (unsigned long)a1,
                                tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "\"%s\"", tmp1);
        break;

    /* ── rename(old, new) ── */
    case 82:
        read_string_from_child(child_pid, (unsigned long)a1,
                                tmp1, sizeof(tmp1));
        read_string_from_child(child_pid, (unsigned long)a2,
                                tmp2, sizeof(tmp2));
        snprintf(buf, ARGS_BUF_SIZE, "\"%s\" -> \"%s\"", tmp1, tmp2);
        break;

    /* ── statfs(path, buf) ── */
    case 137:
        read_string_from_child(child_pid, (unsigned long)a1,
                                tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "\"%s\"", tmp1);
        break;

    /* ── getdents64(fd, dirp, count) ── */
    case 217:
        decode_fd(a1, tmp1, sizeof(tmp1));
        snprintf(buf, ARGS_BUF_SIZE, "%s, count=%ld", tmp1, a3);
        break;

    /* ── futex(uaddr, op, val, ...) ── */
    case 202:
        snprintf(buf, ARGS_BUF_SIZE, "%s, val=%ld",
                 decode_futex_op((int)a2), a3);
        break;

    /* ── openat(dirfd, path, flags) ── */
    case 257:
        decode_fd(a1, tmp1, sizeof(tmp1));
        read_string_from_child(child_pid, (unsigned long)a2,
                                tmp2, sizeof(tmp2));
        {
            char flagbuf[64];
            decode_open_flags((int)a3, flagbuf, sizeof(flagbuf));
            snprintf(buf, ARGS_BUF_SIZE, "%s, \"%s\", %s",
                     tmp1, tmp2, flagbuf);
        }
        break;

    /* ── newfstatat(dirfd, path, statbuf, flags) ── */
    case 262:
        decode_fd(a1, tmp1, sizeof(tmp1));
        read_string_from_child(child_pid, (unsigned long)a2,
                                tmp2, sizeof(tmp2));
        snprintf(buf, ARGS_BUF_SIZE, "%s, \"%s\"", tmp1, tmp2);
        break;

    /* ── faccessat(dirfd, path, mode) ── */
    case 269:
        decode_fd(a1, tmp1, sizeof(tmp1));
        read_string_from_child(child_pid, (unsigned long)a2,
                                tmp2, sizeof(tmp2));
        snprintf(buf, ARGS_BUF_SIZE, "%s, \"%s\"", tmp1, tmp2);
        break;

    /* ── prlimit64(pid, resource, ...) ── */
    case 302:
        snprintf(buf, ARGS_BUF_SIZE, "pid=%ld, res=%ld", a1, a2);
        break;

    /* ── Default: show raw hex for up to 3 args ── */
    default:
        snprintf(buf, ARGS_BUF_SIZE, "0x%lx, 0x%lx, 0x%lx",
                 (unsigned long)a1,
                 (unsigned long)a2,
                 (unsigned long)a3);
        break;
    }
}

/* ===============================================================
 * decode_retval()
 *
 * Format the return value. Negative values are errno codes.
 * =============================================================== */

/* errno name table — maps -errno to a string name */
typedef struct { int code; const char *name; } errno_entry_t;

static const errno_entry_t errno_table[] = {
    {  1, "EPERM"       }, {  2, "ENOENT"     }, {  3, "ESRCH"      },
    {  4, "EINTR"       }, {  5, "EIO"         }, {  6, "ENXIO"      },
    {  7, "E2BIG"       }, {  8, "ENOEXEC"     }, {  9, "EBADF"      },
    { 10, "ECHILD"      }, { 11, "EAGAIN"      }, { 12, "ENOMEM"     },
    { 13, "EACCES"      }, { 14, "EFAULT"      }, { 16, "EBUSY"      },
    { 17, "EEXIST"      }, { 18, "EXDEV"       }, { 19, "ENODEV"     },
    { 20, "ENOTDIR"     }, { 21, "EISDIR"      }, { 22, "EINVAL"     },
    { 23, "ENFILE"      }, { 24, "EMFILE"      }, { 25, "ENOTTY"     },
    { 26, "ETXTBSY"     }, { 27, "EFBIG"       }, { 28, "ENOSPC"     },
    { 29, "ESPIPE"      }, { 30, "EROFS"       }, { 31, "EMLINK"     },
    { 32, "EPIPE"       }, { 33, "EDOM"        }, { 34, "ERANGE"     },
    { 35, "EDEADLK"     }, { 36, "ENAMETOOLONG"}, { 37, "ENOLCK"     },
    { 38, "ENOSYS"      }, { 39, "ENOTEMPTY"   }, { 40, "ELOOP"      },
    { 42, "ENOMSG"      }, { 43, "EIDRM"       }, { 61, "ENODATA"    },
    { 62, "ETIME"       }, { 71, "EPROTO"      }, { 74, "EBADMSG"    },
    { 75, "EOVERFLOW"   }, { 84, "EILSEQ"      }, { 87, "EUSERS"     },
    { 88, "ENOTSOCK"    }, { 89, "EDESTADDRREQ"}, { 90, "EMSGSIZE"   },
    { 91, "EPROTOTYPE"  }, { 92, "ENOPROTOOPT" }, { 93, "EPROTONOSUPPORT"},
    { 95, "EOPNOTSUPP"  }, { 97, "EAFNOSUPPORT"}, { 98, "EADDRINUSE" },
    { 99, "EADDRNOTAVAIL"},{100, "ENETDOWN"    },{101, "ENETUNREACH" },
    {103, "ECONNABORTED"},{104, "ECONNRESET"   },{105, "ENOBUFS"     },
    {106, "EISCONN"     },{107, "ENOTCONN"     },{110, "ETIMEDOUT"   },
    {111, "ECONNREFUSED"},{113, "EHOSTUNREACH" },{114, "EALREADY"    },
    {115, "EINPROGRESS" },{116, "ESTALE"       },{125, "ECANCELED"   },
    {  0, NULL }
};

static const char *errno_name(int err)
{
    const errno_entry_t *e;
    for (e = errno_table; e->name != NULL; e++) {
        if (e->code == err) return e->name;
    }
    return NULL;
}

void decode_retval(long syscall_num, long retval, char *buf)
{
    (void)syscall_num; /* reserved for future context-sensitive decoding */

    if (retval >= 0) {
        /* Success — show the plain number */
        snprintf(buf, ARGS_BUF_SIZE, "%ld", retval);
    } else {
        /* Error — show the number and the errno name */
        int         err  = (int)(-retval);
        const char *name = errno_name(err);
        if (name)
            snprintf(buf, ARGS_BUF_SIZE, "-1 (%s)", name);
        else
            snprintf(buf, ARGS_BUF_SIZE, "%ld (errno=%d)", retval, err);
    }
}

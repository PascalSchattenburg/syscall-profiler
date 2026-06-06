#define _POSIX_C_SOURCE 200809L
/*
 * tracer.c
 *
 * Core ptrace loop — now with full fork/clone/thread following.
 *
 * ================================================================
 * STEP 4: FORK / CLONE / THREAD SUPPORT
 * ================================================================
 *
 * WHY WE NEED A PROCESS TABLE:
 * ------------------------------
 * Previously we tracked exactly one PID. But modern programs call:
 *   fork()   → creates a new child PROCESS (copy of parent)
 *   vfork()  → like fork but child runs first, shares memory
 *   clone()  → Linux-specific, creates threads OR processes
 *              (pthreads uses clone() internally)
 *   execve() → replaces the current process image
 *
 * Each of these can create a new tracee. We need to:
 *   1. Automatically attach ptrace to each new child/thread
 *   2. Track its in_syscall state independently
 *   3. Aggregate all their stats into the same profiler
 *   4. Keep looping until EVERY tracked process has exited
 *
 * HOW PTRACE CHILD ATTACHMENT WORKS:
 * ------------------------------------
 * When we set PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
 * PTRACE_O_TRACECLONE on the parent, the kernel automatically
 * attaches ptrace to any new child/thread it creates.
 *
 * The new child starts in a STOPPED state. We detect this via
 * a special ptrace event delivered to the PARENT:
 *
 *   status >> 8 == (SIGTRAP | (PTRACE_EVENT_FORK  << 8))  → fork()
 *   status >> 8 == (SIGTRAP | (PTRACE_EVENT_CLONE << 8))  → clone() / thread
 *   status >> 8 == (SIGTRAP | (PTRACE_EVENT_EXEC  << 8))  → execve()
 *
 * We then call ptrace(PTRACE_GETEVENTMSG) to get the new child's PID,
 * register it in our process table, set its options, and resume it.
 *
 * PROCESS TABLE DESIGN:
 * ----------------------
 * We maintain a fixed-size array of proc_info_t structs.
 * Each entry represents one actively-traced process or thread:
 *   - pid          the OS PID/TID
 *   - in_syscall   whether we're between entry and exit of a syscall
 *   - current_sys  which syscall we're currently inside
 *   - is_thread    1 if created by clone() (a thread), 0 if a process
 *
 * waitpid(-1, ...) waits for ANY child to stop, then we look up
 * which entry in the table corresponds to the stopped PID.
 *
 * ================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <time.h>

#include "../include/tracer.h"
#include "../include/profiler.h"
#include "../include/output.h"
#include "../include/syscall_table.h"

/* ---------------------------------------------------------------
 * Process table
 *
 * Tracks all actively-traced processes and threads.
 * Sized for up to 64 concurrent tracees — more than enough for
 * any typical university test program.
 * --------------------------------------------------------------- */
#define MAX_PROCS 64

typedef struct {
    pid_t pid;            /* OS process/thread ID, 0 = slot is free */
    int   in_syscall;     /* 1 if we're between syscall entry and exit */
    long  current_sys;    /* syscall number at entry (needed at exit)  */
    int   is_thread;      /* 1 = created by clone() = a thread         */
} proc_info_t;

static proc_info_t proc_table[MAX_PROCS];
static int         proc_count = 0;   /* number of active entries */

/* ---------------------------------------------------------------
 * proc_add()  — register a new tracee in the table
 * --------------------------------------------------------------- */
static proc_info_t *proc_add(pid_t pid, int is_thread)
{
    int i;
    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].pid == 0) {
            proc_table[i].pid         = pid;
            proc_table[i].in_syscall  = 0;
            proc_table[i].current_sys = -1;
            proc_table[i].is_thread   = is_thread;
            proc_count++;
            return &proc_table[i];
        }
    }
    fprintf(stderr, "tracer: process table full (max %d)\n", MAX_PROCS);
    return NULL;
}

/* ---------------------------------------------------------------
 * proc_find()  — look up a PID in the table
 * --------------------------------------------------------------- */
static proc_info_t *proc_find(pid_t pid)
{
    int i;
    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].pid == pid)
            return &proc_table[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------
 * proc_remove()  — mark a slot as free when a process exits
 * --------------------------------------------------------------- */
static void proc_remove(pid_t pid)
{
    int i;
    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].pid == pid) {
            proc_table[i].pid = 0;
            proc_count--;
            return;
        }
    }
}

/* ---------------------------------------------------------------
 * get_time_ns()  — monotonic nanosecond timestamp
 * --------------------------------------------------------------- */
static double get_time_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        return 0.0;
    }
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* ---------------------------------------------------------------
 * run_child()  — set up ptrace in the child and exec the target
 * --------------------------------------------------------------- */
static void run_child(char *argv[])
{
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1) {
        perror("child: ptrace(PTRACE_TRACEME)");
        exit(EXIT_FAILURE);
    }
    execvp(argv[0], argv);
    fprintf(stderr, "child: execvp('%s'): %s\n", argv[0], strerror(errno));
    exit(EXIT_FAILURE);
}

/* ---------------------------------------------------------------
 * set_trace_options()
 *
 * Apply ptrace options to a process. Called for the initial child
 * AND for every new child/thread we attach to.
 *
 * Options used:
 *   PTRACE_O_TRACESYSGOOD  — sets bit 7 of SIGTRAP so we can
 *                             distinguish syscall-stops from other stops
 *   PTRACE_O_TRACEFORK     — auto-attach when tracee calls fork()
 *   PTRACE_O_TRACEVFORK    — auto-attach when tracee calls vfork()
 *   PTRACE_O_TRACECLONE    — auto-attach when tracee calls clone()
 *                             (this covers pthreads)
 *   PTRACE_O_TRACEEXEC     — stop tracee after execve() succeeds
 *   PTRACE_O_EXITKILL      — kill all tracees if the tracer exits
 * --------------------------------------------------------------- */
static int set_trace_options(pid_t pid)
{
    long opts = PTRACE_O_TRACESYSGOOD
              | PTRACE_O_TRACEFORK
              | PTRACE_O_TRACEVFORK
              | PTRACE_O_TRACECLONE
              | PTRACE_O_TRACEEXEC
              | PTRACE_O_EXITKILL;

    if (ptrace(PTRACE_SETOPTIONS, pid, 0, opts) == -1) {
        /* Not fatal — might just mean the process already exited */
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------
 * handle_new_child()
 *
 * Called when the tracer receives a PTRACE_EVENT_FORK/VFORK/CLONE
 * notification from a tracee. We:
 *   1. Read the new child's PID via PTRACE_GETEVENTMSG
 *   2. Add it to our process table
 *   3. Set its trace options
 *   4. Print a notification line
 * --------------------------------------------------------------- */
static void handle_new_child(pid_t parent_pid, int is_thread)
{
    unsigned long new_pid_ul = 0;
    pid_t         new_pid;
    int           child_status;

    /*
     * PTRACE_GETEVENTMSG gives us the new child's PID.
     * This is the canonical way to get it — don't rely on guessing.
     */
    if (ptrace(PTRACE_GETEVENTMSG, parent_pid, NULL, &new_pid_ul) == -1) {
        perror("tracer: PTRACE_GETEVENTMSG");
        return;
    }
    new_pid = (pid_t)new_pid_ul;

    if (proc_count >= MAX_PROCS) {
        fprintf(stderr, "tracer: too many processes, ignoring PID %d\n", new_pid);
        return;
    }

    /*
     * The new child may already be stopped waiting for us, or it may
     * not have started yet. waitpid with WNOHANG checks without blocking.
     * Either way, we add it to the table and will see it in the main loop.
     */
    waitpid(new_pid, &child_status, WNOHANG);

    proc_add(new_pid, is_thread);
    set_trace_options(new_pid);

    /* Print notification */
    if (show_trace) {
        if (is_thread) {
            CPRINT(COLOR_MAGENTA,
                   "  [+] New thread:  TID %-6d  (parent PID %d)\n",
                   new_pid, parent_pid);
        } else {
            CPRINT(COLOR_CYAN,
                   "  [+] New process: PID %-6d  (parent PID %d)\n",
                   new_pid, parent_pid);
        }
    }

    /*
     * Resume the new child. It was stopped by ptrace on creation.
     * We use PTRACE_SYSCALL so it stops at the next syscall boundary.
     */
    ptrace(PTRACE_SYSCALL, new_pid, NULL, NULL);
}

/* ---------------------------------------------------------------
 * tracer_run()
 *
 * Main tracing loop — now multi-process aware.
 *
 * Key differences from the single-process version:
 *
 *   1. We use waitpid(-1, ...) to wait for ANY child to stop,
 *      not just the original child_pid.
 *
 *   2. Each stopped PID is looked up in proc_table[].
 *      State (in_syscall, current_sys) is per-PID.
 *
 *   3. ptrace events (PTRACE_EVENT_FORK etc.) are detected via
 *      the top byte of waitpid status. We call handle_new_child()
 *      which registers the new PID and prints a notification.
 *
 *   4. We loop until proc_count == 0 (all processes/threads exited).
 * --------------------------------------------------------------- */
int tracer_run(int argc, char *argv[])
{
    pid_t  child_pid;
    int    status;

    (void)argc;

    /* Clear the process table */
    memset(proc_table, 0, sizeof(proc_table));
    proc_count = 0;

    profiler_init();

    if (show_trace) {
        printf("\n");
        CPRINT(COLOR_BOLD COLOR_CYAN,
               "  ┌─────────────────────────────────────────┐\n");
        CPRINT(COLOR_BOLD COLOR_CYAN,
               "  │  REAL-TIME TRACE                        │\n");
        CPRINT(COLOR_BOLD COLOR_CYAN,
               "  └─────────────────────────────────────────┘\n");
        printf("\n");
    }

    /* ---- Fork the initial child ---- */
    child_pid = fork();
    if (child_pid == -1) { perror("tracer: fork"); return -1; }

    if (child_pid == 0) {
        run_child(argv);
        exit(EXIT_FAILURE);
    }

    /*
     * Wait for the child's first stop. After execvp(), the child
     * receives SIGTRAP (from PTRACE_TRACEME) and stops automatically.
     */
    if (waitpid(child_pid, &status, 0) == -1) {
        perror("tracer: waitpid (initial)");
        return -1;
    }

    /* Register the initial child and set all tracing options */
    proc_add(child_pid, 0 /* not a thread */);
    if (set_trace_options(child_pid) == -1) {
        perror("tracer: ptrace(PTRACE_SETOPTIONS)");
        return -1;
    }

    /* Start tracing the initial child */
    ptrace(PTRACE_SYSCALL, child_pid, NULL, NULL);

    /* ================================================================
     * Main tracing loop
     *
     * We use waitpid(-1, ...) to receive events from ANY tracee.
     * The loop runs until all tracked processes have exited.
     * ================================================================ */
    while (proc_count > 0) {
        pid_t        stopped_pid;
        proc_info_t *proc;
        int          event;

        /*
         * Wait for any child to stop.
         * __WALL is needed to also catch thread stops (CLONE_THREAD children).
         */
        stopped_pid = waitpid(-1, &status, __WALL);
        if (stopped_pid == -1) {
            if (errno == ECHILD) break;  /* no more children */
            if (errno == EINTR)  continue; /* interrupted by signal, retry */
            perror("tracer: waitpid");
            break;
        }

        /* ---- Process exited normally ---- */
        if (WIFEXITED(status)) {
            proc = proc_find(stopped_pid);
            if (proc != NULL) {
                if (show_trace) {
                    if (proc->is_thread) {
                        CPRINT(COLOR_MAGENTA,
                               "  [-] Thread  TID %-6d exited (status %d)\n",
                               stopped_pid, WEXITSTATUS(status));
                    } else if (stopped_pid == child_pid) {
                        /* Main process — print the primary exit message */
                        printf("\n");
                        CPRINT(COLOR_GREEN,
                               "  [✓] Process exited with status %d\n",
                               WEXITSTATUS(status));
                    } else {
                        CPRINT(COLOR_CYAN,
                               "  [-] Process PID %-6d exited (status %d)\n",
                               stopped_pid, WEXITSTATUS(status));
                    }
                }
                proc_remove(stopped_pid);
            }
            continue;
        }

        /* ---- Process killed by signal ---- */
        if (WIFSIGNALED(status)) {
            proc = proc_find(stopped_pid);
            if (show_trace && proc != NULL) {
                printf("\n");
                CPRINT(COLOR_RED,
                       "  [!] PID %-6d killed by signal %d (%s)\n",
                       stopped_pid, WTERMSIG(status),
                       strsignal(WTERMSIG(status)));
            }
            if (proc != NULL) proc_remove(stopped_pid);
            continue;
        }

        /* ---- Process stopped ---- */
        if (!WIFSTOPPED(status)) continue;

        proc = proc_find(stopped_pid);
        if (proc == NULL) {
            /*
             * Unknown PID stopped — this can happen when a new child
             * is reported to us before we've processed the parent's
             * PTRACE_EVENT_* notification. Register it and continue.
             */
            proc_add(stopped_pid, 0);
            set_trace_options(stopped_pid);
            ptrace(PTRACE_SYSCALL, stopped_pid, NULL, NULL);
            continue;
        }

        /*
         * Decode the stop reason.
         *
         * WSTOPSIG(status) gives the low 8 bits.
         * The high byte (status >> 8) carries ptrace event info.
         *
         * Syscall-stop:  WSTOPSIG == (SIGTRAP | 0x80)   (needs TRACESYSGOOD)
         * Fork event:    (status >> 8) == SIGTRAP | (PTRACE_EVENT_FORK  << 8)
         * Clone event:   (status >> 8) == SIGTRAP | (PTRACE_EVENT_CLONE << 8)
         * Exec event:    (status >> 8) == SIGTRAP | (PTRACE_EVENT_EXEC  << 8)
         */
        event = (status >> 8);

        /* ---- Fork / vfork event — new child process ---- */
        if (event == (SIGTRAP | (PTRACE_EVENT_FORK  << 8)) ||
            event == (SIGTRAP | (PTRACE_EVENT_VFORK << 8))) {
            handle_new_child(stopped_pid, 0 /* process */);
            ptrace(PTRACE_SYSCALL, stopped_pid, NULL, NULL);
            continue;
        }

        /* ---- Clone event — new thread (or process) ---- */
        if (event == (SIGTRAP | (PTRACE_EVENT_CLONE << 8))) {
            /*
             * clone() is used for both threads and processes.
             * We label them all as threads since clone() is how
             * pthreads works — the distinction matters for display.
             */
            handle_new_child(stopped_pid, 1 /* thread */);
            ptrace(PTRACE_SYSCALL, stopped_pid, NULL, NULL);
            continue;
        }

        /* ---- Exec event — process replaced its image ---- */
        if (event == (SIGTRAP | (PTRACE_EVENT_EXEC << 8))) {
            if (show_trace) {
                CPRINT(COLOR_YELLOW,
                       "  [~] PID %-6d called execve() — new image loaded\n",
                       stopped_pid);
            }
            /*
             * After exec, the process is fresh. Reset its syscall state
             * so we don't treat the exec's exit as a stale syscall pair.
             */
            proc->in_syscall  = 0;
            proc->current_sys = -1;
            ptrace(PTRACE_SYSCALL, stopped_pid, NULL, NULL);
            continue;
        }

        /* ---- Syscall-stop (the normal case) ---- */
        if (WSTOPSIG(status) == (SIGTRAP | 0x80)) {
            struct user_regs_struct regs;
            double timestamp_ns;

            if (ptrace(PTRACE_GETREGS, stopped_pid, NULL, &regs) == -1) {
                /* Process may have just exited — skip */
                ptrace(PTRACE_SYSCALL, stopped_pid, NULL, NULL);
                continue;
            }

            timestamp_ns = get_time_ns();

            if (!proc->in_syscall) {
                /* ── SYSCALL ENTRY ── */
                proc->current_sys = (long)regs.orig_rax;

                profiler_record_entry(proc->current_sys, timestamp_ns);
                output_trace_entry(stopped_pid, proc->current_sys, &regs);

                proc->in_syscall = 1;

            } else {
                /* ── SYSCALL EXIT ── */
                long retval = (long)regs.rax;

                profiler_record_exit(proc->current_sys, timestamp_ns);
                output_trace_exit(proc->current_sys, retval);

                proc->in_syscall = 0;
            }

            ptrace(PTRACE_SYSCALL, stopped_pid, NULL, NULL);
            continue;
        }

        /* ---- Any other stop (signal delivery) ---- */
        /*
         * Deliver the signal to the tracee by passing it as the
         * fourth argument to PTRACE_SYSCALL. This is important —
         * if we pass NULL the process never receives the signal
         * and may hang (e.g. SIGCHLD, SIGPIPE).
         */
        {
            int sig = WSTOPSIG(status);
            /* Don't re-deliver SIGTRAP — it's ours */
            if (sig == SIGTRAP) sig = 0;
            ptrace(PTRACE_SYSCALL, stopped_pid, NULL, (void *)(long)sig);
        }
    }

    return 0;
}

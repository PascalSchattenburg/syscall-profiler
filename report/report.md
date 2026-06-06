# System Call Profiler & Tracer
## Technical Development Report

**Course:** Operating Systems
**Project type:** System call profiler and tracer (Linux x86-64, C, ptrace)

> This document is a technical development report and design document. It is
> the foundation from which the final PDF submission can be written: large
> sections are intended to be copied directly into the final report. It records
> the design, the implementation, the challenges encountered during
> development, the testing process, the results, and the lessons learned.
>
> The companion `README.md` is the user/developer handbook (how to build, run,
> and extend the tool). This report focuses on *why* the system is built the
> way it is and *how* it was developed.

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Motivation](#2-motivation)
3. [Project Goals](#3-project-goals)
4. [Operating System Background](#4-operating-system-background)
5. [System Calls Overview](#5-system-calls-overview)
6. [Why ptrace Was Chosen](#6-why-ptrace-was-chosen)
7. [Overall Architecture](#7-overall-architecture)
8. [Component Breakdown](#8-component-breakdown)
9. [Tracing Design](#9-tracing-design)
10. [Profiling Design](#10-profiling-design)
11. [Filtering Design](#11-filtering-design)
12. [Benchmark Design](#12-benchmark-design)
13. [Export Design](#13-export-design)
14. [Visualization Design](#14-visualization-design)
15. [Testing Strategy](#15-testing-strategy)
16. [Results](#16-results)
17. [Challenges Encountered](#17-challenges-encountered)
18. [Lessons Learned](#18-lessons-learned)
19. [Limitations](#19-limitations)
20. [Future Work](#20-future-work)
21. [Results Organization](#21-results-organization)
22. [Run ID System (Server Edition – Phase 001)](#22-run-id-system-server-edition--phase-001)

---

## 1. Introduction

System calls are the boundary between user programs and the operating system
kernel. Every meaningful interaction a program has with the outside world —
reading a file, allocating memory, opening a socket, creating a process —
crosses that boundary through a system call. Understanding which system calls a
program makes, how often, and how long they take is therefore a direct way to
understand both the program's behavior and the cost of the abstractions it
relies on.

For this project we implemented a system call profiler and tracer for Linux
x86-64, written in C, using the kernel's `ptrace()` facility. The tool launches
a target program, intercepts every system call at both its entry and its exit,
decodes the arguments and return value, measures the duration, classifies the
call into a category, and produces two complementary views: a real-time trace
(one line per call) and an aggregated statistical profile (counts, timing,
percentages, and category breakdown). Results can be exported to CSV and JSON,
and a Python script renders the JSON as charts plus a plain-language summary.

We aimed for something conceptually like a simplified combination of `strace`
(live tracing) and `strace -c` (aggregated statistics), with export and
visualization added. Because this is a teaching project and our own first
serious encounter with `ptrace`, we deliberately favored clarity and
correctness over raw performance throughout.

---

## 2. Motivation

Our motivation was educational and practical at once.

Most programmers, ourselves included, interact with the kernel only indirectly,
through library calls such as `fopen`, `malloc`, or `printf`. The system call
layer underneath stays invisible. By making that layer explicit, the tool turns
an abstract part of an operating systems course — "user space talks to the
kernel through system calls" — into something we could observe and measure
directly.

One concrete example motivated the whole project for us. Running the tool on a
trivial command like `ls` reveals dozens of system calls, the majority of which
are made by the dynamic linker loading shared libraries before `ls` executes a
single line of its own logic. Seeing this directly — counting the `openat`,
`mmap`, and `mprotect` calls that happen at startup — taught us the real cost of
dynamic linking and process startup far more effectively than a textbook
paragraph could.

Beyond teaching, the tool answers genuinely useful questions: what is a program
spending its system-call time on, which calls dominate, and which are slowest.
These are the same questions a performance engineer asks, in a simplified form.

---

## 3. Project Goals

The project was developed against a fixed set of goals, which also served as
acceptance criteria:

1. **Trace every system call** made by a target program, in real time, with
   readable output.
2. **Decode arguments and return values** for the most common system calls, so
   the trace is meaningful and not just a list of numbers.
3. **Measure execution time** per call and aggregate it into per-syscall
   statistics.
4. **Classify system calls** into categories so the overall behavior of a
   program is visible at a glance.
5. **Follow child processes and threads**, so multi-process and multi-threaded
   programs are profiled completely.
6. **Provide filtering** (`--only`, `--exclude`, `--top`) with the guarantee
   that all statistics stay consistent with the active filter.
7. **Export** the results to CSV and JSON for external analysis.
8. **Quantify the tracing overhead** with a benchmark mode, rather than
   hand-waving about it.
9. **Visualize** the results and produce a deterministic plain-language
   summary.
10. **Handle errors gracefully** — invalid filters and missing executables
    should fail cleanly with clear messages.
11. Keep the codebase **modular, readable, and reproducible**, with no exotic
    dependencies and no nondeterministic behavior.

---

## 4. Operating System Background

To explain the design, a few operating-system concepts need to be established.

**Privilege levels.** Modern CPUs run code at different privilege levels. On
x86-64, user programs run in an unprivileged level (ring 3) and the kernel runs
in the privileged level (ring 0). User code cannot directly touch hardware,
modify kernel memory, or perform I/O. This separation is what keeps a buggy or
malicious program from crashing or compromising the whole system.

**User space versus kernel space.** Memory and execution are partitioned into
user space (where ordinary programs live) and kernel space (where the operating
system lives). The two are isolated by hardware. The only legitimate way for
user space to request a privileged operation is to ask the kernel to do it on
its behalf — through a system call.

**Context switches.** When execution moves between processes, or between user
space and kernel space, the CPU must save one execution context and restore
another. Context switches are not free; each one costs on the order of
microseconds. This cost is central to understanding the overhead of `ptrace`,
because tracing forces extra context switches around every system call.

**Processes and threads.** A process is an instance of a running program with
its own address space. A thread is an independent flow of execution; on Linux,
threads are created with the `clone()` system call sharing the parent's address
space. Both processes and threads are created through closely related system
calls (`fork`, `vfork`, `clone`), which matters because a complete tracer must
follow all of them.

---

## 5. System Calls Overview

A system call is a controlled entry point into the kernel. On x86-64 Linux, the
calling convention is precise and is the basis for everything the tracer does:

| Register | Role |
|---|---|
| `RAX` | System call number on entry; return value on exit |
| `RDI` | Argument 1 |
| `RSI` | Argument 2 |
| `RDX` | Argument 3 |
| `R10` | Argument 4 |
| `R8`  | Argument 5 |
| `R9`  | Argument 6 |

To make a system call, a program places the call number in `RAX` and the
arguments in the registers above, then executes the `syscall` instruction. The
CPU switches to kernel mode and transfers control to the kernel's system call
dispatcher. When the kernel finishes, it places the result in `RAX` and returns
to user mode.

The return value convention is important for decoding failures: a successful
call returns a non-negative value (often a file descriptor or a byte count),
while a failed call returns a small negative number whose absolute value is an
`errno` code. For example, a return value of `-2` corresponds to `ENOENT` ("no
such file or directory"). The tracer uses this convention to turn raw negative
returns into readable error names.

There are roughly 350+ distinct system calls on Linux x86-64, each identified
by a stable integer. The project maps these numbers to names and categories in
a static table.

---

## 6. Why ptrace Was Chosen

Several mechanisms exist for observing system calls on Linux. The main
candidates considered were `ptrace`, `strace` (as a library or by parsing its
output), `LD_PRELOAD` interposition, `seccomp` with a user-space notifier, and
`eBPF`. `ptrace` was chosen for the following reasons.

**It is the canonical, well-documented mechanism.** `ptrace` is the same
facility that `strace` and debuggers like GDB are built on. It is thoroughly
documented and directly exposes exactly what the project needs: a stop at every
system call entry and exit, with access to the traced process's registers and
memory.

**It requires no kernel programming.** Unlike `eBPF`, which would require
writing and loading kernel-side bytecode and learning a substantial separate
toolchain, `ptrace` is a normal system call usable from an ordinary C program.
For a course project, this keeps the focus on operating-system concepts rather
than on a specialized tracing framework.

**It gives complete, per-call information.** `LD_PRELOAD` interposition only
intercepts library calls, not the underlying system calls, and misses anything
that bypasses libc. `ptrace` sees the actual system calls, including arguments
in registers and data in the traced process's memory.

**It teaches the relevant concepts directly.** Using `ptrace` forces the
implementation to engage with exactly the concepts the course is about: the
user/kernel boundary, the entry/exit nature of a system call, register
conventions, process control, and the cost of context switching.

The main drawback — significant runtime overhead from the extra context
switches — was accepted deliberately, and is itself turned into a teaching
point by the benchmark mode, which measures it. `eBPF` is noted as the natural
future direction for a low-overhead production version (see
[Future Work](#20-future-work)).

---

## 7. Overall Architecture

The system is organized into eight compiled C modules plus a standalone Python
visualizer. Each module has a single responsibility and a narrow interface.

```
                         User Program
                              |
                              |  makes a system call
                              v
                           ptrace()       (kernel stops the tracee at
                              |            syscall entry and exit)
                              v
                           Tracer  (tracer.c)
                              |
        +---------------------+----------------------+
        |                     |                      |
        v                     v                      v
     Decoder              Profiler                Output
   (decoder.c)          (profiler.c)            (output.c)
   decode args,         counts and              live trace +
   return values        timing per syscall      profile report
                                                     |
                                       +-------------+-------------+
                                       v             v             v
                                      CSV          JSON      terminal report
                                                    |
                                                    v
                                            Visualization
                                            (visualize.py)
                                                    |
                                       +------------+-----------+
                                       v            v           v
                                    charts      summary.txt   combined
                                    (PNG)                     report (PNG)
```

The **tracer** is the engine; it drives everything. It feeds two independent
consumers on every system call: the **profiler** (which only aggregates
statistics) and the **output** module (which only formats and prints). The
**decoder** and the **syscall table** are shared services used to turn raw
numbers into readable text. The **filter** sits beside output and answers a
single question — "should this syscall be shown?" — at every output decision
point. The **benchmark** module is a parallel execution path used only in
`--benchmark` mode. The **main** module parses the command line and wires
everything together.

The Python visualizer is decoupled from the C program entirely: it consumes the
JSON export, so it can be run at any later time on any saved profile.

---

## 8. Component Breakdown

This section describes each module's responsibility and its interactions.

**`main.c` — orchestration.** Parses the command line by hand, validates the
target executable before producing any output, builds the traced-command string
for the JSON export, then either runs benchmark mode or the normal trace
followed by the report and any requested exports.

**`tracer.c` — the ptrace engine.** Forks the target, enables tracing, and runs
the main event loop that handles a stop at every syscall entry and exit. It
records timestamps, drives the profiler and output, and follows children and
threads created by `fork`, `vfork`, and `clone`.

**`profiler.c` — statistics.** Maintains one record per distinct syscall number
(call count and total time) and computes derived values. It performs no I/O.

**`syscall_table.c` — the lookup table.** Maps syscall numbers to names and to
one of eight categories, and provides the category labels and ANSI colors. It
is a pure, stateless service used by the decoder, output, and filter.

**`decoder.c` — argument and return-value decoding.** Reads syscall arguments
from the tracee's registers and memory, formats paths and flags, and decodes
return values into success values or errno names.

**`output.c` — formatting and export.** Produces the live trace lines, the
profile report (counts, timing, percentages, category summary, slowest list),
and the CSV and JSON exporters. It consults the filter for every visible row.

**`filter.c` — filtering.** Implements `--only`, `--exclude`, and `--top`;
validates filter names against the syscall table; and exposes the single
predicate the output module asks.

**`benchmark.c` — overhead measurement.** Runs the target untraced and traced,
several times each, and reports the difference.

**`visualize.py` — visualization.** Reads the JSON export and produces charts, a
combined report image, and a deterministic behavioral summary.

A note on the source tree: an earlier argument-decoder pair, `args.c` /
`args.h`, remains in the repository but is **not** part of the build (it is not
listed in the Makefile). The active decoder is `decoder.c`. The legacy files
are kept for historical reference and are candidates for removal.

---

## 9. Tracing Design

The tracer is built around the entry/exit model that `ptrace` exposes.

**Startup.** The tracer calls `fork()`. The child calls
`ptrace(PTRACE_TRACEME, ...)` to mark itself as traceable and then `execvp()`s
the target program. The parent (the tracer) waits for the child's initial stop,
sets its `ptrace` options, and enters the main loop.

**The entry/exit state machine.** With `PTRACE_SYSCALL`, the kernel stops the
tracee twice per system call: once on entry (before the kernel does the work)
and once on exit (after). The tracer must distinguish these two stops, because
they mean different things. It keeps, per traced process, a flag indicating
whether the next stop is an entry or an exit:

- On an **entry** stop, the tracer reads the syscall number from `RAX`/`orig_rax`,
  records a start timestamp, and triggers the trace line (name + decoded
  arguments). It then flips the flag to "expecting exit."
- On an **exit** stop, the tracer reads the return value from `RAX`, records the
  end timestamp (computing the duration), and triggers the rest of the trace
  line (the return value). It flips the flag back to "expecting entry."

This alternation is the core of the tracer and the source of one of the main
implementation challenges (see [Challenges](#17-challenges-encountered)).

**Following children and threads.** Real programs spawn processes and threads.
To trace them, the tracer sets the `ptrace` options
`PTRACE_O_TRACEFORK`, `PTRACE_O_TRACEVFORK`, and `PTRACE_O_TRACECLONE`. When the
tracee creates a child through `fork`, `vfork`, or `clone` (the last of which is
what pthreads uses), the kernel automatically begins tracing the new child and
notifies the tracer. The tracer registers each new process or thread in a
fixed-size table and tracks each one's entry/exit state independently, because
several processes can be mid-syscall at the same time. Statistics from all
traced processes are aggregated into a single profile.

Two further options are set alongside these. `PTRACE_O_TRACEEXEC` makes the
tracee stop cleanly when it calls `execve()`, so the tracer keeps following a
process across a program replacement instead of losing it. `PTRACE_O_EXITKILL`
ensures that if the tracer exits, the kernel terminates the tracees rather than
leaving stopped processes behind. The tracer also uses `PTRACE_O_TRACESYSGOOD`
so that syscall stops can be distinguished from ordinary signal-delivery stops.

**Reading registers and memory.** On each stop, the tracer reads the tracee's
registers with `PTRACE_GETREGS`. String arguments (such as file paths) are
pointers into the tracee's address space, so they are read with `PTRACE_PEEKDATA`,
one word at a time, until a terminating null byte.

---

## 10. Profiling Design

The profiler is intentionally the simplest module: it does one thing, holds no
opinions about formatting, and performs no I/O.

It maintains an array of records, one per distinct syscall number encountered,
each holding the call count and the accumulated total time. The interface is
small:

- `profiler_record_entry(num, timestamp)` — called by the tracer at a syscall
  entry, remembers the start time for that syscall.
- `profiler_record_exit(num, timestamp)` — called at the matching exit,
  computes the duration and adds it to the running totals (incrementing the
  call count and the total time).
- `profiler_get_stats(&count)` — returns the finished array for the output
  module to format.
- `profiler_total_syscalls()` — returns the grand total of completed calls.

Average time per syscall is derived (total time divided by count) rather than
stored, so there is a single source of truth.

Keeping the profiler separate from the output module is a deliberate design
decision. The tracer feeds the profiler and the output module independently, so
the live trace and the aggregated statistics never interfere with each other,
and either can be changed without touching the other. It also means the
statistics are computed exactly once and consumed by every exporter (terminal,
CSV, JSON) identically.

One subtlety the profiling design must handle is system calls that are seen at
entry but never at exit — most importantly `exit_group`, which terminates the
program inside the kernel and never returns to user space. Such a call has a
recorded entry but no exit, so its count and duration remain zero. How this is
handled consistently across all outputs is discussed under
[Export Design](#13-export-design) and [Challenges](#17-challenges-encountered).

---

## 11. Filtering Design

Filtering lets the user restrict what the trace and report show. Three
independent options are provided:

- `--only=a,b,c` — show only the named syscalls.
- `--exclude=a,b` — hide the named syscalls.
- `--top=N` — limit the report table to the N most frequent rows.

`--only` and `--exclude` are mutually exclusive; combining them is rejected.

**The single-predicate design.** The central design decision is that filtering
is expressed through exactly one function, `filter_should_show(syscall_number)`,
which the output module calls at every point where it would display a syscall:
the live trace entry line, the live trace exit line, the per-syscall report
table, the category summary, and the slowest-syscall list. Because there is a
single definition of "should this appear," a filtered run is guaranteed to be
internally consistent — there is no way for one part of the report to disagree
with another.

This matters because an earlier version of the tool computed the report's
totals and percentages from the full data set even when a filter was active, so
the visible rows and the summary numbers disagreed. The single-predicate design,
combined with recomputing the totals over the filtered set, fixed this class of
bug at its root.

**Validation.** Filter names are validated against the syscall table when they
are parsed. A name that is not a real syscall (for example `--only=abc`) is
reported with a clear message and the program exits cleanly, rather than
silently matching nothing and producing a confusing empty report.

---

## 12. Benchmark Design

Benchmark mode (`--benchmark`) exists to make a single claim concrete and
measurable: *tracing with `ptrace` is expensive*. Rather than asserting this, the
tool measures it.

**Method.** The benchmark runs the target program two ways: untraced (a plain
`fork`/`exec` with no `ptrace`) and traced (under a minimal `ptrace` loop that
stops at every syscall but does no decoding or printing). Each mode is run
several times, and the **minimum** runtime of each is reported, along with the
overhead as a percentage and a multiplier.

**Why the minimum.** The minimum of several runs is used rather than the mean
because the minimum is the measurement least distorted by scheduler noise and
background activity. It best represents the underlying, uncontended cost. This
is the same reasoning used by serious micro-benchmarking tools.

**Why the overhead exists.** Each traced system call incurs two extra context
switches — one at the entry stop and one at the exit stop — plus the tracer's
own work to read registers and resume the tracee. The overhead is therefore
proportional to the number of system calls a program makes, not to its
wall-clock runtime. A program that makes many short system calls is slowed down
the most. The benchmark makes this relationship visible.

**Output hygiene.** During measurement, the target program's own standard
output and standard error are redirected away so they do not pollute the
benchmark's reported numbers.

---

## 13. Export Design

The tool exports results in two machine-readable formats, both derived from the
same profiler statistics so they never disagree.

**CSV export (`-c`).** Writes one row per completed syscall:

```
syscall_number,syscall_name,category,call_count,total_time_ms,avg_time_ms
```

CSV is the natural format for spreadsheets and quick scripting.

**JSON export (`--json`).** Writes a structured document containing the traced
program string and the per-syscall statistics:

```json
{
  "program": "find /usr -maxdepth 2",
  "total_syscalls": 922,
  "unique_syscalls": 25,
  "syscalls": [
    { "number": 257, "name": "openat", "category": "FILE",
      "count": 19, "total_ms": 0.357, "avg_ms": 0.0188 }
  ]
}
```

The `program` field records exactly what was traced, so the visualizer can label
its charts without the user retyping the command.

**Consistency across exports.** Both exporters apply the same rule for system
calls that were entered but never returned (such as `exit_group`): a call with a
count of zero is omitted. Critically, the JSON header values `total_syscalls`
and `unique_syscalls` are recomputed over completed calls only, so the header
agrees with the array, and both agree with the terminal report and the CSV.
Achieving this consistency — and the matching trailing-comma handling needed to
keep the JSON valid after skipping entries — was one of the development
challenges (see [Challenges](#17-challenges-encountered)).

---

## 14. Visualization Design

The Python visualizer (`visualize.py`) reads a JSON export and renders charts
plus a behavioral summary. It is deliberately decoupled from the C program: it
runs on a saved JSON file at any later time.

**Input: a file or a run directory.** The visualizer accepts either a
`profile.json` file directly (the original behavior) or a run directory. When we
give it a run directory, it locates `profile.json` inside that directory
automatically and, unless an explicit `--out` is supplied, writes all of its
output back into that same directory. We added this so visualization follows the
exact same "attach to an existing run" philosophy as `--benchmark-run`: instead
of typing the long form

```
python3 visualize.py results/ls/<timestamp>/profile.json --out results/ls/<timestamp>/
```

we can simply run

```
python3 visualize.py results/ls/<timestamp>/
```

and the charts and `summary.txt` land beside the `profile.json`, `results.csv`,
and `benchmark.json` that already belong to that run. If the directory has no
`profile.json`, the visualizer stops with a clear error rather than guessing.
The original file form and the explicit `--out` option are both unchanged, so
earlier workflows keep working exactly as before.

It produces:

- **`syscall_counts.png`** — a vertical bar chart of the most frequent syscalls
  by call count, colored by category.
- **`syscall_distribution.png`** — a sorted horizontal percentage bar chart of
  each syscall's share of total calls, with each row labeled by the syscall
  name and a short plain-language meaning (e.g. `openat · open file`).
- **`category_breakdown.png`** — a horizontal bar chart of total calls per
  category.
- **`slowest_syscalls.png`** — a horizontal bar chart of the syscalls with the
  highest average time, annotated to note that the times include `ptrace`
  overhead.
- **`syscall_report.png`** — a combined single-page image of all four charts
  plus the program name and totals.
- **`summary.txt`** — the deterministic behavioral summary.

**Distribution chart: from pie to bars.** The distribution view was originally a
pie chart. It proved hard to read once a program had many syscall types with
similar colors, and the legend-to-slice mapping was weak. It was replaced with a
sorted horizontal percentage bar chart, which is instantly readable: one labeled
row per syscall, the longest bar at the top, and the exact percentage printed at
the end of every bar.

**The "other" category.** The distribution chart shows the top N syscalls
individually and groups the remaining, lower-frequency syscall types into a
single bar labeled `other (N)`, where N is the number of grouped types (for
example `other (11)`). This group is intentionally preserved and never
discarded. It exists for statistical completeness: without it, the visible bars
would not sum to 100% of the observed system calls, and a reader could be misled
into thinking the program made fewer calls than it actually did. Including the
count in the label tells the reader exactly how many distinct types were folded
together. The grouping exists only to keep the chart readable, not to hide data.

**Behavioral summary: deterministic and rule-based.** The summary is generated
by fixed rules over the data, not by any AI model. It reports the dominant
category and its percentage, the counts of a few specific easy-to-explain
syscalls (opens, reads, writes, directory scans), a note about memory mapping
when it is significant, and an explanation of the "other" group, including how
many syscall types it contains and what share of all calls they represent. This
design is discussed further under [Lessons Learned](#18-lessons-learned); the
key properties are reproducibility (the same JSON always yields a byte-identical
summary), transparency (every sentence maps to a number and a rule), and the
absence of any external dependency, network call, or randomness.

---

## 15. Testing Strategy

We tested continuously during development, organized by the area we were
changing. The project includes a `make test` target for quick smoke tests, and
the categories below describe the broader manual testing process we followed.

**Build testing.** After every change, the project was rebuilt from clean with
`make clean && make`, compiling with `-Wall -Wextra` and treating warnings as
signals to fix. A clean build with no warnings was the baseline requirement
before any further testing.

**Feature testing.** Each feature was exercised with representative commands:

```bash
./profiler ls                       # basic tracing
./profiler -q find /usr -maxdepth 2 # quiet mode, larger trace
./profiler -n ls                    # no-color output
./profiler bash -c "ls | head -5"   # multi-process following
```

What was verified: that trace lines show category, decoded arguments, and
return values; that the report totals and category breakdown are present and
plausible; and that child processes are followed and aggregated.

**Filter testing.** Filtering was the area most prone to subtle inconsistency,
so it was tested specifically for *internal consistency*, not just for producing
output:

```bash
./profiler --only=openat,read,write -q ls   # total must equal the sum of shown rows
./profiler --exclude=mmap,mprotect,brk -q ls
./profiler --top=5 -q ls
./profiler --only=read,write --top=3 cat /etc/hostname
```

What was verified: that the total syscall count, unique-type count, category
totals, percentages, and the slowest-syscall list all reflect the filtered set
and agree with the visible rows; and that the slowest list never contains a
syscall excluded by the filter.

**Error handling testing.** The failure paths were tested explicitly:

```bash
./profiler --only=abc ls            # unknown filter name -> clean error, exit 1
./profiler does_not_exist           # missing executable -> clean error, no banner
```

What was verified: that an invalid filter name produces a clear message and a
non-zero exit code instead of an empty report; and that a missing executable
fails before any banner or trace UI is printed.

**Benchmark testing.** Benchmark mode was run on programs of different syscall
intensity (`true`, `ls`, `find`) to confirm that the reported overhead behaves
as expected — higher for syscall-heavy programs — and that the program's own
output does not leak into the benchmark display.

**Export testing.** CSV and JSON exports were generated and checked for
validity and consistency: the JSON was parsed to confirm it is well-formed, and
the header totals were compared against the array length and against the
terminal report to confirm they agree. The absence of zero-count entries (such
as `exit_group`) in both exports was verified.

**Results organization testing.** After adding the results hierarchy, we
verified directory creation and the no-overwrite guarantee with representative
commands:

```bash
./profiler ls                          # -> results/ls/<timestamp>/ exists
./profiler find /usr -maxdepth 1       # -> results/find/<timestamp>/ exists
./profiler ls ; ./profiler ls          # -> two timestamp dirs, no overwrite
./profiler --results-dir=/tmp/profiles ls   # -> /tmp/profiles/ls/<timestamp>/
./profiler --benchmark ls              # -> results/ls/<timestamp>/benchmark.json
```

What we verified: that each run creates `results/<program>/<timestamp>/`
containing `profile.json` and `results.csv` automatically; that running the same
program repeatedly produces separate timestamped directories and never
overwrites a previous run (including two runs within the same second, which fall
back to a `_2`, `_3`, … suffix); that `--results-dir` redirects the base
directory and creates it recursively; that `--json` and `-c` still write to
their explicit paths as before; and that a benchmark run additionally writes
`benchmark.json` without changing the benchmark's own console output.

**Visualization testing.** The visualizer was run on representative JSON files
and the generated PNGs were inspected. The behavioral summary was checked for
correctness against the underlying numbers, and was generated twice on the same
input to confirm the output is byte-for-byte identical (deterministic). The
`other (N)` label and its summary explanation were verified, including the edge
case where N is large enough that no "other" group exists. After centralizing
the distribution calculation, we specifically re-checked that the three outputs
agree: for `ls` at `--top=12`, the standalone distribution chart, the combined
report panel, and the summary must all report the same group — `other (8)` on
both charts and "remaining 8 syscall types" in `summary.txt`. We confirmed this
matches across all three before considering the fix complete.

---

## 16. Results

The tool was exercised on a range of programs. The observations below are
representative.

**Tracing `ls`.** A simple `ls` produces on the order of 70–80 system calls
across roughly 20 distinct types. The category breakdown is dominated by file
operations and memory operations. The striking result is that most of this
activity is *startup*, not the listing itself: the dynamic linker opens and
memory-maps several shared libraries (`openat`, `read`, `fstat`, `mmap`,
`mprotect`, `close`) before `ls` runs its own logic. The actual directory
listing is a small fraction of the total. This is the project's headline
teaching result — even a trivial command carries substantial kernel-level
startup cost.

**Tracing `find /usr -maxdepth 2`.** A directory walk produces close to a
thousand system calls across ~25 types, overwhelmingly file-related (on the
order of 96%). The dominant calls are `fcntl`, `close`, and `newfstatat`, which
is exactly what a directory traversal that opens, inspects, and closes many
entries should look like. This confirms that the category classification and
counts reflect real program behavior.

**Multi-process pipelines.** Running `bash -c "ls | head -5"` demonstrates
child-following: the shell forks child processes for the pipeline, each child is
automatically traced, the `execve` of the new program image is observed, and the
statistics from all processes are aggregated into one report. The presence of
IPC calls (the pipe) that plain `ls` does not make shows that process
composition changes the syscall profile.

**Threaded programs.** Running a program that creates pthreads shows the threads
being detected (each created through `clone`) and their system calls included in
the aggregated statistics.

**Timing observations.** Across runs, memory-protection calls such as
`mprotect` are consistently among the slowest on average, which is expected
because they modify page tables and can require TLB maintenance. Simple
identifier calls such as `getpid` are among the fastest. These relative
orderings are stable even though absolute timings vary, which is the correct way
to interpret the timing data given the overhead caveat.

**Overhead measurement.** Benchmark mode confirms a multi-fold slowdown under
tracing for syscall-heavy programs, consistent with the two-context-switches-per-call
model. The measured overhead scales with syscall count, as predicted.

---

## 17. Challenges Encountered

This section records the genuine difficulties we met during development,
including issues that surfaced during iterative refactoring. We kept it
deliberately detailed, because these challenges turned out to be the most
instructive part of the project for us.

**Understanding ptrace.** We initially assumed that tracing system calls would
simply involve reading syscall numbers as the program ran. In practice we had to
understand a much larger model: that `ptrace` is event-driven and hands control
to the tracer through `waitpid`, that options must be set only after the first
stop, and that each request (`PTRACE_SYSCALL`, `PTRACE_GETREGS`,
`PTRACE_PEEKDATA`) has its own semantics and failure modes. The documentation is
dense, and our early mistakes (such as setting options too early) produced
confusing behavior rather than clear errors, which cost us a lot of time before
the model clicked.

**Understanding waitpid behavior.** Closely related, we underestimated how
central `waitpid` is. Every stop — syscall entry, syscall exit, a new child
appearing, a process exiting — comes back to us through `waitpid`, and we had to
learn to decode its status value to tell these cases apart. Until we understood
that the status word encodes *why* the tracee stopped (and, for fork/clone
events, carries extra information in its upper bits), our loop either hung
waiting for the wrong thing or misclassified events.

**Syscall entry vs. exit states.** The single most error-prone part of the
tracer is that `ptrace` stops twice per system call, and the two stops look
almost identical. We learned the hard way that the *arguments* are available at
the entry stop while the *return value* is only available at the exit stop, so
we had to capture each at the right moment. The tracer must track, per process,
whether the next stop is an entry or an exit. Getting this wrong shifted every
return value onto the wrong call, or doubled our counts. The state had to be
kept per-process (not global), because once we added child-following, several
processes can each be mid-syscall simultaneously, and a single global flag
corrupted all of their states at once.

**Process lifecycle handling.** Following children correctly turned out to be
more complex than we expected. It required handling the full lifecycle:
detecting fork/clone events, registering the new process, distinguishing a
genuine new-process stop from an ordinary syscall stop, handling the new
process's own initial stop, and removing processes from the tracking table when
they exit. Threads (via `clone`) and processes (via `fork`) arrive through
related but distinct events, and we had to handle both without losing track of
the original process.

**Measuring execution time accurately.** Timing a system call means recording a
timestamp at the entry stop and another at the exit stop and taking the
difference. We learned to use a monotonic clock so the measurement is immune to
wall-clock adjustments. The deeper issue we ran into is that this measurement
*necessarily* includes `ptrace` overhead — the time the tracee spends stopped
while our tracer runs — so the numbers are upper bounds, not pure kernel times.
Rather than hide this, we documented it everywhere and added the benchmark mode
to quantify it.

**Handling failed syscalls.** A failed system call returns a negative errno
value in `RAX`. Decoding these correctly meant building an errno table and
distinguishing genuine errors from valid negative-looking return values.
Displaying `= -2 ENOENT` instead of a bare `-2` made our traces far more useful,
but it required care to map the right number to the right name.

**JSON consistency and the single source of truth.** Our visualization depends
entirely on the JSON export — the Python script never sees the live trace, only
the JSON. This made the JSON the single source of truth for everything
downstream, and it made consistency between the outputs genuinely important. The
most persistent class of bug we hit was inconsistency between different views of
the same data: the terminal report, the CSV, and the JSON all derive from the
same statistics, but each had its own formatting code, and it was easy for them
to drift — for instance, one view counting a zero-count `exit_group` entry that
another omitted.

**Synchronizing console, CSV, and JSON outputs.** Specifically, when we filtered
zero-count syscalls out of the exports, the JSON needed three coordinated fixes:
skip the zero-count entries, recompute the header totals (`total_syscalls`,
`unique_syscalls`) over the remaining entries so the header matches the array,
and fix the trailing-comma logic so that skipping the last entries does not leave
a dangling comma that breaks JSON validity. Missing any one of these produced
either inconsistent totals or invalid JSON, and we only caught the trailing-comma
case when a downstream parse failed.

**Filter correctness.** Making filtering correct everywhere was harder than it
first appeared. Our naive first approach filtered the visible trace rows but left
the totals and percentages computed over the full data set, so a filtered report
was internally contradictory. The fix was a single-predicate design plus
recomputing all summary statistics over the filtered set, applied uniformly to
the trace, the table, the category summary, and the slowest list.

**Invalid filter handling.** Initially an unknown syscall name in a filter was
silently accepted and simply matched nothing, producing an empty report that
looked like a bug. We added validation against the syscall table — and a clear
error message with a non-zero exit code — which turned a confusing silent
failure into a clear, actionable one.

**Missing executable handling.** Early on, running a nonexistent program printed
the full banner and trace interface and only then failed deep inside the child
with an `execvp` error, which looked alarming and left a half-rendered UI. We
moved the validation (including a `PATH` search for bare names) to *before* any
output is printed, so the tool now fails cleanly with a single message.

**Benchmark measurement noise.** Timing whole-program runs is inherently noisy
because of scheduling and background load. Averaging made our numbers swing
wildly between runs. Switching to the minimum of several runs, and suppressing
the target's own output during measurement, produced stable, meaningful figures.

**Visualization iterations and readability.** We evaluated several visualization
designs before settling on the current one. Our first distribution chart was a
pie chart, which became unreadable with many similar-colored slices and a weak
legend once a program made many distinct syscall types. We redesigned it as a
sorted horizontal percentage bar chart, with per-row labels and plain-language
meanings, which solved the readability problem. The same "too many syscall
types" pressure is what led us to introduce the `other` aggregation: grouping
the long tail of rare syscalls into a single labeled `other (N)` bar keeps the
chart readable while still accounting for every call.

**The "other" inconsistency we found during testing.** During testing we
discovered that different visualization components computed the "other" category
independently. The standalone distribution chart grouped everything beyond the
top N, but the combined report panel used a different effective limit
(`min(top_n, 10)`), and the summary used yet another path. This caused
inconsistent values between the standalone distribution chart, the combined
report, and the generated summary — for example, the same `ls` profile at
`--top=12` showed `other (8)` on one chart but `other (10)` on another, while the
summary said "remaining 8 syscall types". We resolved the issue by centralizing
the aggregation into a single shared helper, `compute_distribution_data(df,
top_n)`, that all three outputs now call, so the same input always produces the
same `other (N)` everywhere. This bug was a good lesson in how duplicated logic
drifts apart over time even when each copy looks correct on its own.

**Consistency between filtered and unfiltered statistics.** This deserves
separate mention because it recurred. Several distinct features (the report
table, the category summary, the slowest list, the header totals) each had to
respect the active filter in the same way. Each was a separate opportunity to
forget the filter and reintroduce inconsistency. Centralizing the decision in one
predicate was the structural fix, and the "other" bug above is the same lesson in
a different place.

**Refactoring challenges.** Several of the fixes above were refactors of code
that already worked for the simple case but broke once we generalized it — the
global-to-per-process state when we added child-following, the export totals when
we removed zero-count entries, the distribution logic when we unified the three
copies. We learned that each generalization tended to expose an assumption baked
into the original simple version.

**Testing and debugging complexity.** Debugging a tracer is awkward because the
thing being debugged is itself controlling another process via `ptrace`, and
adding print statements changes the timing. Much of our debugging relied on
small, targeted test programs (a deliberate fork, a deliberate set of pthreads, a
deliberately failing syscall) so that the expected output was known in advance
and deviations were obvious.

---

## 18. Lessons Learned

Coming into this project, none of us had used `ptrace` or thought much about
what happens below the C library. Writing the tracer changed how we think about
operating systems, and the lessons below are written from that
learning-as-we-went perspective.

### What we learned about ptrace

- **ptrace is event-driven, not a stream we read.** We came in imagining we
  would "read syscalls" in a loop. In reality the kernel stops the tracee and
  hands us control at specific events, and our job is to react to each event and
  resume. The whole tracer is shaped around that event loop, not around reading.
- **Every syscall produces an entry event and an exit event.** Understanding
  that these are two separate stops — arguments visible at entry, return value
  visible at exit — was the key insight that made the tracer work, and the thing
  we got wrong most often before it sank in.
- **waitpid behavior is crucial.** We learned that almost everything flows
  through `waitpid` and its status word, and that correctly decoding that status
  (syscall stop vs. fork/clone event vs. exit) is what makes the event loop
  reliable.
- **Process lifecycle handling is more complex than expected.** Following forks,
  vforks, clones, and thread creation, and cleaning up when each one exits, was
  far more involved than tracing a single process suggested it would be.
- **Tracing is tightly coupled to process state transitions.** The tracer is
  essentially a small state machine running in lockstep with the tracee's
  transitions between user mode, kernel mode, and process creation/exit. We had
  to think in terms of those transitions, not in terms of "the program's code".

### What we learned about operating systems

- **Even trivial programs execute a surprising number of syscalls.** Watching
  `ls` or `true` make dozens of calls made the cost of "just running a program"
  concrete in a way lectures never did.
- **Dynamic linking generates large amounts of startup activity.** A big share
  of the syscalls in a simple command happen before `main` runs at all, while
  the dynamic linker opens and maps shared libraries. This was genuinely
  surprising to us the first time we saw it.
- **Memory-related syscalls are very common.** `mmap`, `mprotect`, and `brk`
  show up constantly, mostly from library loading and the allocator, which made
  the memory-management part of the course feel much more real.
- **File access patterns become visible through tracing.** The sequence of
  `openat` / `read` / `close` calls effectively narrates what a program is doing
  with the filesystem, something we had only read about abstractly before.
- **Operating system abstractions are easier to understand when observed
  directly.** Seeing the user/kernel boundary crossed, call by call, turned
  "system calls" from a definition into something we had watched happen.

### What we learned about profiling

- **Measurement itself introduces overhead.** Our own tracer slows the traced
  program down, and the timings we record include that slowdown. Confronting
  this directly was an important lesson about the observer effect.
- **Absolute timings can be misleading.** Because of the overhead, the absolute
  millisecond figures are upper bounds, not ground truth, and presenting them as
  exact would have been wrong.
- **Relative timings are often more useful.** Comparing syscalls against each
  other within the same run (which is slowest, which dominates) is far more
  trustworthy than any single absolute number, and is usually what we actually
  wanted to know.
- **Visualization greatly improves interpretation.** The same numbers that were
  hard to read in a table became immediately understandable as sorted bars and a
  category breakdown. The charts did real explanatory work, not just decoration.
- **Profiling requires careful validation.** We could not simply trust our own
  output; we had to cross-check the totals, percentages, and the "other" group
  against each other and against hand calculations, which is how we caught the
  consistency bugs in the first place.

### Engineering lessons

**A single source of truth prevents whole classes of bugs.** The recurring
inconsistency bugs all had the same root cause: the same logical decision
(should this syscall be shown? what is the total? what goes into "other"?)
implemented in more than one place. Centralizing each decision — one filter
predicate, one statistics table, one distribution helper, recomputed totals —
eliminated the bugs structurally rather than patching them one at a time.

**Measure, do not assert.** It would have been easy to claim "ptrace is slow."
Building the benchmark mode turned that claim into a measured, reproducible
number, which is both more honest and more instructive.

**Determinism is a feature.** We kept the behavioral summary rule-based and
deterministic rather than AI-generated. The payoff is reproducibility (the same
input always gives the same output), transparency (every sentence traces back to
a number and a rule), and zero external dependencies. For a teaching tool that
must be explainable and that a grader should be able to reproduce exactly, this
was clearly the right trade-off over more fluent but unpredictable generated
text.

**Honesty about limitations strengthens the work.** Documenting that timings
include overhead, that the tool only runs on x86-64, and that tracing perturbs
the traced program makes the results more credible, not less. It also frames the
limitations as understood design consequences rather than oversights.

**Small test programs beat big test programs.** When debugging the tracer, the
most effective tests were tiny purpose-built programs whose expected system-call
behavior was known exactly. They made incorrect tracer behavior immediately
visible in a way that tracing a complex real program never could.

---

## 19. Limitations

The following limitations are inherent to the chosen design and are documented
deliberately.

**Linux x86-64 only.** The tool depends on `ptrace` and on the x86-64 register
layout (`orig_rax`, `rdi`, `rsi`, and so on). It does not build or run on other
architectures or operating systems. Porting would require an architecture
abstraction layer in the tracer and decoder.

**Timing includes ptrace overhead.** Measured durations bracket the kernel work
but also include the time the tracee is stopped while the tracer runs. The
numbers are therefore upper bounds, suitable for relative comparison between
syscalls but not as exact kernel execution times. The benchmark mode quantifies
this overhead.

**Scheduler timing inaccuracy.** The tracer is an ordinary user-space process
subject to scheduling. Delays between a tracee stopping and the tracer reading
its registers add noise to individual measurements, which averages out over many
calls but is present in any single one.

**The observer effect.** Tracing changes what it measures: a program running
several times slower under tracing exhibits different cache and timing behavior
than it would natively. The results describe the traced execution.

**Partial argument decoding.** Only the most common syscalls have full argument
decoding; the rest are shown without decoded arguments. Complete decoding would
require an entry for every syscall, including complex structure arguments.

**Bounded process table.** The number of simultaneously traced processes and
threads is limited by a fixed-size table; programs that exceed it would not be
fully followed.

**No attach to running processes.** Only programs the tool launches itself can
be traced; attaching to an already-running PID is not supported.

**No kernel-side aggregation.** Because all data passes through user space via
`ptrace`, the per-call overhead is intrinsic to the approach, regardless of
implementation quality. A fundamentally lower-overhead design would require a
different mechanism (see Future Work).

**Legacy files.** The unused `args.c` / `args.h` remain in the tree and should
be removed in a cleanup.

---

## 20. Future Work

The following are realistic extensions, none currently implemented.

**Improved child-process and thread tracking.** Per-process and per-thread
breakdowns in the report (rather than only aggregated statistics) and a dynamic,
unbounded process table would make the tool more useful on complex workloads.

**Network-related syscall analysis.** Decoding socket addresses and the full
set of network syscalls, with a dedicated network view, would turn the tool into
a useful aid for understanding networked programs.

**Interactive dashboard.** Replacing the static PNGs with an interactive web or
terminal dashboard would allow sorting, filtering, and drilling into categories
without regenerating images.

**Live monitoring mode.** A continuously updating view of syscall activity for
long-running programs, instead of a single end-of-run report, would support
observing services and daemons.

**eBPF backend.** The most significant future direction. An eBPF-based
collection backend aggregates data inside the kernel and avoids the
per-syscall context-switch overhead that `ptrace` imposes, enabling
low-overhead monitoring suitable for production systems. This would address the
single biggest limitation of the current design while keeping the same
reporting, export, and visualization layers on top.

**Advanced performance analytics.** Latency distributions (histograms and
percentiles) per syscall, time-series of activity over the run, and automatic
detection of unusual patterns would extend the tool from descriptive profiling
toward diagnostic analysis.

**Cleanup.** Remove the legacy `args.c` / `args.h` now that `decoder.c` is the
active implementation.

---

## 21. Results Organization

This section documents how we organize the artifacts produced by each run. We
implemented this as a dedicated results hierarchy after noticing, during our own
testing, that flat output files start to collide and overwrite each other as
soon as we profile more than one program or run the same program more than once.

### Why we introduced the hierarchy

Originally the tool wrote its exports to fixed filenames in the current
directory. The moment we ran `./profiler ls` twice, the second run silently
overwrote the first run's `profile.json` and `results.csv`. For a profiler whose
whole point is to capture and compare measurements, losing previous runs that
way was clearly wrong. We wanted every run to be a self-contained, reproducible
artifact that we could keep, archive, compare, and later feed into a dashboard
or upload to a server.

### The structure we implemented

Every run now creates one directory per traced program, and under it one
timestamped directory per run:

```
results/
│
├── ls/
│   ├── 2026-06-05_14-32-11/
│   │     profile.json
│   │     results.csv
│   │     summary.txt              (added by visualize.py)
│   │     syscall_report.png       (added by visualize.py)
│   │     syscall_counts.png       (added by visualize.py)
│   │     syscall_distribution.png (added by visualize.py)
│   │     category_breakdown.png   (added by visualize.py)
│   │     slowest_syscalls.png     (added by visualize.py)
│   │
│   ├── 2026-06-05_16-22-04/
│   │     ...
│
├── find/
│
├── curl/
│
└── python3/
```

The layout is always `results/<program>/<timestamp>/`. We derive the program
name from the traced command (so `/usr/bin/python3` becomes `python3`) and use
the timestamp format `YYYY-MM-DD_HH-MM-SS`. The C profiler writes `profile.json`
and `results.csv` into this directory automatically on every run; a benchmark
run instead writes `benchmark.json`. The base directory defaults to `results/`
and can be redirected with `--results-dir=<dir>`.

### Why timestamped runs are important

The timestamp is what makes each run distinct and non-destructive. Because we
create a fresh timestamped directory for every run, a new run can never overwrite
an older one. We also handle the edge case where two runs start within the same
second: rather than let them share a directory, we append a numeric suffix
(`_2`, `_3`, …) so each run still gets its own directory. This was a deliberate
correctness decision — the rule we set ourselves was that no run may ever
overwrite another run's artifacts, and the timestamp (plus suffix) is how we
guarantee it. The timestamp also ties each measurement to a point in time, which
is what makes a result reproducible and explainable after the fact.

### Linking a benchmark to a run

While building this out we hit a subtle organizational problem. The plain
`--benchmark` mode creates its own fresh timestamped directory, so if we
profiled `ls` at one moment and benchmarked it a minute later, the benchmark
landed in a *different* directory than the profile it actually belonged to. That
defeats the whole idea of a run being a single, self-contained unit of analysis.

To fix this we added a separate mode, `--benchmark-run <run-directory>`, that
attaches a benchmark to an existing run instead of starting a new one. It reads
the `program` field back out of that run's `profile.json`, reconstructs the
original command (handling quoted arguments so something like
`cat "my file.txt"` is rebuilt correctly), runs the existing benchmark
unchanged, and writes `benchmark.json` into the same directory. It deliberately
never creates a new timestamp directory and never touches `profile.json` or
`results.csv` — only `benchmark.json` is created or replaced. If the directory,
the `profile.json`, or the `program` field is missing, it stops with a clear
error and changes nothing.

The result is that one run is one directory containing everything that belongs
to it — the profile, the CSV, the benchmark, and (after visualization) the
summary and charts. This is exactly the shape a future WebUI or server would
want: open one run folder and find every related artifact in it.

### Visualizing a run

We extended the same idea to the visualizer. Originally, producing charts for a
run meant typing the JSON path and the output directory separately:

```
python3 visualize.py results/ls/<timestamp>/profile.json --out results/ls/<timestamp>/
```

That is verbose and easy to get wrong, and it does not match the clean
"point at the run" pattern we had just built for benchmarks. So we taught
`visualize.py` to accept a run directory directly:

```
python3 visualize.py results/ls/<timestamp>/
```

When given a directory, it finds `profile.json` inside it automatically and
writes the charts and `summary.txt` back into that same directory, unless an
explicit `--out` is given. This mirrors `--benchmark-run` exactly: you hand the
tool a run directory and it attaches its artifacts to that run. Passing a
`profile.json` file directly still works, so nothing we had before broke. With
this in place, the full workflow keeps every artifact for one run in one place:

```
./profiler ls                              # profile.json + results.csv
./profiler --benchmark-run results/ls/<timestamp>/   # + benchmark.json
python3 visualize.py results/ls/<timestamp>/         # + summary.txt + 5 PNGs
```

### How the structure improves maintainability

Keeping all of one run's artifacts together in a single directory means we never
have to reassemble which JSON, CSV, summary, and charts belong to which run —
they are co-located by construction. The path-management code that does this
lives entirely in `main.c` as a few small helper functions
(`make_timestamp`, `basename_of`, `mkdir_recursive`, `build_run_dir`, and a
uniqueness check), cleanly separated from the tracing, profiling, and export
logic, which we did not touch. That separation keeps the change easy to
understand and easy to maintain.

### How it supports future scaling

The layout is intentionally predictable and machine-readable, which sets up
several future directions cleanly:

- A future **WebUI** can enumerate `results/<program>/<timestamp>/` to list and
  display runs without any extra index.
- A future **server deployment** can map the same layout onto server-side
  storage and archive or upload runs straightforwardly.
- **Historical comparisons** between runs (or between programs) become a matter
  of reading two directories whose files line up by name.
- The grouping by program and then timestamp **scales** to large collections of
  runs while staying navigable.

We deliberately did not make the profiler run the Python visualizer itself: the
profiler writes `profile.json` and `results.csv`, and the recommended workflow
is to point `visualize.py --out` at the same run directory so the charts and
`summary.txt` land beside them. This keeps the C tool free of any Python runtime
dependency while still producing a complete artifact set per run.

---

## 22. Run ID System (Server Edition – Phase 001)

This section documents the first piece of work on the **server edition** of the
project. The university version was already complete and stable, and it worked
perfectly well without any of what follows — so it is worth being clear about
why we added this at all.

### The starting point: runs without IDs

In the version we submitted, a run was identified purely by where its files
lived: `results/<program>/<timestamp>/`. That was enough for us. When we wanted
to look at a run, we opened its folder; when we wanted to compare two runs, we
opened two folders. Nothing in the profiling, tracing, or benchmarking logic
needed a name for a run, and we did not give it one.

We only started to feel the limitation when we began thinking ahead to the
server edition — the WebUI, an API, and managing many runs over time. Those
future features do not browse folders by hand the way we do; they need to refer
to a specific run programmatically. A directory path like
`results/find/2026-06-06_12-02-14/` is awkward for that: it is long, it mixes
the program name and a timestamp together, and it is clumsy to put in a URL or
pass through an API. We wanted a single, compact handle for each run instead.

### Introducing Run IDs for future scalability

So in Phase 001 we gave every profiling run a **Run ID** — the string `run_`
followed by eight hexadecimal characters, for example `run_a8f3d21c`. It is
generated automatically when a run's directory is created, and then written
into that run's `profile.json` (and into `benchmark.json`). We were careful to
add it *in front of* the existing fields and to change nothing else, so the
files we already produce are byte-for-byte the same below the new metadata. The
profiler still traces, profiles, and benchmarks exactly as before; the Run ID is
just a label riding along.

One design point we had to get right was the benchmark. A benchmark is not a
separate thing from the run it measures — it belongs to that run. So when we
attach a benchmark to an existing run with `--benchmark-run`, it does **not**
mint a new Run ID; it reads the `run_id` and `timestamp` out of that run's
`profile.json` and reuses them. We also did not want this to break on the older
runs we had already generated (which have no Run ID yet), so for those we
reconstruct the metadata — generating an ID and recovering the timestamp from
the directory name — print a short note that we did so, and leave the old
`profile.json` untouched.

### A registry to simplify future server-side analysis

The second half of Phase 001 is a small catalog file, `results/runs_index.json`,
that lists every completed profiling run. Each run appends one short entry: its
Run ID, the program, the timestamp, the path to its directory, and two summary
counts (total and unique syscalls).

The reason for this is something we anticipated for the server edition: a WebUI
that wants to show a list of runs should not have to walk the entire results
tree and open thousands of directories just to build that list. Reading one
small index file is far cheaper and simpler. The registry is essentially a table
of contents for the results folder.

We were deliberate about what the registry is **not**. It is not a second copy
of the profiling data — it stores only metadata and a `path` pointing back to
the real run directory. The detailed syscall data still lives in exactly one
place, inside `results/<program>/<timestamp>/`. Keeping a single source of truth
avoids the classic problem of two copies drifting out of sync, and it keeps the
index small and quick to read. The two counts we do duplicate are only there so
a listing can show a run's size without opening it, and we compute them with the
same completed-syscall rule the JSON export uses, so they always agree.

We also decided that only complete profiling runs (the normal tracing commands)
get a registry entry. A plain `--benchmark` run produces only a `benchmark.json`
with no syscall data, so cataloguing it as a "run" would be misleading; we leave
it out. To make the index robust we only ever append to it, and we write it
atomically (to a temporary file that is then renamed into place) so that even an
interrupted run cannot corrupt the catalog.

### Why this was worth doing now

None of this changes what the profiler measures or how. It is purely
preparation. But getting the run identity and the catalog right early means the
later server-edition work — the API, the WebUI, run management, and historical
comparison — can build on a stable foundation instead of retrofitting IDs onto
data that was never designed to have them. It was a small, low-risk change with
a clear payoff for everything we plan to build next.

---

## References

1. Linux manual pages: `ptrace(2)`, `waitpid(2)`, `fork(2)`, `vfork(2)`,
   `clone(2)`, `execve(2)`, `execvp(3)`, `clock_gettime(2)`, `futex(2)`.
2. Linux kernel source: `arch/x86/entry/syscalls/syscall_64.tbl` (the
   authoritative syscall number table).
3. Linux x86-64 ABI documentation (calling convention and register usage).
4. Michael Kerrisk, *The Linux Programming Interface*, No Starch Press —
   chapters on processes, signals, and system call internals.
5. Daniel P. Bovet and Marco Cesati, *Understanding the Linux Kernel*,
   O'Reilly — system calls and signal handling.
6. The `strace` project — reference for syscall tracing behavior and argument
   decoding conventions.
7. Linux `perf` documentation — context for performance tracing approaches.
8. Brendan Gregg, *BPF Performance Tools*, Addison-Wesley — background for the
   eBPF future-work direction and low-overhead, kernel-side aggregation.

# System Call Profiler & Tracer

A Linux system call profiler and tracer written in C using `ptrace()`.
It runs a target program, intercepts every system call the program makes,
decodes the arguments and return values, measures how long each call takes,
classifies calls into categories, and produces both a live trace and a
statistical profile. Results can be exported to CSV and JSON, and a Python
script turns the JSON into charts and a plain-language behavioral summary.

This document is the project handbook. It is meant to let a new team member
read a single file and understand what the project does, why it exists, how
it works internally, how to build and run it, and how every feature is used.
It is **not** the final university report — that lives in `report/report.md`.

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Motivation](#2-motivation)
3. [Design Goals](#3-design-goals)
4. [Feature Overview](#4-feature-overview)
5. [Architecture Overview](#5-architecture-overview)
6. [Internal Components](#6-internal-components)
7. [Build Instructions](#7-build-instructions)
7a. [Server Setup & Quick Start (Virtual Environment)](#7a-server-setup--quick-start-virtual-environment)
8. [Usage Examples](#8-usage-examples)
9. [Filtering System](#9-filtering-system)
10. [Benchmark Mode](#10-benchmark-mode)
11. [CSV Export](#11-csv-export)
12. [JSON Export](#12-json-export)
13. [Visualization System](#13-visualization-system)
14. [Behavioral Summary Generation](#14-behavioral-summary-generation)
14a. [Results Management](#14a-results-management)
14b. [Phase 001 – Run ID System](#14b-phase-001--run-id-system)
14c. [Phase 002 – Dynamic Artifact Detection](#14c-phase-002--dynamic-artifact-detection)
14d. [Phase 003 – Run Registry Access Layer](#14d-phase-003--run-registry-access-layer)
15. [Project Structure](#15-project-structure)
16. [Source File Overview](#16-source-file-overview)
17. [Design Decisions](#17-design-decisions)
18. [Known Limitations](#18-known-limitations)
19. [Future Work](#19-future-work)
20. [Troubleshooting](#20-troubleshooting)

---

## 1. Project Overview

The System Call Profiler & Tracer is a command-line tool for Linux x86-64
that observes how a program interacts with the operating system kernel.

Every time a normal program reads a file, allocates memory, opens a network
socket, or starts another process, it does so through a **system call** — a
controlled entry point into the kernel. This tool launches a target program
under `ptrace()` control, stops it at the boundary of every system call, and
records what happened: which call, with what arguments, what it returned, and
how long it took.

The output has two parts:

- A **real-time trace**: one line per system call as it happens, showing the
  category tag, decoded name and arguments, and the return value.
- A **profile report**: aggregated statistics — per-syscall counts, total and
  average time, percentage of all calls, a category breakdown, and the slowest
  syscalls by average time.

The tool is conceptually similar to a simplified `strace` combined with the
aggregation you would get from `strace -c`, plus export and visualization on
top. It was built as a university Operating Systems project, so readability
and educational value were prioritized over raw performance.

---

## 2. Motivation

Most developers use system calls indirectly, through library functions like
`fopen`, `malloc`, or `printf`. The actual kernel interface stays invisible.
This project makes that interface visible and measurable.

Concretely, the tool answers questions like:

- Which system calls does a given program actually make, and how often?
- How much time is spent in each kind of system call?
- Is a program dominated by file I/O, memory management, or networking?
- Which system calls are the slowest on average?
- What does the kernel-level "shape" of a simple command like `ls` look like?

Running `ls` and discovering that it makes dozens of system calls — most of
them from the dynamic linker loading shared libraries before `ls` even starts
its own work — is a concrete, memorable lesson about the cost of abstractions.
That kind of insight is the motivation behind the project.

---

## 3. Design Goals

The project was built around a small set of explicit goals:

- **Correctness first.** The trace and the statistics must accurately reflect
  what the program actually did. A filtered report must be internally
  consistent (totals, percentages, and the slowest list all agree).
- **Readability over cleverness.** This is a teaching project. Each module has
  one responsibility, functions are small, and non-obvious decisions are
  explained in comments.
- **Modularity.** Tracing, profiling, decoding, output, filtering, and
  benchmarking are separate modules with narrow interfaces, so any one of them
  can be read or modified in isolation.
- **No exotic dependencies.** The C side uses only the standard library and
  Linux system headers. The Python visualizer uses only `matplotlib` and
  `pandas`.
- **Deterministic, reproducible output.** The same profiling run always yields
  the same statistics, the same exports, and the same behavioral summary. No
  randomness, no AI, no network calls.
- **Honest about limitations.** The tool measures traced execution, which is
  slower than native execution. Timing includes `ptrace` overhead, and the
  documentation says so rather than pretending the numbers are exact kernel
  times.

---

## 4. Feature Overview

- **Real-time syscall tracing** — every call printed as it happens, with a
  colored category tag, decoded arguments, and the return value.
- **Argument decoding** — file paths, flags (e.g. `O_RDONLY|O_CLOEXEC`),
  file descriptors, sizes, socket domains/types, and more for the most common
  syscalls.
- **Return value and errno decoding** — success values shown directly; failures
  shown with their errno name (e.g. `= -2 ENOENT`).
- **Syscall categorization** — every call is mapped to one of eight categories:
  `FILE`, `MEM`, `NET`, `PROC`, `SIG`, `IPC`, `TIME`, and `SYS` (other).
- **Execution time measurement** — each call timed using a monotonic clock,
  bracketing the kernel work between the entry and exit stops.
- **Profile report** — counts, total time, average time, percentage of calls,
  a category summary, and the top slowest syscalls by average time.
- **Multi-process and thread following** — children created by `fork`, `vfork`,
  and `clone` (including pthreads) are automatically traced and aggregated.
- **Filtering** — `--only`, `--exclude`, and `--top` restrict what the trace
  and report show, with all statistics staying consistent with the filter.
- **CSV export** — the per-syscall statistics as a spreadsheet-friendly file.
- **JSON export** — the same statistics plus the traced program string, ready
  for the visualizer or any external analysis.
- **Benchmark mode** — measures the overhead `ptrace` adds, comparing untraced
  versus traced runtime.
- **Visualization** — a Python script generates count, distribution, category,
  and slowest-syscall charts, a combined report image, and a `summary.txt`.
- **Behavioral summary** — a deterministic, rule-based plain-language
  description of what the program did.
- **Robust error handling** — invalid syscall filter names and missing
  executables are reported clearly and the tool exits cleanly without printing
  a misleading trace interface.

---

## 5. Architecture Overview

At a high level, control flows from the target program, through the kernel's
`ptrace` mechanism, into the tracer, which fans out to the decoder, profiler,
and output modules. Output in turn feeds the CSV, JSON, and (via JSON) the
visualization layer.

```
                         User Program
                              |
                              |  makes a system call
                              v
                           ptrace()           (kernel stops the program
                              |                at syscall entry and exit)
                              v
                           Tracer             (src/tracer.c)
                              |
        +---------------------+----------------------+
        |                     |                      |
        v                     v                      v
     Decoder              Profiler                Output
  (src/decoder.c)      (src/profiler.c)        (src/output.c)
   decode args,         count, total &          live trace +
   return values,       average time per        profile report
   errno names          syscall                      |
                                                     |
                                       +-------------+-------------+
                                       |             |             |
                                       v             v             v
                                      CSV          JSON      terminal report
                                       |             |
                                       |             v
                                       |       Visualization
                                       |       (visualize.py)
                                       |             |
                                       |   +---------+----------+
                                       |   |         |          |
                                       |   v         v          v
                                       | charts   summary.txt  combined
                                       |  (PNG)               report (PNG)
                                       v
                                  spreadsheet tools
```

The filter module (`src/filter.c`) sits beside the output module: the output
code asks the filter "should this syscall be shown?" at every decision point,
so filtering applies uniformly to the live trace, the report table, the
category summary, and the slowest list. The benchmark module
(`src/benchmark.c`) is a parallel path: it runs the program twice (untraced
and traced) using its own minimal `ptrace` loop and reports the difference.

---

## 6. Internal Components

This section describes what each compiled module is responsible for and how it
interacts with the others. The exact file list is in
[Source File Overview](#16-source-file-overview).

**Tracer** (`tracer.c`). The heart of the tool. It forks the target, sets up
`ptrace`, and runs the main event loop. For every syscall it receives two
stops from the kernel — entry and exit. On entry it records the start time and
asks the output module to print the call; on exit it records the duration and
the return value. It also follows new processes and threads created by `fork`,
`vfork`, and `clone`, keeping a small table of the processes it is tracing.

**Decoder** (`decoder.c`). Turns raw register values into human-readable text.
It reads the syscall arguments out of the traced process's registers and
memory, formats file paths and flags, and decodes return values (including
mapping negative returns to errno names like `ENOENT`). The tracer reaches the
decoder indirectly through the output module.

**Profiler** (`profiler.c`). Pure statistics. It maintains one record per
distinct syscall number: how many times it was called and the total time spent
in it. It does not print anything. The tracer feeds it entry and exit
timestamps; the output module reads the finished statistics.

**Syscall table** (`syscall_table.c`). A static lookup table mapping syscall
numbers to names and to one of eight categories, plus the category labels and
terminal colors. Used by the decoder, the output module, and the filter.

**Output** (`output.c`). All formatting lives here: the live trace lines, the
profile report, the category summary, the slowest-syscall list, and the CSV
and JSON exporters. It consults the filter for every visible row so that a
filtered run is internally consistent.

**Filter** (`filter.c`). Implements `--only`, `--exclude`, and `--top`. It
validates filter names against the syscall table and exposes a single
predicate, `filter_should_show()`, plus a description string for the banner.

**Benchmark** (`benchmark.c`). Implements `--benchmark`. It runs the target
untraced and then traced, several times each, and reports the runtime
difference as an overhead percentage.

**Main** (`main.c`). Argument parsing and orchestration. It interprets the CLI
flags, validates the target executable before doing anything visible, builds
the traced-program string for the JSON export, and calls the tracer followed
by the report and any requested exports.

**Visualizer** (`visualize.py`). A standalone Python script, not part of the C
build. It reads a JSON export and produces charts, a combined report image,
and a deterministic behavioral summary.

---

## 7. Build Instructions

### Prerequisites

- **Linux on x86-64.** The tool relies on `ptrace` and on the x86-64 register
  layout. It will not build or run on macOS, Windows, ARM, or other
  architectures.
- **GCC** (any version supporting C99) and **make**.

The visualizer (`visualize.py`) depends on **pandas** and **matplotlib**, listed
in `requirements.txt`. The profiler itself remains a pure C application with no
Python dependency — Python is only required for visualization. For the
recommended virtual-environment install, see
[Section 7a](#7a-server-setup--quick-start-virtual-environment).

### Building

```bash
unzip syscall_profiler.zip
cd syscall_profiler
make
```

The result is a single executable, `./profiler`, in the project root.

Useful make targets:

```bash
make            # build ./profiler
make clean      # remove compiled objects and the binary
make test       # build and run the built-in smoke tests
```

---

## 7a. Server Setup & Quick Start (Virtual Environment)

This section is the end-to-end path for deploying the project on a server: get
the code, set up an isolated Python environment for the visualizer, and build
the profiler. The actual day-to-day commands (tracing, benchmarking,
visualizing) are documented in their own sections — this one only covers
**setup** and then links to them, so each command is explained in exactly one
place.

> **Note on what needs Python.** Only the visualizer (`visualize.py`) needs
> Python and the packages in `requirements.txt` (`matplotlib`, `pandas`). The C
> profiler itself has **no** Python dependency — it is built with `make` and can
> profile and benchmark on its own. The virtual environment below exists purely
> to keep the visualizer's dependencies isolated from the system Python.

### 1. Clone the repository

```bash
git clone <repository-url>
cd syscall_profiler
```

### 2. Create and activate a virtual environment

A virtual environment keeps `matplotlib` and `pandas` local to this project
instead of installing them system-wide:

```bash
python3 -m venv .venv
source .venv/bin/activate
```

This creates a `.venv/` directory in the project root and activates it; your
shell prompt now shows `(.venv)`. To leave the environment later, run
`deactivate`.

### 3. Install the Python requirements

```bash
pip install --upgrade pip
pip install -r requirements.txt
```

This installs the visualizer's dependencies (`matplotlib`, `pandas`) into the
active environment.

### 4. Build the profiler

```bash
make
```

This produces the `./profiler` executable. Prerequisites and the available
make targets are in [Section 7](#7-build-instructions).

### Quick start — the full workflow

The complete sequence, from a fresh clone to a visualized run. Each command is
explained in detail in the section linked beneath the block:

```bash
git clone <repository-url>
cd syscall_profiler

python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install -r requirements.txt

make
./profiler ls                                      # profile a program
./profiler --benchmark-run results/ls/<timestamp>/ # benchmark that run
python3 visualize.py results/ls/<timestamp>/       # charts + summary
```

Replace `<timestamp>` with the actual run directory name (printed as
`Results:` when you profile). For what each command does and its full options,
see [Usage Examples](#8-usage-examples), [Benchmark Mode](#10-benchmark-mode),
[Results Management](#14a-results-management) (the `--benchmark-run` workflow),
and [Visualization System](#13-visualization-system).

---

## 8. Usage Examples

The general form is:

```
./profiler [OPTIONS] <program> [program-args...]
```

All options must come before the target program. Short options can be
combined (`-qn` is the same as `-q -n`).

| Option | Meaning |
|---|---|
| `-q` | Quiet mode — suppress the live trace, show only the report |
| `-n` | No color — plain text output (good for piping to a file) |
| `-c <file>` | Export the statistics to a CSV file |
| `-h` | Show help and exit |
| `--only=a,b,c` | Show only these syscalls (trace + report) |
| `--exclude=a,b` | Hide these syscalls (trace + report) |
| `--top=N` | Show only the top N rows in the report table |
| `--json=<file>` | Export the statistics (and program name) to JSON |
| `--results-dir=<dir>` | Base directory for run artifacts (default `results/`); each run is saved to `<dir>/<program>/<timestamp>/` |
| `--benchmark` | Measure `ptrace` overhead instead of tracing normally |
| `--benchmark-run <dir>` | Attach a benchmark to an existing run directory (writes `benchmark.json` into it) |

### Basic tracing

```bash
./profiler ls
./profiler ls -la /usr/bin
./profiler cat /etc/hostname
```

A trace line looks like this:

```
[FILE] openat     (AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
[MEM ] mmap       (length=4096, PROT_READ|PROT_WRITE) = 140234...
[FILE] access     ("/etc/ld.so.preload", R_OK) = -2 ENOENT
```

### Quiet mode (report only)

```bash
./profiler -q find /usr -maxdepth 2
```

### Multi-process and threads

```bash
# bash spawns child processes — all are followed automatically
./profiler bash -c "ls | head -5"

# a pthreads program — threads are detected and aggregated
./profiler ./my_threaded_program
```

### Exports and visualization

```bash
./profiler -c results.csv -q ls                 # CSV
./profiler --json=profile.json -q ls            # JSON
python3 visualize.py profile.json               # charts + summary
```

---

## 9. Filtering System

Filtering narrows what the tool shows, both in the live trace and in the
report. There are three independent options.

**`--only=a,b,c`** shows only the named syscalls and hides everything else.

```bash
./profiler --only=openat,read,write,close ls
```

**`--exclude=a,b`** hides the named syscalls and shows everything else. This
is useful for removing high-volume noise like memory management calls.

```bash
./profiler --exclude=mmap,mprotect,brk,munmap -q ls
```

**`--top=N`** limits the report table to the N most frequent syscalls. It is a
display limit on the table only; it does not change the totals.

```bash
./profiler --top=5 -q ls
```

Options can be combined (except `--only` and `--exclude`, which are mutually
exclusive):

```bash
./profiler --only=read,write --top=3 cat /etc/hostname
```

**Consistency guarantee.** When a filter is active, every summary statistic
reflects the filtered data set: the total syscall count, the unique-type
count, the per-category totals, the percentages, and the slowest-syscall list
all agree with each other and with the visible rows. This consistency was a
specific correctness goal; an earlier version showed filtered rows but
unfiltered totals, which was misleading and has been fixed.

**Invalid filter names.** If a filter contains a name that is not a real
syscall, the tool reports it and exits cleanly rather than silently producing
an empty report:

```bash
$ ./profiler --only=abc ls
  Error: unknown syscall filter: abc
```

---

## 10. Benchmark Mode

`--benchmark` measures how much overhead `ptrace` tracing adds, by comparing
untraced and traced runtime of the same program.

```bash
./profiler --benchmark ls
./profiler --benchmark find /usr -maxdepth 2
```

The tool runs the program several times without tracing and several times with
a minimal tracing loop, then reports the difference:

```
Normal (untraced)         1.145 ms
Traced (with ptrace)      3.582 ms
Overhead:                 +213%  (3.1x slower)
```

It reports the minimum of several runs rather than the average, because the
minimum is the least affected by scheduler noise and best represents the
underlying cost. The program's own output is suppressed during measurement so
it does not pollute the timing display.

The overhead is proportional to the number of system calls, not to wall-clock
runtime: each call costs two extra context switches (entry stop and exit stop)
plus the tracer's work. A program that makes many short syscalls per second is
slowed down the most.

---

## 11. CSV Export

`-c <file>` writes the per-syscall statistics to a CSV file suitable for
spreadsheets or quick scripting.

```bash
./profiler -c results.csv -q ls
```

The columns are:

```
syscall_number,syscall_name,category,call_count,total_time_ms,avg_time_ms
```

One row per distinct syscall that completed at least once. Syscalls that were
entered but never returned (such as `exit_group`, which never comes back to
user space) have a call count of zero and are intentionally omitted, so the
CSV is consistent with the terminal report.

---

## 12. JSON Export

`--json=<file>` writes the statistics in a structured JSON document, plus the
traced program string. This is the input the visualizer consumes, and it is
also convenient for any external analysis.

```bash
./profiler --json=profile.json -q find /usr -maxdepth 2
```

The document looks like this:

```json
{
  "program": "find /usr -maxdepth 2",
  "total_syscalls": 922,
  "unique_syscalls": 25,
  "syscalls": [
    {
      "number": 257,
      "name": "openat",
      "category": "FILE",
      "count": 19,
      "total_ms": 0.357,
      "avg_ms": 0.0188
    }
  ]
}
```

The `program` field records the exact command that was traced, so charts and
summaries can show it without the user retyping it. The `total_syscalls` and
`unique_syscalls` header values are computed over completed syscalls only, so
they match the array and the terminal report exactly (the same consistency
rule as the CSV export).

---

## 13. Visualization System

`visualize.py` reads a JSON export and produces a set of PNG charts plus a
text summary. It is a standalone script and does not require the C tool to be
running.

It accepts **either** a `profile.json` file **or** a run directory. When given a
run directory, it finds `profile.json` inside it and writes its output back into
that same directory automatically:

```bash
# Step 1: produce a run directory (profile.json + results.csv)
./profiler -q find /usr -maxdepth 2

# Step 2a: render charts and summary — pass the run directory (recommended)
python3 visualize.py results/find/<timestamp>/

# Step 2b: or pass a profile.json file directly (also works)
python3 visualize.py profile.json
```

Options:

| Option | Meaning |
|---|---|
| `--top=N` | How many syscalls to show individually before grouping the rest into "other" (default 15) |
| `--out=DIR` | Output directory for the generated files (default: the run directory, or next to the JSON file) |
| `--no-combined` | Skip the combined overview image |
| `--program=CMD` | Override the program label (normally read from the JSON `program` field) |

It generates the following files:

- **`syscall_counts.png`** — vertical bar chart of the most frequent syscalls
  by call count, colored by category.
- **`syscall_distribution.png`** — a sorted horizontal percentage bar chart
  showing each syscall's share of the total. Each row is labeled with the
  syscall name and a short plain-language meaning (e.g.
  `openat · open file`). This chart replaced an earlier pie chart, which was
  hard to read when many slices had similar colors.
- **`category_breakdown.png`** — horizontal bar chart of total calls grouped
  by category (FILE, MEM, NET, and so on).
- **`slowest_syscalls.png`** — horizontal bar chart of the syscalls with the
  highest average execution time, with a note that these times include
  `ptrace` overhead.
- **`syscall_report.png`** — a single combined image with all four charts plus
  the program name and totals, suitable for a slide or report page.
- **`summary.txt`** — the behavioral summary described in the next section.

### The "other" category

The distribution chart shows the top N syscalls individually and groups all
remaining syscall types into a single bar labeled `other (N)`, where N is the
number of grouped types (for example `other (11)`).

This "other" group is **intentionally preserved** and is not discarded. It
exists for statistical completeness: without it, the visible bars would not add
up to 100% of the observed system calls, and a reader could be misled into
thinking the program made fewer calls than it really did. Grouping the
low-frequency tail into one labeled bar keeps the chart readable while still
accounting for every call. The label includes the count so the reader knows
exactly how many distinct syscall types were folded into it.

The "other" count is computed in one place — a single shared helper,
`compute_distribution_data(df, top_n)` — that the standalone distribution
chart, the combined report's distribution panel, and the behavioral summary
all call. Because there is exactly one calculation, the same JSON input and the
same `--top` value always produce an identical `other (N)` value and identical
percentage everywhere. (An earlier version computed the group separately in
each place and could disagree — for example showing `other (8)` on one chart
and `other (10)` on another; centralizing the calculation fixed that.)

---

## 14. Behavioral Summary Generation

Alongside the charts, the visualizer writes `summary.txt`, a short
plain-language description of what the program did. A typical summary:

```
Program traced: find /usr -maxdepth 2

Most activity was file-system related (96%).
The program opened files 19 times, read data 7 times, wrote data 8 times,
and scanned directories 25 times.
Memory mapping activity (17 mmap calls) was mostly caused by loading shared
libraries at startup.
The remaining 15 syscall types were grouped into 'other' and accounted for
2.6% of all observed system calls. These are lower-frequency operations
grouped together to keep the visualization readable.

In total: 922 syscalls across 25 distinct types.
```

### Deterministic, rule-based generation

The summary is produced by **deterministic, rule-based logic**, not by an AI
model. It is built directly from the numbers in the JSON: the dominant
category and its percentage, the counts of a few specific easy-to-explain
syscalls (opens, reads, writes, directory scans), a note about memory mapping
when it is significant, and the description of the "other" group.

This choice is deliberate. Deterministic generation gives three properties
that matter for a teaching tool:

- **Reproducibility.** The same profiling JSON always produces the exact same
  summary. Running the visualizer twice on the same data yields byte-identical
  `summary.txt` files. There is nothing to seed, configure, or re-roll.
- **Transparency.** Every sentence in the summary can be traced back to a
  specific number in the data and a specific rule in the code. There is no
  hidden model whose output cannot be explained.
- **No external dependencies.** The summary needs no AI model, no LLM, no
  network API, and no API key. It works offline and will keep working
  unchanged for as long as Python, matplotlib, and pandas exist.

For an Operating Systems course, being able to explain exactly why the summary
says what it says is more valuable than the more fluent but unpredictable text
an AI model might produce.

---

## 14a. Results Management

Every profiling run automatically organizes its output into a structured
results hierarchy, so runs never overwrite each other and each run is a
self-contained, reproducible artifact set.

### Directory hierarchy

For every run the profiler creates:

```
results/
│
├── ls/
│   ├── 2026-06-05_14-32-11/
│   │     profile.json
│   │     results.csv
│   │     summary.txt              (added by visualize.py)
│   │     syscall_counts.png       (added by visualize.py)
│   │     syscall_distribution.png (added by visualize.py)
│   │     category_breakdown.png   (added by visualize.py)
│   │     slowest_syscalls.png     (added by visualize.py)
│   │     syscall_report.png       (added by visualize.py)
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

The hierarchy is always `results/<program>/<timestamp>/`. The program name is
derived from the traced command (for example `/usr/bin/python3` becomes
`python3`), and the timestamp uses the format `YYYY-MM-DD_HH-MM-SS`.

### Timestamped runs

Every run creates a new timestamped directory; an existing run directory is
never reused. The C profiler writes two artifacts into it automatically, on
every run, even without any export flags:

- `profile.json` — the full statistics plus the traced program string
- `results.csv` — the same statistics in spreadsheet form

If a benchmark run is performed (`--benchmark`), it instead writes:

- `benchmark.json` — the untraced/traced times and the derived overhead

In the rare case that two runs start within the same second and would land on
the same timestamp, the profiler appends a numeric suffix (`_2`, `_3`, …) so the
two runs still get separate directories. This guarantees that no run can ever
overwrite the artifacts of a previous run.

### Choosing the base directory

The base directory defaults to `results/` and can be changed with
`--results-dir=<dir>`:

```bash
./profiler ls
# -> results/ls/<timestamp>/

./profiler --results-dir=/mnt/data/profiles ls
# -> /mnt/data/profiles/ls/<timestamp>/
```

Missing directories are created automatically (recursively), so the base
directory does not need to exist beforehand.

### Linking a benchmark to an existing run

A profiling run is the central unit of analysis: ideally every artifact that
belongs to one run — `profile.json`, `results.csv`, the charts, `summary.txt`,
and the benchmark — lives together in that run's directory.

The plain `--benchmark` mode creates its own fresh run directory, which means a
benchmark taken separately from a profiling run ends up in a *different*
timestamped folder than the profile it relates to. To keep everything for one
run together, use `--benchmark-run` to attach a benchmark to an existing run:

```bash
# 1. Profile a program (creates results/ls/<timestamp>/)
./profiler ls

# 2. Benchmark THAT run — writes benchmark.json into the same directory
./profiler --benchmark-run results/ls/<timestamp>/
```

This reads the `program` field from `<run-directory>/profile.json`,
reconstructs the original command (quoted arguments are handled correctly), runs
the existing benchmark, and writes `benchmark.json` into that same directory. It
does **not** create a new timestamp directory, and it never modifies
`profile.json` or `results.csv` — only `benchmark.json` is created or replaced.

If the run directory does not exist, has no `profile.json`, or that file has no
`program` field, the command stops with a clear error and changes nothing.

Keeping the benchmark in the same run directory means one run equals one
directory equals all of its artifacts, which makes runs easier to navigate and
reproduce, simplifies historical comparisons, and prepares the layout for a
future WebUI, server deployment, and API that open a single run folder and find
everything in it.

### Recommended visualization workflow

Visualization remains a separate, optional step — the profiler never runs
Python itself and adds no runtime dependency. The recommended workflow writes
the charts and summary into the *same* run directory, so everything for one run
stays together.

The simplest way is to pass the **run directory** to the visualizer. It locates
`profile.json` inside that directory automatically and writes all of its output
back into the same directory — no `--out` needed:

```bash
# 1. Profile — creates results/ls/<timestamp>/ with profile.json + results.csv
./profiler ls

# 2. Visualize — just point it at the run directory (path printed as "Results:")
python3 visualize.py results/ls/<timestamp>/
```

This follows the same "attach to an existing run" design as
`--benchmark-run`: you hand the visualizer a run directory, and it adds its
artifacts to that run instead of scattering them elsewhere. When a directory is
given, the visualizer automatically discovers `profile.json` inside it and uses
that directory as the output location.

If the directory has no `profile.json`, the visualizer stops with a clear error:

```
Error:
profile.json not found in run directory:
results/ls/<timestamp>/
```

The explicit form still works too, and is unchanged:

```bash
python3 visualize.py results/ls/<timestamp>/profile.json \
    --out results/ls/<timestamp>/
```

After visualization, `results/ls/<timestamp>/` contains the complete set:
`profile.json`, `results.csv`, `summary.txt`, and the five PNG charts.

### Why this structure exists

- **Prevents overwriting previous runs.** Each run has its own timestamped
  directory, so profiling the same program repeatedly never destroys earlier
  results.
- **Groups all artifacts of one run.** The JSON, CSV, summary, charts, and any
  benchmark result for a single run live together in one directory.
- **Improves reproducibility.** The timestamp ties each result to a specific
  point in time, making past measurements easy to locate and explain.
- **Supports historical comparisons.** With a directory per program and a
  sub-directory per run, comparing two runs of `find`, or `ls` against `curl`,
  is just opening two folders side by side; files line up by name.
- **Simplifies future WebUI development.** A predictable, machine-readable
  layout (`results/<program>/<timestamp>/`) is straightforward for a future web
  interface to enumerate, index, and display.
- **Simplifies future server deployment.** The same predictable layout maps
  cleanly onto server-side storage and makes runs easy to archive or upload.
- **Scales to large collections of runs.** Grouping by program and then by
  timestamp keeps even thousands of runs organized and navigable.
- **Improves long-term maintainability.** Output organization is handled in one
  place with clear, minimal path-management code, separate from the tracing and
  profiling logic.

---

## 14b. Phase 001 – Run ID System

> This section documents the **server-edition** Phase 001 work. It is purely
> additive metadata: it does **not** change tracing, profiling, decoding,
> benchmarking, filtering, visualization, the export formats, or the
> `results/<program>/<timestamp>/` hierarchy described in 14a. Everything that
> worked before works identically; Phase 001 only labels runs and catalogs them.

Phase 001 prepares the project for future server-side use — a WebUI, an API,
run management, and historical analysis — by giving every profiling run a
stable identity and adding a lightweight catalog of runs. No databases, web
servers, or networking are introduced; this phase is metadata only.

### What a Run ID is

A **Run ID** is a short unique label assigned automatically to each profiling
run, in the form `run_` followed by eight hexadecimal characters:

```
run_a8f3d21c
run_91bc44ff
run_fa8123de
```

It is generated once, when a new run directory is created, and then recorded
in that run's artifacts. The ID carries no meaning of its own — it is simply a
stable handle that uniquely identifies one run.

### Where the Run ID appears

The Run ID (and the run's timestamp) are added to the **front** of the two
JSON artifacts, ahead of the existing fields. Nothing else in these files
changes:

```json
{
  "run_id": "run_a8f3d21c",
  "timestamp": "2026-06-06_11-24-36",
  "program": "ls",
  "total_syscalls": 79,
  "unique_syscalls": 20,
  "syscalls": [ ... ]
}
```

`benchmark.json` gains the same two leading fields.

### Why it exists

Until now a run was identified only by its directory path
(`results/<program>/<timestamp>/`). That works for browsing files by hand, but
it is awkward for software: paths are long, contain a program name and a
timestamp glued together, and are clumsy to pass through a URL or an API call.
A Run ID gives every run a single, compact, stable identifier that a future
WebUI or API can use as a key — for example to request one run, link to it, or
compare two runs — without parsing directory paths.

### How a benchmark relates to a Run ID

A benchmark **belongs to** the profiling run it measures, so it must share that
run's identity. When a benchmark is attached to an existing run with
`--benchmark-run <run-directory>`, it **inherits** the `run_id` and `timestamp`
from that run's `profile.json` and writes them into `benchmark.json`. A
benchmark never invents a new Run ID for an existing run.

For older runs created before Phase 001 — whose `profile.json` has no `run_id`
or `timestamp` yet — `--benchmark-run` does not fail. It reconstructs the
metadata (generating a Run ID, and recovering the timestamp from the run
directory's name), prints a short note that it did so, and leaves the original
`profile.json` untouched.

### The run registry: `results/runs_index.json`

Alongside the per-run directories, the profiler maintains a single catalog
file at the top of the results tree:

```
results/
├── runs_index.json        <- the registry (catalog of all runs)
├── ls/
│   └── 2026-06-06_11-24-36/
│       ├── profile.json
│       └── ...
├── find/
└── ...
```

Each completed profiling run appends one entry:

```json
[
  {
    "run_id": "run_a8f3d21c",
    "program": "ls",
    "timestamp": "2026-06-06_11-24-36",
    "path": "results/ls/2026-06-06_11-24-36",
    "total_syscalls": 79,
    "unique_syscalls": 20
  },
  {
    "run_id": "run_b719fa2e",
    "program": "find",
    "timestamp": "2026-06-06_12-02-14",
    "path": "results/find/2026-06-06_12-02-14",
    "total_syscalls": 1321,
    "unique_syscalls": 34
  }
]
```

### Why the registry contains metadata only

The registry is deliberately **not** a second copy of the profiling data. It
stores only a handful of fields per run — the Run ID, program name, timestamp,
the path to the run directory, and two summary counts. The full syscall data
remains stored in exactly one place: inside `results/<program>/<timestamp>/`.

This keeps the design honest and avoids duplication. The detailed data has a
single source of truth (the run directory); the registry is just an index that
*points* at it. The `path` field is the link from a catalog entry back to the
complete artifact set. The two counts (`total_syscalls`, `unique_syscalls`) are
included only so a listing can show a run's size at a glance without opening it;
they are computed with the same completed-syscall rule as `profile.json`, so
they always match.

### Which runs are catalogued

Only **complete profiling runs** — the normal tracing commands — append a
registry entry:

```bash
./profiler ls
./profiler -q ls
./profiler find /usr -maxdepth 1
```

A plain `--benchmark` run is **not** added to the registry. It produces a
`benchmark.json` but no `profile.json`, no syscall totals, and no full profiling
data, so it is not a profiling run the catalog should list. Likewise
`--benchmark-run` only attaches a benchmark to an existing run and does not
create a new catalog entry.

The registry is written atomically (to a temporary file that is then renamed
into place) and is only ever appended to, so existing entries are never
overwritten and an interrupted write cannot corrupt the catalog.

### Future WebUI and API benefits

The catalog is the foundation for the server edition's future features:

- **Fast listing.** A WebUI or API can read one small file,
  `results/runs_index.json`, to list every run, instead of recursively scanning
  thousands of directories on disk.
- **Stable addressing.** Each run has a Run ID that a URL or API endpoint can
  use directly (for example to fetch or link to a single run).
- **Historical analysis.** With every run catalogued by program and time, the
  server side can compare runs, track a program's behavior over time, or filter
  runs without touching the bulk syscall data.
- **Clean separation.** Because the catalog only indexes and never duplicates,
  the heavy data stays on disk in the run directories and the catalog stays
  small and quick to read — which scales naturally to a large collection of
  runs.

---

## 14c. Phase 002 – Dynamic Artifact Detection

> Like Phase 001, this is **server-edition** infrastructure for a future
> WebUI/API. It adds one small, read-only helper module and changes nothing
> else: tracing, profiling, benchmarking, the JSON/CSV export formats, the
> visualization step, and the `runs_index.json` format are all untouched, and
> no user-facing command is added. The module compiles into the project and is
> available for future callers, but is not yet exposed to users.

Phase 002 answers a question the registry deliberately does **not**: for a given
run, which artifacts exist *right now* — `profile.json`, `benchmark.json`, and
the visualization?

### Registry metadata stays immutable; artifact state is derived

The Phase 001 registry (`runs_index.json`) stores only facts that are fixed when
a run is created — `run_id`, `program`, `timestamp`, `path`, and the syscall
counts. Those never change, so the registry never needs updating and can never
go stale.

Artifact presence is different: it **changes over the life of a run**. A run may
start with only `profile.json`, gain `benchmark.json` hours later from
`./profiler --benchmark-run <dir>`, and gain `syscall_report.png` later still
when the visualizer runs. If the registry stored flags like `has_benchmark`,
every one of those later actions would have to remember to rewrite the registry,
and any path that forgot would leave it lying about what is on disk.

Phase 002 avoids that whole class of bug by **never storing artifact state**. It
is computed from the filesystem at the moment it is asked for. The filesystem is
the single source of truth for "what exists now"; there is no second copy to
drift, so **stale artifact metadata is impossible**.

### What is detected

The module inspects a run directory directly (using `stat()` only — no JSON
parsing, no registry lookups, no caching) and reports three booleans:

| Field | True when… |
|---|---|
| `has_profile` | `profile.json` exists |
| `has_benchmark` | `benchmark.json` exists |
| `has_visualization` | `syscall_report.png` exists |

`syscall_report.png` is the single canonical visualization artifact — the
combined report produced by a completed visualization run — so it is the marker
for `has_visualization`.

> **Caveat.** A run generated with the visualizer's `--no-combined` option
> produces the individual charts but **not** `syscall_report.png`, and will
> therefore report `has_visualization = false` even though chart PNGs exist.
> This is intentional for Phase 002: detection keys on the one canonical
> combined report, not on the presence of arbitrary PNG files.

### API

The helper lives in `src/run_artifacts.c` / `include/run_artifacts.h`:

```c
typedef struct {
    int has_profile;
    int has_benchmark;
    int has_visualization;
} RunArtifacts;

/* Detect all three in a single filesystem pass. */
RunArtifacts run_detect_artifacts(const char *run_dir);

/* Convenience single-artifact queries (delegate to the above). */
int run_has_profile(const char *run_dir);
int run_has_benchmark(const char *run_dir);
int run_has_visualization(const char *run_dir);
```

`run_detect_artifacts()` is preferred when more than one state is needed, since
it resolves a run in one pass instead of three. A `NULL`, empty, or non-existent
`run_dir` simply reports all-false (a missing run has no artifacts); a trailing
slash on the path is tolerated.

### Example

```c
#include "run_artifacts.h"

RunArtifacts a =
    run_detect_artifacts("results/ls/2026-06-06_11-24-36");

printf("%d %d %d\n",
       a.has_profile,
       a.has_benchmark,
       a.has_visualization);
/* e.g. "1 0 1" — profiled and visualized, but not yet benchmarked */
```

### How this prepares the system for a future WebUI/API

A future WebUI or API lists runs cheaply from `runs_index.json` (no directory
scan), and then — only for a run it is actually displaying — calls
`run_detect_artifacts()` to learn which artifacts are available right now. That
drives the interface: enable or grey out a "View charts" button, decide whether
to still offer a "Run benchmark" action, show per-run status badges, and so on.
Because the answer is always recomputed from disk, it stays correct even if
artifacts are added or removed between requests — no cache to invalidate, no
registry to keep in sync.

---

## 14d. Phase 003 – Run Registry Access Layer

> Like Phases 001 and 002, this is **server-edition** infrastructure for a
> future WebUI/API. It is purely additive and internal: no CLI flag, no server,
> no HTTP, no database. It changes nothing about tracing, profiling,
> benchmarking, the JSON/CSV export formats, Run ID generation, the registry
> *writer*, or artifact detection — it only adds a module that **reads**
> `runs_index.json`.

### Why this layer exists

Phase 001 created `runs_index.json` as a catalog of completed runs, and that
file is the right place to look up runs without scanning the whole results tree
(see [Phase 001](#14b-phase-001--run-id-system)). But until now nothing in the
codebase actually *loads* it — any future component (a CLI tool, an API, a
WebUI) would have to open the file, re-implement the same JSON parsing, and
hard-code the same filesystem layout. That is duplicated, fragile, and easy to
get subtly wrong in each consumer.

Phase 003 adds a single module — `src/run_registry.c` / `include/run_registry.h`
— that becomes the **one owner of registry access**. Future code asks it for
runs through plain C functions and never touches JSON or paths directly. If the
on-disk format ever changes, only this module changes; every consumer keeps
working unchanged.

### What it provides

```c
typedef struct {
    char run_id[32];
    char program[256];
    char timestamp[64];
    char path[512];
    long total_syscalls;
    long unique_syscalls;
} RunInfo;

typedef struct {
    RunInfo *runs;
    size_t   count;
} RunRegistry;

RunRegistry  registry_load(const char *results_dir);
RunInfo     *registry_find_by_id(RunRegistry *registry, const char *run_id);
RunInfo     *registry_find_by_program(RunRegistry *registry, const char *program);
void         registry_free(RunRegistry *registry);
RunArtifacts registry_get_artifacts(const RunInfo *run);   /* Phase 002 bridge */
```

`RunInfo` mirrors exactly the fields stored per entry in `runs_index.json` — it
is **metadata only** and deliberately does not load `profile.json` or duplicate
any syscall data. `registry_load("results")` reads `results/runs_index.json`
and loads only what the registry already stores; it does **not** scan run
directories (avoiding that scan is the entire reason the registry exists).

Lookups are exact-match and return a **borrowed** pointer into the registry's
array (valid until `registry_free`, never freed by the caller). A missing,
empty, or entry-less registry loads quietly as `{ runs = NULL, count = 0 }`,
which is the normal state of a fresh install with no runs yet.

### Memory ownership

`registry_load()` returns a `RunRegistry` that owns its `runs` array; the caller
must release it with `registry_free()`, which is the only function that frees
registry memory. The pointers returned by `registry_find_by_id()` and
`registry_find_by_program()` point *into* that array — they are borrowed, must
not be freed, and must not be used after `registry_free()`. Because `RunInfo`
holds only fixed-size buffers (no nested pointers), `registry_free()` releases a
single block and there is nothing else to clean up.

### How it integrates with Phase 002

The registry stays metadata-only; mutable artifact state is still never stored
in it. `registry_get_artifacts(run)` is the bridge between the two phases: given
a run's Phase 001 metadata, it calls `run_detect_artifacts(run->path)`
(Phase 002) and returns the live `RunArtifacts`. So a caller gets stable
catalog metadata from the registry and up-to-the-moment artifact state from the
filesystem, with no duplicated or stale flags.

### How this prepares the project for CLI tools, APIs, and a WebUI

Any of those future front-ends now shares one access path: load the registry
once, list or look up runs as `RunInfo`, and ask `registry_get_artifacts()`
what each run currently has on disk. A CLI `list` command, an API
`GET /runs/<id>`, or a WebUI run table all become thin layers over this module
instead of re-parsing JSON. None of them exists yet — Phase 003 only builds the
missing layer between `runs_index.json` and that future development.

### Example

```c
#include "run_registry.h"

RunRegistry registry =
    registry_load("results");

RunInfo *run =
    registry_find_by_id(
        &registry,
        "run_a8f3d21c"
    );

RunArtifacts artifacts =
    registry_get_artifacts(run);

/* ... use run metadata + live artifact state ... */

registry_free(&registry);   /* releases the runs array */
```

---

## 15. Project Structure

```
syscall_profiler/
│
├── src/                    C source files
│   ├── main.c              CLI parsing and orchestration
│   ├── tracer.c            ptrace event loop, process/thread following
│   ├── profiler.c          per-syscall statistics
│   ├── syscall_table.c     syscall number -> name + category table
│   ├── decoder.c           argument and return-value decoding
│   ├── output.c            trace lines, report, CSV and JSON export
│   ├── filter.c            --only / --exclude / --top logic
│   ├── benchmark.c         --benchmark overhead measurement
│   ├── run_artifacts.c     dynamic artifact detection (Phase 002)
│   ├── run_registry.c      run registry access layer (Phase 003)
│   └── args.c              legacy decoder (not compiled; see note below)
│
├── include/                matching header files
│   ├── tracer.h
│   ├── profiler.h
│   ├── syscall_table.h
│   ├── decoder.h
│   ├── output.h
│   ├── filter.h
│   ├── benchmark.h
│   ├── run_artifacts.h     dynamic artifact detection (Phase 002)
│   ├── run_registry.h      run registry access layer (Phase 003)
│   └── args.h              legacy (not used by the build)
│
├── visualize.py            Python chart + summary generator
├── requirements.txt        Python dependencies for visualize.py
├── Makefile                build rules and the test target
├── README.md               this handbook
└── report/
    └── report.md           the technical development report
```

---

## 16. Source File Overview

This section explains every source and header file, its responsibility, and
how it interacts with the rest of the system.

### Compiled C modules

**`src/main.c`** — Program entry point. Parses the command
line by hand (so short and long options can appear in any order without the
permutation surprises `getopt` can cause), validates the target executable
before printing anything, builds the traced-command string used by the JSON
export, dispatches to either benchmark mode or normal tracing, and at the end
prints the report and triggers the CSV/JSON exports. Depends on `tracer`,
`profiler`, `output`, `filter`, and `benchmark`.

**`src/tracer.c` / `include/tracer.h`** — The `ptrace` engine. Forks the
target, enables tracing, and runs the main loop that receives a stop at each
syscall entry and exit. On entry it records the timestamp and triggers the
trace line; on exit it records the duration and the return value. It follows
children and threads created by `fork`, `vfork`, and `clone` (the latter
covers pthreads) by setting the corresponding `ptrace` options and tracking
each traced process in a small table. Drives `profiler` and `output`.

**`src/profiler.c` / `include/profiler.h`** — Statistics only, no I/O. Keeps
one record per distinct syscall number with its call count and total time, and
computes derived values (average time, total calls). The tracer pushes entry
and exit timestamps in; the output module reads the finished table out.

**`src/syscall_table.c` / `include/syscall_table.h`** — A static table mapping
each syscall number to its name and to one of eight categories (`FILE`, `MEM`,
`NET`, `PROC`, `SIG`, `IPC`, `TIME`, `SYS`), plus the short category labels and
the ANSI colors used in the terminal. A pure lookup module with no state; used
by the decoder, output, and filter.

**`src/decoder.c` / `include/decoder.h`** — Converts raw register and memory
values into readable text. It reads syscall arguments from the traced
process, formats file paths and flag sets (for example `O_RDONLY|O_CLOEXEC`),
and decodes return values, including mapping negative returns to errno names.
Invoked through the output module during tracing.

**`src/output.c` / `include/output.h`** — All formatting and file export. It
produces the live trace lines (entry and exit), the profile report (counts,
times, percentages, category summary, slowest list), and the CSV and JSON
exporters. It calls into `filter` for every visible row so a filtered run is
consistent, into `decoder` for argument text, and into `syscall_table` for
names, categories, and colors.

**`src/filter.c` / `include/filter.h`** — Implements the filtering options. It
parses and validates the `--only` / `--exclude` name lists against the syscall
table, stores the `--top` limit, and exposes `filter_should_show()` (the single
predicate the output module asks) plus a human-readable description for the
banner.

**`src/benchmark.c` / `include/benchmark.h`** — Implements `--benchmark`. It
times the target program run untraced versus run under a minimal `ptrace`
loop, takes the minimum across several iterations, and reports the overhead.
It is independent of the normal tracing path.

### Non-compiled files

**`src/args.c` / `include/args.h`** — A legacy/earlier version of the argument
decoder. It is **not** listed in the Makefile and is **not** part of the
build; the active decoder is `decoder.c`. These files are kept only for
historical reference and can be ignored when reading the current system. (They
are a good candidate for removal in a future cleanup.)

### Tooling and docs

**`visualize.py`** — Standalone Python script (matplotlib + pandas). Reads a
JSON export and writes the charts, the combined report image, and the
deterministic `summary.txt`. Not part of the C build.

**`requirements.txt`** — The Python dependencies for `visualize.py`
(`matplotlib`, `pandas`). Used for the virtual-environment install in
[Section 7a](#7a-server-setup--quick-start-virtual-environment). The C profiler
does not use it.

**`Makefile`** — Lists the eight compiled sources, builds `./profiler`, and
provides `clean` and `test` targets.

**`report/report.md`** — The technical development report (design, challenges,
testing, results). Distinct from this handbook.

---

## 17. Design Decisions

**Separate event tracing from statistics.** The tracer feeds two independent
consumers: the output module (which prints each event) and the profiler (which
aggregates). Keeping them separate means the live trace and the statistics
never interfere, and either can change without touching the other.

**A single filter predicate.** Rather than scattering filter checks through the
code, the filter module exposes one function, `filter_should_show()`, that the
output module calls at every decision point. This is why the filtered report is
guaranteed to be self-consistent: there is exactly one definition of "should
this syscall appear."

**Manual CLI parsing.** Argument parsing is done by hand rather than with
`getopt`, so that long options (`--only=...`) and short options (`-q`) can be
mixed in any order without `getopt`'s argument-permutation behavior causing
the target program's own flags to be misinterpreted.

**Validate the executable before any output.** The target program is checked
before the banner or trace UI is printed. A missing program produces a single
clean error and a non-zero exit code, instead of printing a full tracing
interface and then failing deep inside the child.

**Drop never-returning syscalls from statistics.** `exit_group` (and similar
calls that never return to user space) are seen at entry but never at exit, so
they have a duration of zero and a count of zero. Including them with zeroed
fields was inconsistent across the report, CSV, and JSON, so they are omitted
everywhere. The header totals are recomputed over completed calls so they
match the rows that are actually shown.

**Report the minimum in benchmark mode.** Benchmark overhead is reported as the
minimum of several runs, which best reflects the underlying cost and is least
distorted by scheduler noise.

**Horizontal bars instead of a pie chart.** The distribution chart uses a
sorted horizontal percentage bar chart. A pie chart was tried first but became
unreadable once many syscalls had similar colors and the legend mapping was
weak; a labeled, sorted bar chart is instantly readable.

**Deterministic summaries instead of AI.** The behavioral summary is generated
by fixed rules over the data, so it is reproducible, explainable, and free of
external dependencies. See
[Behavioral Summary Generation](#14-behavioral-summary-generation).

---

## 18. Known Limitations

- **Linux x86-64 only.** The tool depends on `ptrace` and the x86-64 register
  layout. It does not build or run on other architectures or operating
  systems.
- **Timing includes `ptrace` overhead.** Reported durations bracket the kernel
  work but also include the cost of stopping and resuming the traced process.
  They are upper bounds useful for relative comparison, not exact kernel
  execution times. Use `--benchmark` to quantify the overhead.
- **The observer effect.** Tracing changes the timing and, to some degree, the
  behavior of the traced program. Measurements describe the traced run, not a
  perfectly native run.
- **Partial argument decoding.** Only the most common syscalls have full
  argument decoding; others show the call without decoded arguments.
- **No attach to running processes.** The tool only traces programs it launches
  itself; attaching to an existing PID is not supported.
- **Bounded process table.** The number of simultaneously traced processes and
  threads is bounded by a fixed-size table.
- **Legacy files present.** `args.c` / `args.h` remain in the tree but are not
  part of the build.

---

## 19. Future Work

The following extensions are realistic next steps; none of them is currently
implemented.

- **Improved child-process and thread tracking.** Richer per-process and
  per-thread breakdowns in the report, beyond the current aggregation, and a
  larger or dynamic process table.
- **Network-focused analysis.** Deeper decoding of socket addresses and
  network syscalls, with a dedicated network view in the report and charts.
- **Live monitoring mode.** A continuously updating view of syscall activity
  while a long-running program runs, instead of a single end-of-run report.
- **Interactive dashboard.** A small web or TUI dashboard for exploring a
  profile (sorting, filtering, drilling into categories) instead of static
  PNGs.
- **eBPF backend.** An alternative collection backend using eBPF, which
  aggregates in the kernel and avoids the per-syscall context-switch overhead
  of `ptrace`, enabling low-overhead production monitoring.
- **Advanced performance analytics.** Latency distributions (histograms,
  percentiles) per syscall, time-series of activity, and detection of unusual
  patterns.
- **Cleanup.** Remove the legacy `args.c` / `args.h` files now that `decoder.c`
  is the active implementation.

---

## 20. Troubleshooting

A few issues come up often enough to collect here. Most have a one-line fix.

**`./profiler: command not found` or `make: command not found`.** Run the
profiler as `./profiler` (with the leading `./`) from the project directory, not
`profiler`. If `make` itself is missing, install the build tools — on
Debian/Ubuntu, `sudo apt install build-essential`. Remember the tool is
Linux x86-64 only; it will not build or run on macOS, Windows, or ARM.

**`ModuleNotFoundError: No module named 'pandas'` (or `matplotlib`).** These are
needed only by the visualizer, not by the C profiler. Install them with the
virtual-environment workflow in
[Section 7a](#7a-server-setup--quick-start-virtual-environment)
(`pip install -r requirements.txt` inside an activated `.venv`). The profiler
itself runs fine without Python.

**`Error: profile.json not found in run directory: <path>`.** The visualizer was
pointed at a directory that has no `profile.json` in it. Make sure you pass an
actual run directory — the path printed as `Results:` when you ran the profiler,
e.g. `python3 visualize.py results/ls/<timestamp>/`. The same applies to
`./profiler --benchmark-run <dir>`, which also needs a `profile.json` (with a
`program` field) inside the directory.

**Permission or `ptrace` issues** (`ptrace: Operation not permitted`). The
profiler needs permission to trace the target process. This commonly appears
inside containers or hardened systems. Options: run in an environment where
tracing is allowed, grant the container the `SYS_PTRACE` capability (for Docker,
`--cap-add=SYS_PTRACE`), or check the host's `kernel.yama.ptrace_scope` setting.
Avoid running as root just to work around this unless you understand the
implications.

**Benchmark numbers vary between runs.** Some run-to-run variation is normal:
benchmark timing is affected by system load, caching, and scheduling. The tool
already reports the minimum of several runs to reduce this noise. For more
stable figures, close other heavy programs and run the benchmark a few times;
compare the minimums rather than single results.

**Visualization output not appearing.** Charts and `summary.txt` are written to
the output directory, not the current directory. With a run directory
(`python3 visualize.py results/ls/<timestamp>/`) they go into that directory;
with a plain `profile.json` and no `--out`, they go next to the JSON file. Check
the `Output` path the visualizer prints. If you passed `--out`, look there. Also
confirm the previous step actually produced a `profile.json`.

**`Error: unknown syscall filter: <name>`.** A name passed to `--only` or
`--exclude` is not a recognized syscall. Check the spelling and use the exact
syscall name (for example `openat`, not `open`, on modern x86-64). Separate
multiple names with commas and no spaces, e.g.
`--exclude=mmap,mprotect,brk`.
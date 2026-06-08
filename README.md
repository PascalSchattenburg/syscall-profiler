# Syscall Profiler — Observability Console

A Linux x86-64 system call profiler and tracer written in C with `ptrace()`, extended
into a small observability platform: a run registry, a benchmarking system, a
visualization pipeline, and a local web dashboard.

The C profiler traces a target program, records how often each system call is used and
how long it takes, and writes structured results to disk. Every run is indexed so that
later tools — the benchmarker, the Python visualizer, and the Flask Web UI — can find,
analyze, compare, and present it.

---

## 1. Project Overview

At its core, the profiler runs a target program under `ptrace()`, intercepts every
system call the program makes, and builds a per-syscall summary: call counts, total
time, average time, and a behavioral category (file, memory, network, process, signal,
IPC, time, other). Each profiling run is stored in its own directory and registered in a
central index.

Around that core we built four cooperating components:

- A **run registry** that keeps a lightweight index of every run so other tools can
  discover runs without rescanning the filesystem.
- A **benchmarking system** that measures the overhead `ptrace` adds, by timing the same
  program traced and untraced.
- A **visualization pipeline** (Python) that turns the raw JSON into charts and a
  plain-language summary of what the program actually did.
- A **Web UI** (Flask) that reads everything back and presents runs, profiles,
  benchmarks, summaries, and charts in the browser.

The C profiler has no Python dependency and can be built and used entirely on its own.
The visualizer and Web UI are optional layers on top of the data it produces.

---

## 2. Features

- **Full syscall tracing** of a target process under `ptrace()`, following children and
  threads created via `fork()`, `vfork()`, and `clone()`.
- **Per-syscall statistics**: call count, total time, average time, syscall number, name,
  and behavioral category.
- **Eight behavioral categories**: file, memory, network, process, signal, IPC, time, and
  other.
- **Organized results**: every run is written to `results/<program>/<timestamp>/` and
  indexed in `results/runs_index.json`.
- **Multiple export formats**: human-readable terminal output, CSV, and JSON.
- **Filtering and shaping** of both the live trace and the final report
  (`--only`, `--exclude`, `--top`).
- **Benchmarking mode** that reports tracing overhead, either inline (`--benchmark`) or
  attached to an existing run (`--benchmark-run`).
- **Visualization pipeline** producing five charts and a deterministic, rule-based
  behavior summary.
- **Run registry library** (C) exposing the index as plain structs for other tools.
- **Local Web UI** (Flask, read-only) for browsing runs, profiles, benchmarks, summaries,
  and charts.

---

## 3. Architecture

The system is a pipeline. Each stage consumes what the previous stage produced and adds a
new layer of interpretation:

```
    Profiler (C)
        |
        v
    Run Registry
        |
        v
    Benchmarking
        |
        v
    Visualization
        |
        v
    Web UI
```

**Profiler (C).** The foundation. It forks the target program, attaches `ptrace`,
intercepts each system call, times it with `CLOCK_MONOTONIC`, and accumulates per-syscall
statistics. On completion it writes `profile.json` and `results.csv` into the run
directory and appends a metadata record to the registry. Core files: `src/tracer.c`
(the ptrace loop and process/thread table), `src/profiler.c` (statistics),
`src/syscall_table.c` (number → name + category), `src/output.c` (terminal/CSV/JSON
output), `src/decoder.c`, `src/filter.c`, and `src/main.c` (CLI, run directories, and
registry writing).

**Run Registry.** A discovery layer. The profiler appends one record per run to
`results/runs_index.json`. The C access layer (`src/run_registry.c`) reads that index
into `RunInfo` structs, and `src/run_artifacts.c` detects which artifacts currently exist
on disk for a given run. This lets any later tool list and locate runs without parsing
every result directory itself.

**Benchmarking.** A measurement layer (`src/benchmark.c`). It runs the target program in
two modes — untraced and traced — several times each, and reports how much overhead
tracing added. Results are written as `benchmark.json` inside the run directory.

**Visualization.** An interpretation layer (`visualize.py`). It reads `profile.json`,
produces five charts, and generates a plain-language `summary.txt` describing the
program's behavior. This is where raw numbers become something a human can read at a
glance.

**Web UI.** A presentation layer (`webui/app.py`). A small read-only Flask app that loads
the registry and each run's artifacts and serves them through a browser dashboard. It
never runs the C profiler and never writes to the results directory; it only reads what
the other stages produced.

---

## 4. Project Structure

```
syscall_profiler_uni/
├── Makefile                  Build rules for the C profiler
├── README.md                 This file
├── requirements.txt          Python deps for the visualizer (matplotlib, pandas)
├── visualize.py              Visualization pipeline + summary generator
│
├── include/                  C headers
│   ├── tracer.h              ptrace loop interface
│   ├── profiler.h            statistics accumulation
│   ├── syscall_table.h       syscall number -> name + category
│   ├── output.h              terminal / CSV / JSON output
│   ├── decoder.h, filter.h, args.h, benchmark.h
│
├── src/                      C sources (one .c per header above)
│   ├── main.c                CLI, run directories, registry writing
│   ├── tracer.c              ptrace loop, fork/thread following
│   ├── profiler.c            per-syscall statistics
│   ├── syscall_table.c       syscall table + categories
│   ├── output.c              output formats
│   ├── benchmark.c           overhead measurement
│   ├── run_registry.c        reads runs_index.json into RunInfo
│   └── run_artifacts.c       detects profile.json / benchmark.json / report
│
├── tests/                    C test programs
│   ├── registry_test.c
│   └── artifact_test.c
│
├── report/
│   └── report.md             Project report (development diary)
│
├── webui/                    Local web dashboard (Flask, read-only)
│   ├── app.py                Flask app + JSON API
│   ├── requirements.txt      Web UI deps (Flask)
│   ├── generate_demo_data.sh seeds sample runs for a quick demo
│   ├── templates/            index.html (single-page dashboard)
│   └── static/               self-hosted fonts and assets
│
└── results/                  created at runtime (git-ignored)
    ├── runs_index.json       the run registry
    └── <program>/<timestamp>/
        ├── profile.json      per-syscall statistics
        ├── results.csv       same data as CSV
        ├── benchmark.json    overhead measurement (if benchmarked)
        ├── summary.txt        plain-language summary (if visualized)
        └── *.png             charts (if visualized)
```

`results/` is created the first time you profile something and is excluded from version
control by `.gitignore`.

---

## 5. Installation

**Requirements**

- A Linux x86-64 system (the profiler uses Linux `ptrace` and the x86-64 syscall table).
- `gcc` and `make` for the C profiler.
- Python 3 with `matplotlib` and `pandas` for the visualizer (optional).
- `Flask` for the Web UI (optional).

**Get the code**

```bash
git clone <your-repository-url>
cd syscall_profiler_uni
```

The C profiler needs nothing beyond `gcc` and `make`. Python is only required if you want
charts, summaries, or the Web UI — see [Section 12](#12-python-virtual-environment) for
the virtual environment.

---

## 6. Build

Build the profiler with a single command:

```bash
make
```

This compiles every source file in `src/` with `gcc -Wall -Wextra -g -std=gnu99 -I include`
and links the `profiler` binary into the project root.

Other Make targets:

| Command            | What it does                                        |
|--------------------|-----------------------------------------------------|
| `make`             | Build the `profiler` binary                         |
| `make run`         | Build, then trace `ls` as a quick smoke test        |
| `make run-csv`     | Build, run, and export CSV                           |
| `make run-find`    | Trace `find` (a syscall-heavy program)              |
| `make test`        | Run a few quick self-tests (`ls`, `echo`, `true`)   |
| `make clean`       | Remove object files and the binary                  |
| `make help`        | Print available targets and usage examples          |

---

## 7. Usage

Basic form:

```bash
./profiler [OPTIONS] <program> [program-args...]
```

Profile a program:

```bash
./profiler ls
./profiler ls -la
./profiler -q cat /etc/hostname
```

**Options**

| Option                 | Effect                                                              |
|------------------------|---------------------------------------------------------------------|
| `-q`                   | Quiet mode — suppress the live, real-time trace output              |
| `-n`                   | No color — plain text (useful when piping to a file)                |
| `-c <file>`            | Export results to a CSV file                                        |
| `-h`                   | Show help                                                           |
| `--only=a,b,c`         | Show ONLY these syscalls (affects both the trace and the report)    |
| `--exclude=a,b`        | Hide these syscalls (affects both the trace and the report)         |
| `--top=N`              | Show only the top N entries in the final report                     |
| `--json=<file>`        | Export the full results as JSON to an explicit path                 |
| `--results-dir=<dir>`  | Base directory for run artifacts (default: `results/`)              |
| `--benchmark`          | Measure ptrace overhead inline (not added to the registry)          |
| `--benchmark-run <dir>`| Attach a benchmark to an existing run directory                     |

`--only` and `--exclude` cannot be combined.

**What a normal run produces.** Every normal profiling run automatically creates a run
directory and indexes it:

```
results/<program>/<timestamp>/
    profile.json     full per-syscall statistics
    results.csv      the same data in CSV form
```

The `<timestamp>` has the form `YYYY-MM-DD_HH-MM-SS` (filesystem-safe and chronologically
sortable). Each run also receives a unique run ID of the form `run_<8 hex digits>` and is
appended to `results/runs_index.json`.

**`profile.json` (excerpt)**

```json
{
  "run_id": "run_aa810cf4",
  "timestamp": "2026-06-08_19-36-47",
  "program": "ls",
  "total_syscalls": 74,
  "unique_syscalls": 20,
  "syscalls": [
    {
      "number":   9,
      "name":     "mmap",
      "category": "MEM ",
      "count":    17,
      "total_ms": 0.111448,
      "avg_ms":   0.006556
    }
  ]
}
```

**`results.csv` (header + first rows)**

```
syscall_number,syscall_name,category,call_count,total_time_ms,avg_time_ms
12,brk,MEM ,3,0.016253,0.005418
9,mmap,MEM ,17,0.111448,0.006556
257,openat,FILE,7,0.064287,0.009184
```

Categories are stored as short fixed-width labels (`FILE`, `MEM `, `NET `, `PROC`,
`SIG `, `IPC `, `TIME`, `OTHR`) corresponding to the eight behavioral categories.

---

## 8. Benchmarking

The benchmarker answers a practical question: *how much does tracing slow a program
down?* It times the target program in two modes:

- **untraced** — fork + exec + wait, with no `ptrace` attached;
- **traced** — the same program under a minimal `ptrace(PTRACE_SYSCALL)` loop, with no
  decoding or output, to measure pure tracing cost.

Each mode is run **three times** and the **minimum** of each is kept, since the minimum
best represents uncontended, best-case performance. Timing uses `CLOCK_MONOTONIC`
wall-clock measured by the parent around `fork()`/`waitpid()`.

**Inline benchmark** (prints a report, does not touch the registry):

```bash
./profiler --benchmark ls
./profiler --benchmark find /usr -maxdepth 2
```

**Attach a benchmark to an existing run** (recommended): this reads the program name from
the run's `profile.json`, reconstructs the command, runs the benchmark, and writes
`benchmark.json` into the same directory while inheriting that run's `run_id` and
`timestamp`:

```bash
./profiler --benchmark-run results/ls/2026-06-08_19-36-47
```

**`benchmark.json`**

```json
{
  "run_id": "run_aa810cf4",
  "timestamp": "2026-06-08_19-36-47",
  "program": "ls",
  "untraced_ms":  2.070469,
  "traced_ms":    1.755309,
  "overhead_pct": -15.22,
  "overhead_x":   0.85
}
```

`overhead_x` is `traced_ms / untraced_ms` and `overhead_pct` is the percentage change.
For very short-running programs these numbers can be noisy; see
[Limitations](#13-limitations) for a discussion, including the occasional case where a
traced run measures *faster* than an untraced one.

---

## 9. Visualization

The visualizer turns a profiling run into charts and a readable summary. It accepts either
a `profile.json` file or a run directory containing one:

```bash
python3 visualize.py results/ls/2026-06-08_19-36-47
python3 visualize.py results/ls/2026-06-08_19-36-47/profile.json
```

**Options**

| Option           | Effect                                                            |
|------------------|-------------------------------------------------------------------|
| `--top N`        | How many syscalls to show in the bar charts (default: 15)         |
| `--out DIR`      | Output directory for the PNGs (default: next to the JSON/run dir) |
| `--no-combined`  | Skip the combined overview image                                  |
| `--program CMD`  | Override the program name shown on the charts                     |

When given a run directory, the charts and `summary.txt` are written back into that same
directory, alongside the data.

**Outputs**

- `syscall_counts.png` — top N syscalls by call count
- `syscall_distribution.png` — share of total calls per syscall
- `category_breakdown.png` — calls grouped by behavioral category
- `slowest_syscalls.png` — top N by average time
- `syscall_report.png` — all four charts on one page
- `summary.txt` — a deterministic, plain-language behavior summary

**`summary.txt` (real output for `ls`)**

```
Program traced: ls

Most activity was file-system related (57%).
The program opened files 7 times, read data 7 times, wrote data 1 time, and scanned directories 2 times.
Memory mapping activity (17 mmap calls) was mostly caused by loading shared libraries at startup.
The remaining 5 syscall types were grouped into 'other' and accounted for 6.8% of all observed system calls. These are lower-frequency operations grouped together to keep the visualization readable.

In total: 74 syscalls across 20 distinct types.
```

This summary is generated by rule-based logic, not by any AI model or network service: the
same `profile.json` always produces the exact same text. The detailed mechanism is
described in the project report.

The visualizer requires `matplotlib` and `pandas`, so it must be run inside the Python
virtual environment ([Section 12](#12-python-virtual-environment)).

---

## 10. Run Registry

The registry is how the system keeps track of every run without rescanning the filesystem.

**On disk.** Each normal profiling run appends one metadata record to
`results/runs_index.json`. The file is an array of records:

```json
[
  {
    "run_id": "run_aa810cf4",
    "program": "ls",
    "timestamp": "2026-06-08_19-36-47",
    "path": "results/ls/2026-06-08_19-36-47",
    "total_syscalls": 74,
    "unique_syscalls": 20
  }
]
```

The profiler appends to this file atomically (it writes a temporary file and renames it),
so a crash mid-write cannot corrupt the index. Inline `--benchmark` and `--benchmark-run`
do **not** add entries — they are not complete profiling runs.

**In C.** `src/run_registry.c` (with `include/run_registry.h`) reads the index into plain
structs so other tools can use it:

- `registry_load("results")` returns a `RunRegistry` that owns an array of `RunInfo`.
- `registry_find_by_id(...)` and `registry_find_by_program(...)` look up records.
- `registry_free(...)` releases the registry.

A `RunInfo` holds `run_id`, `program`, `timestamp`, `path`, `total_syscalls`, and
`unique_syscalls` in fixed-size buffers (no nested allocations).

**Artifact detection.** `src/run_artifacts.c` (with `include/run_artifacts.h`) answers a
different question — *which artifacts exist on disk right now?* — via
`run_detect_artifacts(run_dir)`, which reports whether `profile.json`, `benchmark.json`,
and `syscall_report.png` are present. Artifact state is never cached; it is detected fresh
each time, because a run can gain a benchmark or charts long after it was first profiled.

The test programs `tests/registry_test.c` and `tests/artifact_test.c` exercise these two
modules.

---

## 11. Web UI

The Web UI is a small, **read-only** Flask dashboard for browsing runs in the browser. It
reads `results/runs_index.json` and each run's artifacts directly; it never invokes the C
profiler and never writes to `results/`.

**Start it**

```bash
cd webui
source ../.venv/bin/activate      # the Python venv (see Section 12)
python app.py
# then open http://127.0.0.1:5000
```

To point the Web UI at a results directory elsewhere, set `SYSCALL_RESULTS_DIR`:

```bash
SYSCALL_RESULTS_DIR=/mnt/data/profiles python app.py
```

For a quick demonstration without profiling anything yourself, seed sample runs first:

```bash
cd webui
./generate_demo_data.sh
python app.py
```

**JSON API**

| Endpoint                              | Returns                                            |
|---------------------------------------|----------------------------------------------------|
| `GET /`                               | The single-page dashboard                          |
| `GET /api/runs`                       | All runs from the registry, enriched with status   |
| `GET /api/run/<id>`                   | One run's metadata                                  |
| `GET /api/run/<id>/artifacts`         | Which artifacts exist for that run                 |
| `GET /api/run/<id>/summary`           | The plain-language `summary.txt`                   |
| `GET /api/run/<id>/benchmark`         | The `benchmark.json` (if present)                  |
| `GET /api/run/<id>/profile`           | The full `profile.json`                            |
| `GET /api/run/<id>/image/<name>`      | A chart PNG (whitelisted filenames only)           |
| `GET /api/programs`                   | Programs seen across all runs                      |

**Run status.** Each run is labeled by artifact availability only — there is no
performance judgment:

- **OK** — both `profile.json` and `syscall_report.png` are present (fully processed).
- **INCOMPLETE** — something required is missing.

`benchmark.json` is optional and never affects status. The image route only serves the
five known chart filenames, which prevents path-traversal through the `<name>` parameter.

Fonts are self-hosted under `webui/static/`, so the dashboard works without any external
network access.

---

## 12. Python Virtual Environment

The **visualizer** (`visualize.py`) and the **Web UI** (`webui/app.py`) require Python
packages. The recommended way to install them is a virtual environment so they do not
interfere with system Python.

**Create the environment (once):**

```bash
python3 -m venv .venv
```

**Activate it (every new shell):**

```bash
source .venv/bin/activate
```

**Install the dependencies:**

```bash
pip install -r requirements.txt          # visualizer: matplotlib, pandas
pip install -r webui/requirements.txt    # Web UI: Flask
```

**Why this matters.** If you run the visualizer or Web UI *without* an active environment
that has the packages installed, Python will fail with an error like:

```
ModuleNotFoundError: No module named 'pandas'
```

This almost always means the virtual environment is not active (or the dependencies were
never installed into it). The C profiler is unaffected — it has no Python dependency and
runs fine without any of this.

**Verify the environment is active.** When the venv is active your shell prompt is usually
prefixed with `(.venv)`. You can also check explicitly:

```bash
which python        # should point inside .../.venv/bin/python
python -c "import pandas, matplotlib; print('ok')"
```

If `which python` points at `/usr/bin/python` instead of your `.venv`, the environment is
not active — run `source .venv/bin/activate` again.

To leave the environment:

```bash
deactivate
```

---

## 13. Example Workflow

A complete end-to-end session, from building the tool to viewing the results in the
browser:

```bash
# 1. Build the profiler
make

# 2. Profile a program (creates results/ls/<timestamp>/ and indexes it)
./profiler ls

# 3. Set up Python (first time only)
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
pip install -r webui/requirements.txt

# 4. Attach a benchmark to the run you just created
#    (replace <timestamp> with the directory that step 2 printed)
./profiler --benchmark-run results/ls/<timestamp>

# 5. Generate charts + summary into that same run directory
python3 visualize.py results/ls/<timestamp>

# 6. Browse everything in the Web UI
cd webui
python app.py
# open http://127.0.0.1:5000
```

After step 5 the run directory is "OK" in the Web UI, because it now contains both
`profile.json` and `syscall_report.png`.

---

## 14. Limitations

- **Linux-only.** The profiler depends on Linux `ptrace` and an x86-64 syscall table. It
  does not run on macOS or Windows.
- **Tracing overhead.** `ptrace` stops the tracee twice per system call, which adds real,
  measurable overhead. The profiler is a learning and analysis tool, not a zero-cost
  production tracer.
- **Scheduler-sensitive benchmarks.** Benchmark numbers are affected by OS scheduling, CPU
  frequency scaling, and cache state. Running the same benchmark twice can give different
  numbers.
- **Short programs are noisy.** For extremely short-running programs (a few milliseconds),
  measurement noise can dominate. In a small number of such runs the *traced* execution
  even measured faster than the untraced one (a negative `overhead_pct`). This is a
  measurement artifact, not a real speedup; it is discussed in detail in the project
  report and we do not claim to have fully explained every such case.
- **Per-syscall timing includes tracing cost.** The reported `total_ms`/`avg_ms` per
  syscall are measured under `ptrace` and therefore include tracing overhead. They are
  best used for *relative* comparison between syscalls, not as absolute kernel timings.

These are presented as honest engineering trade-offs of a `ptrace`-based approach.

---

## 15. Future Work

The project is structured so the data layer and the presentation layer can grow
independently. Planned and possible directions, focused on the observability platform:

- **eBPF backend** as an alternative to `ptrace` for much lower overhead and better
  scaling on busy systems.
- **Live monitoring dashboard** with real-time syscall streaming into the Web UI.
- **Alerting** for suspicious behavior (for example, unexpected network or process
  activity).
- **Historical trend analysis** and **long-term run comparison** across many runs.
- **Remote agent architecture** and **multi-host / distributed monitoring** so several
  servers report into one console.
- **AI-assisted observability** with natural-language queries such as
  *"What happened on my server today?"*, *"Which program generated the most system
  calls?"*, or *"Show me unusual behavior in the last 24 hours."*
- **Notifications** (for example, Telegram integration) for important events.

---

## 16. Test Environment

The project was developed and tested on a dedicated Ubuntu server. A dedicated machine was
chosen so the tool could run continuously, accumulate many runs over time, and be reached
remotely through the Web UI — a setup much closer to a real monitoring environment than a
laptop used occasionally.

**Hardware**

| Component | Specification                                        |
|-----------|------------------------------------------------------|
| CPU       | Intel Core i7-11700F, 8 cores / 16 threads           |
| RAM       | 32 GB DDR4                                            |
| GPU       | NVIDIA RTX 3060 Ti, 8 GB VRAM                         |
| Storage   | 1 TB NVMe SSD + 1 TB HDD + 4 TB external SSD          |

**Software**

| Component  | Version                          |
|------------|----------------------------------|
| OS         | Ubuntu Server 24.04.4 LTS        |
| Kernel     | 6.8.0-111-generic                |
| Compiler   | GCC 13.3.0                       |
| Build      | GNU Make                         |
| Tracing    | Linux `ptrace()`                 |

---

*Built as an Operating Systems course project. See `report/report.md` for the full
development story, design decisions, and lessons learned.*

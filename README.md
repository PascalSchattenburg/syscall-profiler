# System Call Profiler & Tracer

A Linux system call profiler, tracer, benchmark tool, visualization pipeline and local Web UI for analyzing how programs interact with the operating system through system calls.

The project was built for the Operating Systems course at the University of Basel. It started as a command-line `ptrace()` tracer and evolved into a small observability platform for a dedicated Ubuntu server. The system can trace a program, count and categorize its system calls, measure tracing overhead, store runs persistently, generate charts and summaries, and display all collected runs in a browser-based dashboard.

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Features](#2-features)
3. [Architecture](#3-architecture)
4. [Project Structure](#4-project-structure)
5. [Installation](#5-installation)
6. [Build](#6-build)
7. [Usage](#7-usage)
8. [Benchmarking](#8-benchmarking)
9. [Visualization](#9-visualization)
10. [Run Registry](#10-run-registry)
11. [Web UI](#11-web-ui)
12. [Example Workflow](#12-example-workflow)
13. [Limitations](#13-limitations)
14. [Future Work](#14-future-work)
15. [Test Environment](#15-test-environment)
16. [Troubleshooting](#16-troubleshooting)

---

## 1. Project Overview

The System Call Profiler & Tracer observes how a Linux program communicates with the operating system kernel. When a program opens a file, allocates memory, creates a process, reads data, writes output, or performs many other privileged operations, it does so through a system call.

This project launches a target program under `ptrace()` control and stops it at every system call entry and exit. It records:

- the system call name
- decoded arguments where supported
- the return value and errno information
- the system call category
- the number of calls
- total and average syscall time
- the relation between traced and untraced runtime

The project can be understood as a simplified combination of:

- `strace` for live syscall tracing
- `strace -c` for aggregated statistics
- a benchmark tool for tracing overhead
- a visualization pipeline for charts and summaries
- a Web UI for browsing collected runs

The final version is not only a terminal tool anymore. It stores every run in a structured results directory, indexes runs in a registry, generates deterministic behavior summaries, and exposes the collected data through a local dashboard.

---

## 2. Features

### Core profiler

- Live system call tracing with `ptrace()`
- Syscall entry and exit handling
- Return value and errno decoding
- Category labels such as file system, memory, process, IPC and system
- Per-syscall count, total time and average time
- Profile report printed after execution
- CSV export
- JSON export
- Filtering with `--only`, `--exclude` and `--top`

### Benchmarking

- Compare normal execution against traced execution
- Attach benchmark results to an existing profiling run
- Store benchmark data as `benchmark.json`
- Show overhead as multiplier and percentage

### Results management

- Every run gets a timestamped directory
- Every run gets a stable `run_id`
- Run metadata is stored in `results/runs_index.json`
- Profile, CSV, benchmark, summary and visualization files stay together in one run directory

### Visualization

- Python visualization script using `pandas` and `matplotlib`
- Generates PNG charts
- Generates a combined report image
- Generates a deterministic `summary.txt`
- Accepts both `profile.json` files and full run directories

### Web UI

- Local Flask-based dashboard
- Reads the existing registry and artifacts
- Shows runs, programs, benchmark data, charts and summaries
- Does not modify profiler data
- Works as a read-only interface for demonstrations and server use

---

## 3. Architecture

The project is organized as a pipeline. Each part has a clear responsibility.

```text
Profiler (C)
    |
    v
Run Directory + Registry
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

### Profiler

The C profiler launches a target program and traces it with `ptrace()`. It collects raw syscall events and aggregates them into a profile.

### Registry

Each completed profiling run is written to a timestamped folder and indexed in `results/runs_index.json`. The registry stores metadata only, not full syscall data.

### Benchmarking

Benchmarking can be run separately or attached to an existing run with `--benchmark-run`. This keeps benchmark data in the same directory as the profile.

### Visualization

The Python visualizer reads `profile.json`, generates charts and creates a deterministic behavior summary. The summary is rule-based and not AI-generated.

### Web UI

The Flask Web UI reads `runs_index.json` and the run artifacts from disk. It does not call or modify the C profiler. It is a read-only consumer of the generated data.

---

## 4. Project Structure

```text
syscall-profiler/
│
├── src/
│   ├── main.c
│   ├── tracer.c
│   ├── profiler.c
│   ├── decoder.c
│   ├── args.c
│   ├── output.c
│   ├── filter.c
│   ├── benchmark.c
│   ├── syscall_table.c
│   ├── run_artifacts.c
│   └── run_registry.c
│
├── include/
│   ├── tracer.h
│   ├── args.h
│   ├── profiler.h
│   ├── decoder.h
│   ├── output.h
│   ├── filter.h
│   ├── benchmark.h
│   ├── syscall_table.h
│   ├── run_artifacts.h
│   └── run_registry.h
│
├── webui/
│   ├── app.py
│   ├── requirements.txt
│   ├── templates/
│   │   ├── index.html
│   └── static/
│   │   ├── fonts/
│
├── results/
│   ├── runs_index.json
│   └── <program>/<timestamp>/
│       ├── profile.json
│       ├── results.csv
│       ├── benchmark.json
│       ├── summary.txt
│       └── *.png
│
├── visualize.py
├── requirements.txt
├── Makefile
├── README.md
└── report/
    └── report.md
```

---

## 5. Installation

The C profiler itself only needs Linux, GCC and Make. The visualization pipeline and Web UI need Python packages installed inside a virtual environment.

### System packages

On Ubuntu:

```bash
sudo apt update
sudo apt install build-essential python3 python3-venv python3-pip
```

### Clone or unpack the project

```bash
git clone <repository-url>
cd syscall-profiler
```

## 6. Build

Build the C profiler:

```bash
make
```

This creates:

```text
./profiler
```

Clean the build:

```bash
make clean
```

Run the included smoke tests, if available:

```bash
make test
```

---

## 7. Usage

General command format:

```bash
./profiler [options] <program> [program arguments...]
```

### Profile a simple command

```bash
./profiler ls
```

Example with arguments:

```bash
./profiler ls -la /usr/bin
```

Quiet mode:

```bash
./profiler -q ls
```

No color output:

```bash
./profiler -n ls
```

Export to CSV:

```bash
./profiler -c results.csv ls
```

Export to JSON:

```bash
./profiler --json=profile.json ls
```

### Filtering

Show only selected syscalls:

```bash
./profiler --only=openat,read,write,close ls
```

Exclude selected syscalls:

```bash
./profiler --exclude=mmap,mprotect,brk ls
```

Show only the top N rows in the final report:

```bash
./profiler --top=5 ls
```

### Help

```bash
./profiler -h
```

The help output should document the main workflow commands, including profiling, benchmarking, visualization and Web UI startup.

---

## 8. Benchmarking

Benchmarking compares normal execution with traced execution.

### Benchmark directly

```bash
./profiler --benchmark ls
```

### Attach a benchmark to an existing run

Recommended workflow:

```bash
./profiler ls
./profiler --benchmark-run results/ls/<timestamp>
```

The second command writes:

```text
results/ls/<timestamp>/benchmark.json
```

This keeps the benchmark together with the profile, CSV, visualization and summary of the same run.

### Interpreting overhead

The benchmark reports values such as:

```text
Normal execution: 2.804 ms
Traced execution: 1.010 ms
Overhead: 0.36x
```

Usually traced execution should be slower because `ptrace()` stops the traced program at every syscall entry and exit. In very short programs, measurement noise, scheduling effects, CPU frequency scaling and cache effects can sometimes produce inconsistent results. For this reason, benchmark numbers should be interpreted carefully, especially for tiny commands such as `pwd`, `true` or very small `ls` runs.

---

## 9. Visualization

The visualization system uses Python and therefore requires the project virtual environment.

### Create and activate the Python environment

From the project root:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Verify that the environment is active:

```bash
which python3
```

Expected result:

```text
/path/to/project/.venv/bin/python3
```

If the virtual environment is not active, the visualizer may fail with:

```text
ModuleNotFoundError: No module named 'pandas'
```

### Visualize a run

Recommended:

```bash
python3 visualize.py results/ls/<timestamp>
```

The script reads:

```text
results/ls/<timestamp>/profile.json
```

and writes:

```text
summary.txt
syscall_counts.png
syscall_distribution.png
category_breakdown.png
slowest_syscalls.png
syscall_report.png
```

into the same run directory.

### Visualize a profile file directly

```bash
python3 visualize.py results/ls/<timestamp>/profile.json
```

---

## 10. Run Registry

The run registry is stored at:

```text
results/runs_index.json
```

It contains one metadata entry per completed profiling run:

```json
{
  "run_id": "run_a8f3d21c",
  "program": "ls",
  "timestamp": "2026-06-07_10-06-16",
  "path": "results/ls/2026-06-07_10-06-16",
  "total_syscalls": 75,
  "unique_syscalls": 20
}
```
Each profiling run receives a stable run_id. The run identifier allows the Web UI to reference and locate runs independently of their physical directory path. This makes run lookups more robust and avoids relying solely on timestamps or filesystem locations.

The registry is intentionally metadata-only. It does not store mutable artifact flags such as `has_benchmark` or `has_visualization`, because those values can become stale. Instead, artifact existence is detected dynamically from the run directory.

Example:

- `profile.json` exists -> profile available
- `benchmark.json` exists -> benchmark available
- `syscall_report.png` exists -> visualization available

This design prevents stale registry entries and prepares the project for Web UI and API-style usage.

---

## 11. Web UI

The Web UI is a local Flask dashboard that reads the existing results directory.

### Start the Web UI

From the project root:

```bash
cd webui
source .venv/bin/activate
python app.py
```

Then open:

```text
http://127.0.0.1:5000
```

If the app is configured to listen on all interfaces, it can also be opened from another device using the server IP:

```text
http://<server-ip>:5000
```

### Web UI behavior

The Web UI:

- reads `results/runs_index.json`
- reads `profile.json`
- reads `benchmark.json`
- reads `summary.txt`
- serves generated PNG charts
- shows run status dynamically
- does not modify profiler data
- does not write to the results directory

### Web UI dependencies

The Web UI has its own requirements file:

```bash
cd webui
pip install -r requirements.txt
```

If the same root `.venv` is used, activate it before starting the app.

---

## 12. Example Workflow

This is the recommended full workflow for a complete run.

```bash
# 1. Build profiler
make

# 2. Create Python environment once
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt

# 3. Profile a program
./profiler ls

# 4. Attach benchmark to the generated run
./profiler --benchmark-run results/ls/<timestamp>

# 5. Generate visualizations and summary
python3 visualize.py results/ls/<timestamp>

# 6. Start Web UI
cd webui
source .venv/bin/activate
python app.py
```

A complete run directory then contains:

```text
profile.json
results.csv
benchmark.json
summary.txt
syscall_counts.png
syscall_distribution.png
category_breakdown.png
slowest_syscalls.png
syscall_report.png
```

---

## 13. Limitations

- Linux-only implementation.
- x86-64 specific register layout.
- `ptrace()` introduces measurable overhead.
- Timing values include tracing overhead.
- Benchmark measurements can be influenced by scheduling, cache effects and CPU frequency scaling.
- Very short-running programs can produce inconsistent benchmark values.
- Some benchmark results showed traced execution appearing faster than untraced execution; this is interpreted as measurement noise, not as a real speedup.
- The tool currently launches new programs; attaching to arbitrary already-running processes is not the main focus.
- The Web UI is local and read-only; it is not hardened as a production web service.

---

## 14. Future Work

Possible future extensions:

- eBPF backend for lower-overhead tracing
- live monitoring dashboard
- real-time syscall streaming
- alerting for suspicious behavior
- historical trend analysis
- long-term comparison of runs
- remote agent architecture
- distributed monitoring across multiple servers
- multi-host support
- improved Web UI search and filtering
- AI-assisted observability agent
- Telegram integration
- natural-language queries such as:
  - "What happened on my server today?"
  - "Which program generated the most system calls?"
  - "Show me unusual behavior in the last 24 hours."

The current project already has a useful foundation for these ideas because it stores runs persistently, assigns stable run IDs, keeps artifacts grouped per run, and exposes the data through a dashboard.

---

## 15. Test Environment

The project was developed and evaluated on a dedicated Linux server.

### Hardware

- CPU: Intel Core i7-11700F
- Cores / Threads: 8 cores / 16 threads
- RAM: 32 GB DDR4
- GPU: NVIDIA RTX 3060 Ti, 8 GB VRAM

### Software

- Operating System: Ubuntu Server 24.04.4 LTS
- Kernel: 6.8.0-111-generic
- Compiler: GCC 13.3.0
- Build system: GNU Make
- Main tracing interface: `ptrace()`
- Programming language: C
- Visualization: Python, pandas, matplotlib
- Web UI: Python, Flask

The dedicated server was chosen because it allowed us to collect many runs over time, keep profiling artifacts centrally stored, and test the project in a more realistic monitoring scenario than a short local laptop demo.

---

## 16. Troubleshooting

### `ModuleNotFoundError: No module named 'pandas'`

Activate the virtual environment and install requirements:

```bash
source .venv/bin/activate
pip install -r requirements.txt
```

Then rerun:

```bash
python3 visualize.py results/ls/<timestamp>
```

### Web UI starts but cannot be reached from another computer

By default Flask may bind only to localhost. For remote access, ensure `app.py` binds to:

```python
host="0.0.0.0"
```

Then open:

```text
http://<server-ip>:5000
```

Also check firewall settings.

### `ptrace: Operation not permitted`

The environment may restrict tracing. This can happen in containers or hardened systems. Run on a normal Linux host or allow the necessary tracing permissions.

### Benchmark results look strange

Very short programs can produce noisy measurements. Repeat the benchmark, close background load, and interpret results as approximate overhead measurements rather than perfect kernel timing.

### Visualizations are missing in the Web UI

Run the visualizer for the specific run:

```bash
source .venv/bin/activate
python3 visualize.py results/<program>/<timestamp>
```

The Web UI marks visualization as available when the expected PNG files, especially `syscall_report.png`, exist in the run directory.

# System Call Profiler & Tracer
## Final Project Report

**Course:** Operating Systems  
**Project:** System Call Profiler and Tracer  
**Platform:** Linux x86-64, Ubuntu Server  
**Main technologies:** C, `ptrace()`, Python, Flask, pandas, matplotlib  

---

## 1. Introduction

For our Operating Systems project, we built a system call profiler and tracer for Linux. The original idea was simple: we wanted to run a normal program, observe which system calls it makes, count them, measure them and understand how the program interacts with the operating system.

During the project, this idea grew much larger than we initially expected. We started with a command-line tracer, but over time we added persistent run storage, benchmarking, visualizations, automated summaries, a run registry and finally a Web UI dashboard. By the end, the project was no longer only a syscall tracer. It became a small observability platform for understanding program behavior on a Linux server.

The final system can:

- trace a program with `ptrace()`
- decode and categorize system calls
- count and time system calls
- export profile data to JSON and CSV
- benchmark tracing overhead
- store every run in a structured directory
- assign every run a stable run ID
- generate charts and a deterministic behavior summary
- display runs, benchmarks, summaries and charts in a browser-based Web UI

We built the project as three Computer Science students. A large part of the project was not only implementing features, but also learning how to turn low-level operating system data into something that is understandable and useful.

---

## 2. Background

System calls are the interface between user programs and the operating system kernel. A program cannot directly access files, devices, network sockets or process management functions. Instead, it asks the kernel to perform these operations through system calls.

Examples include:

- `openat()` to open files
- `read()` and `write()` for I/O
- `mmap()` for memory mapping
- `fork()`, `clone()` and `execve()` for process and thread behavior
- `close()` to release file descriptors

In the Operating Systems lecture, system calls are introduced as the lower-level interface to OS services. This project made that concept visible. Instead of only learning that applications use system calls, we could actually watch a program crossing the user-kernel boundary again and again.

A simple command such as:

```bash
ls
```

already produces many system calls. Some of them are caused by `ls` itself, but many happen before the actual program logic starts, for example while shared libraries are loaded through the dynamic linker. Seeing this directly was one of the first moments where the project became meaningful for us.

---

## 3. Project History: From Local Tracer to Server Edition

At the beginning, our plan was to build a simple local syscall profiler. The first goal was to run the tool on a normal Ubuntu machine and print a live trace and a final report.

Very early in development, we realized that this was useful, but also limited. A one-time terminal output is interesting for understanding one command, but it is not ideal for collecting many runs, comparing behavior over time or presenting the project in a clear way.

Because we had access to a dedicated Ubuntu server, we decided to move the project toward a server edition. This was not just because it was "cooler". We realized that a server deployment gave the project a more realistic monitoring scenario:

- the server can run permanently
- many profiling runs can be collected over time
- results can be stored centrally
- a browser dashboard can make the data easier to access
- the workflow becomes closer to real observability and monitoring tools

This decision significantly expanded the scope. We had to think about persistent storage, run organization, metadata, benchmark artifacts, visualization artifacts and how a Web UI should read the generated data. Looking back, this was the point where the project changed from a small tracer into a platform.

---

## 4. Why We Chose ptrace()

We chose `ptrace()` as the tracing mechanism because it is the standard Linux interface for observing and controlling another process. Tools such as `strace` and debuggers such as GDB are based on the same idea.

We considered other approaches conceptually, especially eBPF, auditd and kernel modules.

### Why not eBPF?

eBPF would probably be the better choice for a production-grade tracing backend. It can collect data with much lower overhead and can aggregate information inside the kernel. However, it would also have introduced much more complexity: eBPF programs, verifier restrictions, kernel-side data structures and a separate development workflow.

For an Operating Systems course project, we wanted to understand process tracing, syscall entry and exit, register usage and context switching directly. `ptrace()` forced us to learn exactly those concepts.

### Why not auditd?

`auditd` is useful for security auditing and system-wide logging, but it is less suitable for building our own educational profiler. It would not give us the same direct control over syscall entry and exit handling, argument decoding and timing.

### Why not kernel modules?

A kernel module would have been powerful, but also risky and too complex for the project scope. Writing kernel code would require more care, more debugging effort and higher risk of system instability. We wanted a user-space tool that could be built, tested and demonstrated safely.

### Why ptrace was appropriate

`ptrace()` was the best fit because:

- it is available on standard Linux systems
- it requires no custom kernel module
- it gives direct access to syscall entry and exit events
- it allows reading registers and tracee memory
- it is educational and closely connected to OS concepts
- it keeps the implementation complexity realistic
- it is easier to debug than kernel-space code

The main disadvantage is overhead. Every syscall causes the traced program to stop twice: once at entry and once at exit. This creates additional context switches. We accepted this limitation and added benchmark mode to measure it instead of hiding it.

---

## 5. System Architecture

The final system is built as a pipeline:

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

The profiler is written in C. It launches the target program, traces it with `ptrace()`, decodes syscalls and records statistics.

### Results directory

Every profiling run is written into:

```text
results/<program>/<timestamp>/
```

The directory contains the artifacts belonging to that run.

### Run registry

The registry file:

```text
results/runs_index.json
```

stores metadata for completed runs. This gives every run a stable entry that the Web UI can read without scanning all directories manually.

### Benchmarking

Benchmarking compares normal execution with traced execution. With `--benchmark-run`, benchmark data is written into the same run directory as the profile.

### Visualization

The visualization script reads `profile.json`, produces charts and writes a deterministic `summary.txt`.

### Web UI

The Flask Web UI reads the registry and the run artifacts. It does not modify data. It is a read-only dashboard for exploring collected runs.

---

## 6. Implementation

### Tracing

The tracer uses the typical `ptrace()` model:

1. The profiler forks.
2. The child process calls `PTRACE_TRACEME`.
3. The child executes the target program.
4. The parent waits for syscall stops.
5. The tracer alternates between syscall entry and syscall exit.
6. On entry, arguments are decoded.
7. On exit, return values and durations are recorded.

The hardest part was understanding that every syscall appears twice. At first, it is tempting to think that a syscall is just one event. In reality, with `ptrace()`, we receive an entry stop and an exit stop. We had to build a small state machine to know whether the current stop belongs to the beginning or the end of a syscall.

### Profiling

The profiler aggregates:

- syscall count
- total time
- average time
- percentage of total calls
- category totals

The final report shows which syscalls occurred most often and which were slowest on average.

### Syscall categories

We grouped syscalls into categories such as:

- File System
- Memory
- Process
- IPC
- System
- Network
- Signal
- Time

This helped us interpret program behavior at a higher level. A raw list of syscall names is correct, but a category summary is much easier to understand.

### Exports

The project exports both CSV and JSON.

CSV is useful for spreadsheets and quick inspection. JSON became the most important format because the visualization pipeline and Web UI use it as structured input.

### Run storage

One important improvement was the results hierarchy. Instead of overwriting files, every run gets its own directory:

```text
results/ls/2026-06-08_16-27-57/
```

This directory can contain:

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

This structure made the later Web UI much easier to build.

---

## 7. Why Visualization Became Necessary

At first, the profiler only produced terminal output and JSON files. Technically, that was enough. The data was correct and complete. But we quickly discovered that raw syscall data is difficult to interpret.

A large list of syscall counts and timings does not immediately tell a user what the program did. Even we, as the developers, often had to stare at the JSON and ask: what does this actually mean?

That is why we added the visualization pipeline. The charts made patterns much easier to see:

- Which syscall was most frequent?
- Which category dominated?
- How much of the run was file-system behavior?
- Which syscalls were slowest?
- How large is the long tail of less frequent syscalls?

Later, the visualization step also produced `summary.txt`, and the Web UI displayed the generated charts and summaries. This made the project much more usable. We learned that observability is not only about collecting data. It is also about presenting data in a form that humans can understand quickly.

---

## 8. Automated Behavior Summary

One important part of the project is the automated behavior summary.

The summaries are not generated by AI. They are generated deterministically by our visualization pipeline.

The process works conceptually like this:

1. `visualize.py` loads `profile.json`.
2. It reads all syscall entries and their counts.
3. It groups syscalls into categories such as File System, Memory, Process, IPC and System.
4. It calculates:
   - total syscall count
   - unique syscall count
   - category frequencies
   - dominant category
   - dominant syscall types
   - file operations such as open, read, write and directory scans
   - memory mapping activity such as `mmap`
   - process-related activity
5. Rule-based heuristics decide what behavior is most important.
6. Predefined text templates are filled with the measured numbers.
7. The generated text is written to `summary.txt`.

For example, if a program has many `openat`, `read`, `write` and `getdents64` calls, the summary describes it as file-system oriented. If it has many `mmap` calls, the summary explains memory mapping activity, often related to shared library loading.

A typical generated sentence is:

```text
The program opened files 3 times, read data 1 time, and wrote data 1 time.
```

This approach has several advantages:

- it is reproducible
- it is deterministic
- it requires no external AI service
- it can be explained from the code
- it works offline
- the same input always produces the same summary

We added this because we wanted users to get a quick interpretation without reading raw syscall tables themselves.

---

## 9. Benchmarking and Observations

Benchmarking was added because `ptrace()` overhead is a real limitation. A traced program should normally run slower than an untraced program because every syscall causes additional stops and context switches.

The benchmark system compares:

- normal execution
- traced execution

and reports an overhead multiplier.

In most cases, the results made sense: tracing was slower. However, we also discovered an interesting anomaly in a small number of short runs. Sometimes the traced execution appeared faster than the untraced execution.

Example:

```text
normal = 2.804 ms
traced = 1.010 ms
overhead = 0.36x
```

Clearly, tracing does not actually make a program faster. We interpret this as a benchmark artifact. Possible explanations are:

- measurement noise
- operating system scheduling effects
- CPU frequency scaling
- cache effects
- very short runtime of the target program
- background load on the system

We did not fully solve or eliminate these anomalies. Instead, we documented them honestly. This was an important engineering lesson: benchmarking very short programs is difficult, and measurement results need interpretation.

---

## 10. Web UI

The Web UI was the final major feature. It made the project much easier to present and use.

The dashboard reads:

- `results/runs_index.json`
- `profile.json`
- `benchmark.json`
- `summary.txt`
- generated PNG charts

It shows runs, programs, benchmark values, summaries and visualizations in a browser.

A key design decision was that the Web UI is read-only. It does not modify the C profiler, does not write to the registry and does not change result files. It is only a consumer of existing artifacts.

This separation kept the system understandable:

- C produces the data
- Python visualizes the data
- Flask displays the data

The Web UI also confirmed that the earlier registry and run directory decisions were useful. Because every run has a stable ID and path, the dashboard can load and display run data cleanly.

---

## 11. Server Test Environment

We developed and evaluated the final version on a dedicated Linux server.

### Hardware

- CPU: Intel Core i7-11700F
- Cores / Threads: 8 cores / 16 threads
- RAM: 32 GB DDR4
- GPU: NVIDIA RTX 3060 Ti, 8 GB VRAM

### Software

- Operating System: Ubuntu Server 24.04.4 LTS
- Linux Kernel: 6.8.0-111-generic
- Compiler: GCC 13.3.0
- Build system: GNU Make
- Main tracing interface: `ptrace()`
- Visualization: Python, pandas, matplotlib
- Web UI: Flask

We chose the server because it made the project feel closer to a real monitoring tool. Instead of only tracing one command on a laptop, we could collect many runs, store them centrally and inspect them through a Web UI.

---

## 12. Biggest Challenge

The biggest challenge was turning a simple command-line syscall tracer into a complete observability platform.

At the beginning, the main problem was technical: how do we trace syscalls with `ptrace()` and count them correctly? But every solved problem created a new question:

- If we can trace syscalls, how do we profile them?
- If we can profile them, how do we store the results?
- If we store the results, how do we avoid overwriting old runs?
- If we have many runs, how do we find them again?
- If we have JSON files, how do we make them understandable?
- If we have charts and summaries, how do we present them cleanly?
- If we have a dashboard, how do we keep it read-only and consistent?

This gradual evolution was the central engineering challenge. We learned that adding features is not only about writing more code. It also means deciding where responsibility belongs, what should be stored, what should be computed dynamically and how different parts of the system should communicate.

---

## 13. Lessons Learned

We learned a lot about operating systems, but also about software engineering.

### Understanding ptrace

Before this project, `ptrace()` was mostly an abstract word for us. We learned that it is event-driven and that the tracer must react to process stops. We also learned that syscall entry and exit must be handled separately.

### Understanding syscall behavior

We were surprised by how many syscalls simple programs make. Even `ls` performs many file and memory operations before the visible output appears. This made dynamic linking, process startup and OS abstractions much more concrete.

### Understanding interpretation

Collecting raw data is not enough. The first JSON files were technically correct, but hard to understand. The visualizer and summary system were our answer to that problem.

### Understanding benchmarking

We learned that benchmark results are not automatically true just because they are numbers. Especially for very short programs, the operating system scheduler, CPU state and cache effects can strongly influence results.

### Understanding project growth

We realized that the project became much larger because we kept asking how a real user would interact with the output. This pushed us from terminal traces toward persistent runs, summaries and the Web UI.

---

## 14. Limitations

The project has several limitations.

- It is Linux-only.
- It is designed for x86-64.
- `ptrace()` introduces measurable overhead.
- Timing values include tracing overhead.
- Benchmark results can be affected by scheduling and system load.
- Very short-running programs can produce inconsistent benchmark results.
- Some benchmark runs showed traced execution appearing faster than untraced execution.
- Not all benchmark anomalies could be fully explained.
- Not all syscall arguments are decoded.
- The Web UI is local and read-only, not a production monitoring service.
- The system does not yet provide live streaming or alerting.

We do not see these as failures. They are realistic engineering limitations of our chosen design.

---

## 15. Future Work

The current system is a strong foundation, but there are many possible extensions.

### eBPF backend

The most important technical improvement would be an eBPF backend. This could reduce overhead significantly and make the tool more suitable for production-like monitoring.

### Live monitoring

Instead of only showing completed runs, the Web UI could show syscall activity live while a program is running.

### Alerting

The system could detect suspicious behavior, for example unusual file access, unexpected process creation or high syscall volume.

### Historical trends

Because runs are already stored persistently, the Web UI could compare behavior over time. It could show whether a program became more syscall-heavy or whether overhead changed.

### Remote agents and multi-host support

A future version could run small agents on multiple servers and collect their results in one dashboard.

### AI-assisted observability agent

An ambitious extension would be an assistant that can answer natural-language questions about the server, for example:

```text
What happened on my server today?
Which program generated the most system calls?
Show me unusual behavior in the last 24 hours.
```

This could also be connected to Telegram or another chat interface.

---

## 16. Project Reflection and Conclusion

When we started, we thought we were building a syscall tracer. By the end, we had built something closer to a small observability platform.

The most important lesson was that observability is not only about gathering information. It is about making information understandable. A large trace file can contain valuable data, but if nobody can interpret it quickly, its usefulness is limited.

That realization shaped the project. We added summaries because raw syscall tables were hard to read. We added visualizations because charts communicate patterns faster than JSON. We added a registry because many runs need structure. We added a Web UI because browsing a dashboard is easier than manually opening folders.

We also learned that every design decision has consequences. Choosing `ptrace()` made the project understandable and educational, but introduced overhead. Storing run metadata in a registry made the Web UI easier, but forced us to think carefully about stale state. Generating summaries without AI made the output reproducible, but required us to define clear rules.

Overall, we are happy with the final result. The project started as a simple local tracer and evolved into a system that can collect, analyze, visualize and compare syscall behavior through a unified workflow. It helped us understand operating systems more deeply, especially system calls, process control, tracing overhead and the difficulty of turning low-level data into useful information.

---

## References

- Linux manual pages: `ptrace(2)`, `waitpid(2)`, `fork(2)`, `clone(2)`, `execve(2)`, `clock_gettime(2)`
- Linux x86-64 syscall table
- Operating Systems course material, University of Basel, Spring Semester 2026
- Silberschatz, Galvin, Gagne: Operating System Concepts
- Michael Kerrisk: The Linux Programming Interface
- Flask documentation
- pandas documentation
- matplotlib documentation
- strace project documentation
- Brendan Gregg: BPF Performance Tools

# Project Report — Syscall Profiler

*Operating Systems course project*

This report is the story of how we built our system call profiler. It is written as a
development diary rather than a formal paper, because that is honestly how the project
felt: we started with a small idea, kept running into things we did not expect, and ended
up somewhere much larger than we planned. We have tried to be honest about what we got
right, what surprised us, and what we still do not fully understand.

---

## How the project started

We began with a simple goal. We wanted to write a program that could watch another program
and tell us which system calls it made. The plan was modest: trace a process on a normal
Ubuntu machine, count the system calls, time them, and print a table at the end. That was
it. A command-line tool for a single laptop.

We got that working fairly quickly. We could run `./profiler ls` and see every `openat`,
`read`, `mmap`, and `close` that `ls` performed, with counts and timings. It was satisfying
the first time we saw it, because suddenly a program we ran every day became transparent —
we could see exactly what it asked the kernel to do.

But almost immediately we realized two things. First, the raw output was hard to read.
Second, a single laptop running the tool occasionally was not a very interesting way to
use it. Both of those realizations pushed the project in directions we had not originally
planned, and most of this report is about those directions.

---

## Why we chose ptrace()

Before writing any tracing code we had to decide *how* to observe system calls. We looked
at several options and discussed the trade-offs as a team.

**eBPF.** This was the most powerful option. eBPF can hook into the kernel with very low
overhead and is what serious, production-grade observability tools use today. We were
genuinely tempted. But the more we read, the more we realized eBPF would have pulled the
project in a direction we did not want for a course project: writing and loading kernel
programs, dealing with the verifier, managing toolchains, and debugging in kernel space.
We decided that eBPF would almost certainly give us lower overhead and far better
scalability, but at the cost of a steep increase in complexity that would have left us less
time to actually understand system calls themselves.

**auditd.** The Linux audit subsystem can log system calls too. But it is more of a
security-auditing facility than a profiling tool. It would have given us logs to parse
rather than a live, programmatic view of a process, and configuring it felt like working
*around* a system rather than learning how tracing actually works.

**Kernel modules.** We briefly considered writing our own kernel module. We quickly talked
ourselves out of it. A bug in user space crashes our program; a bug in a kernel module can
take down the whole machine. For a learning project that was the wrong place to spend our
risk budget.

**ptrace().** In the end we chose `ptrace()`, and we are happy with that choice. The
reasons that mattered to us were:

- It is available on any standard Linux system, with no custom kernel modules and no
  special configuration.
- It gives direct, immediate visibility into every system call a process makes — exactly
  the thing we wanted to study.
- Its complexity is right for an Operating Systems course: hard enough to teach us a lot,
  but contained entirely in user space.
- It was far easier to develop and debug. We could set breakpoints, print things, and
  iterate quickly, because everything ran as an ordinary program.
- The educational value was high. Using `ptrace` forced us to actually understand
  process control, signals, `wait()` semantics, and how a tracer and tracee interact.

We want to be clear that this was a deliberate trade-off and not us pretending `ptrace` is
the best tool for every job. For a real high-traffic server we would reach for eBPF. For
*learning how system call tracing works*, `ptrace` was the right choice, and we would make
the same decision again.

---

## Learning ptrace the hard way

Getting the first trace working taught us more than we expected. We learned that the child
calls `PTRACE_TRACEME` and then `execve`, that the parent waits for it to stop, and that
the tracee stops twice per system call — once on entry and once on exit. We learned to use
`PTRACE_O_TRACESYSGOOD` so we could tell a syscall-stop apart from an ordinary signal.

The part that genuinely caught us out was following children and threads. Our early
version only traced the top-level process, which was fine for `ls` but wrong for anything
that forked or spawned threads. We discovered that we needed `PTRACE_O_TRACEFORK`,
`PTRACE_O_TRACEVFORK`, and `PTRACE_O_TRACECLONE` so the kernel would automatically attach
to new children and threads, and that we then had to track every traced process in a small
table and wait on *any* of them. We also added `PTRACE_O_EXITKILL` so that if our profiler
died, it would not leave orphaned, stopped processes behind — a lesson we learned after
leaving a few stuck processes on the machine.

We timed each system call with `CLOCK_MONOTONIC`, because it does not jump around when the
system clock is adjusted. None of this was conceptually huge, but every piece had to be
exactly right, and the debugging taught us how subtle process control really is.

---

## Why we moved to a server edition

Early in development we made a decision that changed the whole shape of the project: we
moved it onto a dedicated Ubuntu server instead of running it on a laptop now and then.

We did not do this just because it sounded more interesting. We did it because we realized
a dedicated server would let us explore the kind of scenarios that observability tools
actually exist for. On a laptop, a profiling run is a one-off: you run it, you read the
output, you close the terminal, and the data is gone. On a server, things look different:

- The tool can be **permanently available**, ready whenever we want to profile something.
- Runs can be **stored centrally** in one place instead of scattered across machines.
- We can **collect and compare many runs over time** rather than looking at one in
  isolation.
- We can serve a **Web UI dashboard** that we reach **remotely** from any browser.
- The whole workflow starts to resemble a **real monitoring system** rather than a
  classroom exercise.

That decision expanded the scope dramatically. Once runs lived on a server and accumulated
over time, we needed a way to keep track of them — which led to the run registry. Once we
had many runs, we wanted to measure and compare their cost — which led to the benchmarking
system. Once we were storing all this data, we wanted to actually understand it — which led
to the visualization pipeline and, finally, the Web UI. The server decision is the single
choice that turned a tracing tool into an observability platform.

---

## Why visualization became necessary

At first the profiler only produced JSON. Technically that was complete — every number we
could possibly want was in there. But we quickly discovered an uncomfortable truth: raw
syscall data is genuinely hard to interpret.

A long list of syscall names with counts and timings does not actually tell you what a
program is *doing*. We would stare at a table with `openat: 7`, `mmap: 17`, `read: 7`,
`getdents64: 2` and have to reconstruct in our heads that "this program opened some files,
loaded its shared libraries, read some data, and listed a directory." We were doing
interpretation work by hand, every time, and we realized that anyone using the tool would
have to do the same.

So we started adding layers whose only purpose was to make the data understandable:

- A **visualization pipeline** that draws the data as charts, so distribution and
  proportion are visible at a glance instead of being computed mentally.
- **Automated behavior summaries** that describe a run in plain English.
- **Benchmark visualizations** so overhead is something you can see, not just a number.
- A **Web UI dashboard** that brings runs, profiles, benchmarks, summaries, and charts
  together in one place.

Each of these steps took low-level trace data and moved it one step closer to something a
human can read quickly. In hindsight this was the real heart of the project, even though it
was not part of our original plan at all.

---

## How the automated behavior summaries work (and why they are not AI)

We want to explain this part carefully, because it is easy to assume that a tool which
writes English sentences about a program must be using an AI model. **It is not.** Our
summaries are generated **deterministically** by the visualization pipeline using
rule-based logic. The same `profile.json` always produces exactly the same summary text.
There is no AI model, no language model, no network API, and no randomness involved.

Conceptually, here is what happens when we generate a summary:

1. **`visualize.py` loads `profile.json`** for the run and reads the per-syscall table
   into a data frame.

2. **Syscalls are grouped into categories** using the same behavioral categories the
   profiler assigns — File System, Memory, Network, Process Management, Signal, IPC, Time,
   and Other.

3. **The pipeline counts things** it knows how to talk about:
   - the total number of system calls and the number of distinct types;
   - how many calls fall into each category;
   - the dominant category and its share of all calls;
   - specific, easy-to-explain syscalls — `open`/`openat`, `read`, `write`,
     `getdents`/`getdents64` (directory scanning);
   - memory-mapping activity (`mmap`);
   - how many low-frequency syscall types were grouped into the "other" bucket, and what
     share of all calls that represents.

4. **Rule-based heuristics decide what to say.** For example:
   - if the dominant category is File System, the summary says the activity was mostly
     file-system related, with its percentage;
   - if there are several `open`/`read`/`write`/directory-scan calls, it states them
     explicitly ("opened files 7 times, read data 7 times, ...");
   - if there are at least a few `mmap` calls, it notes that memory-mapping activity was
     mostly caused by loading shared libraries at startup;
   - it explains the "other" group so the reader knows those calls were folded together
     only to keep the charts readable.

5. **Predefined templates are filled** with the collected statistics. The English
   sentences are fixed templates with the real numbers substituted in — for instance,
   *"The program opened files X times and read data Y times."*

6. **The result is written to `summary.txt`** in the run directory.

A real example, produced for `ls`, looks like this:

```
Program traced: ls

Most activity was file-system related (57%).
The program opened files 7 times, read data 7 times, wrote data 1 time, and scanned directories 2 times.
Memory mapping activity (17 mmap calls) was mostly caused by loading shared libraries at startup.
The remaining 5 syscall types were grouped into 'other' and accounted for 6.8% of all observed system calls. These are lower-frequency operations grouped together to keep the visualization readable.

In total: 74 syscalls across 20 distinct types.
```

We deliberately chose this rule-based approach over anything fancier because it gives us:

- **reproducible summaries** — the same input always gives the same text;
- **deterministic output** — nothing depends on sampling or external state;
- **no external dependency** — no AI service, no network, nothing to break or pay for;
- **transparent logic** — every sentence can be traced back to a specific rule we wrote.

One thing we are quietly proud of: we also made sure the numbers in the summary match the
charts exactly. Early on, the "other" count in the summary disagreed with the distribution
chart because they were computed in two different places. We fixed this by centralizing
that calculation so the summary, the distribution chart, and the combined report all use
the same shared computation. It was a small bug, but it taught us that consistency between
different views of the same data has to be engineered on purpose.

The whole point of these summaries is that a user should be able to understand what a
program did without ever reading the raw syscall table themselves.

---

## Benchmarking, and a result we still cannot fully explain

We wanted to know how much our tracing actually cost, so we built a benchmarking mode. It
times the target program in two ways: untraced (a plain fork/exec/wait) and traced (a
stripped-down `ptrace(PTRACE_SYSCALL)` loop with no decoding or output, to isolate pure
tracing cost). To reduce noise we run each mode three times and keep the **minimum** of
each, on the theory that the minimum is the cleanest, least-contended measurement.

For most programs the results matched our intuition: tracing adds overhead, sometimes a
lot of it, because the tracee stops twice per system call and the kernel has to context
switch to our profiler each time.

But then we hit something strange. In a small number of runs, the *traced* execution came
out **faster** than the untraced one. One run we kept around looked like this:

```
normal   = 2.804 ms
traced   = 1.010 ms
overhead = 0.36x
```

That is obviously not real. Tracing cannot make a program genuinely faster — it can only
add work. We saw the same kind of thing in our own final test runs, where an untraced `ls`
measured around 2.07 ms and the traced run measured around 1.76 ms, giving an "overhead"
below 1.0.

We spent a while trying to explain it and discussed several plausible causes:

- **Measurement noise.** For programs that finish in a couple of milliseconds, the timing
  noise is on the same order as the thing we are measuring.
- **Scheduling effects.** The OS scheduler decides when our processes actually run, and a
  lucky or unlucky scheduling slot can swamp the real difference.
- **CPU frequency scaling.** The CPU changes clock speed dynamically. If it ramped up
  between the untraced and traced runs, the later run can finish faster for reasons that
  have nothing to do with tracing.
- **Cache effects.** By the time the traced run executes, the program's pages and the
  loader's work may already be warm in cache, making it faster than the "cold" untraced
  run.
- **Extremely short runtimes.** All of the above hit hardest when the program barely runs
  at all, which is exactly when we saw the anomaly.

We are honest about the fact that we did **not** fully solve this. We are fairly confident
it is a measurement artifact rather than a real speedup, and taking the minimum of several
runs reduced how often it happened, but it did not eliminate it for very short programs. We
have left it as an interesting observation rather than pretending we have a complete
explanation, because that is the truth of where we ended up.

---

## The biggest challenge

If we had to name the single hardest part of the project, it was not any one piece of code.
It was this: **turning a simple command-line syscall tracer into a complete observability
platform.**

When we look back, the project grew in distinct stages, and each stage forced new design
decisions:

- **Syscall tracing.** Getting `ptrace` working, then following children and threads
  correctly.
- **Persistent run storage.** Once we ran things repeatedly, we needed a consistent place
  to put results, which led to the `results/<program>/<timestamp>/` layout, unique run
  IDs, and handling the case where two runs happen in the same second.
- **Benchmarking.** Measuring overhead reliably, which forced us to think about noise,
  repetition, and what a "fair" measurement even is.
- **Visualization.** Turning numbers into pictures, which is a completely different skill
  from systems programming.
- **Automated summaries.** Deciding what is worth saying about a run and writing rules that
  say it consistently.
- **The registry system.** Indexing every run so other tools could find them without
  rescanning the disk, and doing the index writes atomically so a crash could not corrupt
  it.
- **The Web UI dashboard.** Presenting all of the above in a browser, read-only and safe.

Each new feature was not just "more code." Each one introduced its own design questions:
how to store data so future tools could read it, how to keep different components from
stepping on each other, how to keep the data layer independent from the presentation
layer. The challenge was less about any single algorithm and more about growing a small
program into a coherent system without it turning into a mess.

---

## Lessons learned

Looking back, here is what we actually took away from this project:

- **Understanding `ptrace`.** We now genuinely understand how a tracer attaches to a
  tracee, how syscall-stops work, and how to follow forks and threads.
- **Understanding syscall behavior.** We learned to recognize what programs do from their
  system calls — library loading via `mmap`, directory scanning via `getdents64`, and so
  on.
- **Raw traces are hard to interpret.** Maybe the biggest lesson: complete data is not the
  same as understandable data.
- **Separating collection from visualization.** Keeping the C profiler (collection) cleanly
  separate from the Python visualizer (interpretation) made both far easier to work on.
- **Storing runs consistently.** A predictable directory and naming scheme made everything
  downstream simpler.
- **Building a registry.** We learned why an index is worth having and why writing it
  atomically matters.
- **Generating useful summaries automatically.** Writing rules that produce consistent,
  human-readable text was harder and more interesting than we expected.
- **Designing a user-friendly Web UI.** We learned to think about a read-only API,
  artifact-based status, and not letting users break things through the interface.
- **Benchmarking overhead correctly.** We learned how noisy real measurements are and why
  methodology (repetition, minimums, isolating the traced loop) matters.
- **Deploying on a real Linux server.** Running the tool on a dedicated server taught us
  about persistence, remote access, and the difference between a script and a system.

The throughline of all of these is that our understanding deepened in layers, the same way
the project itself grew in layers.

---

## Limitations

We want to be honest about what our tool cannot do:

- It is a **Linux-only** solution, tied to `ptrace` and the x86-64 syscall table.
- `ptrace` introduces **measurable overhead**, because the tracee stops twice per system
  call. This is fine for analysis but not for production-grade, always-on tracing.
- **Benchmark results are influenced by OS scheduling**, CPU frequency scaling, and cache
  behavior, so they vary between runs.
- **Extremely short-running programs** sometimes produced inconsistent benchmark results.
- In a small number of runs, **traced programs appeared faster than untraced programs**,
  which is clearly a measurement artifact rather than a real effect.
- We believe these anomalies come from **measurement noise, scheduling, cache behavior, or
  CPU frequency scaling**, but we could **not fully explain every case**.

We see these as realistic engineering limitations of the approach we chose, not as failures
of the project.

---

## Future work

We designed the system so the data layer and the presentation layer can grow
independently, and most of our future ideas are about growing it into a fuller
observability platform:

- An **eBPF backend** as an alternative to `ptrace`, for much lower overhead and far better
  scalability on busy systems.
- A **live monitoring dashboard** with **real-time syscall streaming** into the Web UI.
- An **alerting system** that flags suspicious behavior, such as unexpected network or
  process activity.
- **Historical trend analysis** and **long-term run comparison** across many runs.
- A **remote agent architecture** and **distributed monitoring across multiple servers**,
  with **multi-host support**, so several machines report into one console.
- An **AI-assisted observability agent** that answers natural-language questions like
  *"What happened on my server today?"*, *"Which program generated the most system
  calls?"*, or *"Show me unusual behavior in the last 24 hours."*
- **Telegram integration** so the platform can notify us about important events.

These are ambitious, but every one of them is a natural extension of what we already built.

---

## Final reflection and conclusion

When we started, we thought we were building a syscall tracer. We finished having built
something quite different, and the gap between those two things is the most important thing
we learned.

The project began as a relatively simple tracer: run a program, collect its system calls,
print them. But during development we kept bumping into the same realization — collecting
raw system calls is, on its own, not very useful to most people. A trace file can contain
enormous amounts of valuable information, but actually *interpreting* that information takes
real effort and a fair bit of operating-system knowledge. We were doing that interpretation
in our heads every single time, and that did not scale.

So our focus gradually shifted. We stopped asking "how do we collect more data?" and
started asking "how do we make this data understandable?" That shift is what produced the
automated behavior summaries, the benchmarking support, the visualization pipeline, the run
registry, and the Web UI dashboard. None of those were in the original plan. All of them
exist because we realized that data you cannot understand is not much better than no data
at all.

The most important lesson of the whole project is this: **observability is not only about
gathering information — it is about presenting information in a way that humans can
understand and act upon.** Throughout the project we kept coming back to the same three
goals: improving the **interpretability**, the **usability**, and the **accessibility** of
the data we collected. Almost every feature we are proud of exists to serve one of those
three.

The result is that our final system is no longer just a syscall tracer. It evolved into a
small observability platform — one that lets a user collect, analyze, visualize, and
compare system call behavior through a single, unified workflow. We did not set out to
build that. We built it because the project kept teaching us what was actually missing, and
we kept following where the data led.

---

## Test environment

We developed and tested the project on a dedicated Ubuntu server. We chose a dedicated
machine deliberately: we wanted the tool to be permanently available, to accumulate many
runs over time, and to be reachable remotely through the Web UI — a setup much closer to a
real monitoring environment than a laptop used occasionally.

**Hardware**

| Component | Specification                                  |
|-----------|------------------------------------------------|
| CPU       | Intel Core i7-11700F, 8 cores / 16 threads     |
| RAM       | 32 GB DDR4                                      |
| GPU       | NVIDIA RTX 3060 Ti, 8 GB VRAM                   |
| Storage   | 1 TB NVMe SSD + 1 TB HDD + 4 TB external SSD    |

**Software**

| Component  | Version                     |
|------------|-----------------------------|
| OS         | Ubuntu Server 24.04.4 LTS   |
| Kernel     | 6.8.0-111-generic           |
| Compiler   | GCC 13.3.0                  |
| Build      | GNU Make                    |
| Tracing    | Linux `ptrace()`            |

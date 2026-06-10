#!/usr/bin/env python3
"""
visualize.py — System Call Profiler Visualization Script (P7)

Reads a JSON file produced by:
    ./profiler --json=profile.json -q <program>

Generates 4 charts saved as PNG files:
    1. syscall_counts.png     — bar chart: top N syscalls by call count
    2. syscall_distribution.png — pie chart: % of total calls per syscall
    3. category_breakdown.png — bar chart: calls grouped by category
    4. slowest_syscalls.png   — horizontal bar: top N by average time (ms)

And one combined overview:
    5. syscall_report.png     — all 4 charts on a single page

USAGE:
    python3 visualize.py profile.json
    python3 visualize.py profile.json --top=10
    python3 visualize.py profile.json --out=./charts/
    python3 visualize.py profile.json --top=8 --out=./output/

OPTIONS:
    profile.json   Path to JSON file from --json= export
    --top=N        How many syscalls to show in bar charts (default: 15)
    --out=DIR      Output directory for PNG files (default: same dir as JSON)
    --no-combined  Skip generating the combined overview image

DEPENDENCIES:
    pip install matplotlib pandas
"""

import json
import sys
import os
import argparse

import pandas as pd
import matplotlib
matplotlib.use("Agg")          # non-interactive backend — no display needed
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.gridspec import GridSpec

# This block only changes *appearance*. No data, statistics, filenames, output
# paths, or chart contents are affected.
PAPER = "#fafaf8"   
INK   = "#0d0d0d"  
GRID  = "#d0cdc7"   
MUTED = "#8a8a8a"   

SERIF      = ["serif"]
SANS_STACK = ["sans-serif"]
MONO_STACK = ["monospace"]

# Preferred IBM Plex family names, by role.
_PLEX_SERIF = "IBM Plex Serif"
_PLEX_SANS  = "IBM Plex Sans"
_PLEX_MONO  = "IBM Plex Mono"


def _register_fonts():
    """Best-effort: register IBM Plex Serif / Sans / Mono if TTF/OTF files
    are available (system fonts, or webui/static/fonts). woff2 is ignored
    because matplotlib cannot read it. If IBM Plex is not present we fall back
    silently to the generic serif / sans-serif / monospace families."""
    try:
        import glob
        from matplotlib import font_manager
        here = os.path.dirname(os.path.abspath(__file__))
        for d in (os.path.join(here, "webui", "static", "fonts"),):
            for ext in ("*.ttf", "*.otf"):
                for fp in glob.glob(os.path.join(d, ext)):
                    try:
                        font_manager.fontManager.addfont(fp)
                    except Exception:
                        pass
    except Exception:
        pass


def _resolve_font_stacks():
    """Build the family stacks, including an IBM Plex face only when it is
    actually installed. Naming an absent font would make matplotlib emit a
    'findfont: Font family not found' warning, so absent Plex faces are simply
    omitted and the generic family is used instead (silent on a clean Ubuntu)."""
    global SERIF, SANS_STACK, MONO_STACK
    try:
        from matplotlib import font_manager
        have = {f.name for f in font_manager.fontManager.ttflist}
    except Exception:
        have = set()
    SERIF      = ([_PLEX_SERIF] if _PLEX_SERIF in have else []) + ["serif"]
    SANS_STACK = ([_PLEX_SANS]  if _PLEX_SANS  in have else []) + ["sans-serif"]
    MONO_STACK = ([_PLEX_MONO]  if _PLEX_MONO  in have else []) + ["monospace"]


def setup_style():
    """Apply the editorial/print look globally. Call once before plotting."""
    _register_fonts()
    _resolve_font_stacks()
    plt.rcParams.update({
        "figure.facecolor":  PAPER,
        "savefig.facecolor": PAPER,
        "axes.facecolor":    PAPER,
        "font.family":       MONO_STACK,
        "font.size":         10,
        "text.color":        INK,
        "axes.edgecolor":    INK,
        "axes.labelcolor":   INK,
        "axes.linewidth":    0.8,
        "axes.titlecolor":   INK,
        "xtick.color":       INK,
        "ytick.color":       INK,
        "xtick.labelsize":   9,
        "ytick.labelsize":   9,
        "axes.grid":         False,
        "grid.color":        GRID,
        "grid.linewidth":    0.6,
        "legend.frameon":    False,
        "legend.fontsize":   8,
    })


# Colour palette

CATEGORY_COLORS = {
    "FILE": "#2d5fa3",   # blue
    "MEM ": "#8a6a2f",   # ochre / brown
    "NET ": "#7c3f3f",   # muted red
    "PROC": "#5a4b8a",   # muted violet
    "SIG ": "#9c5a3f",   # muted terracotta
    "IPC ": "#2f7a6a",   # muted teal
    "TIME": "#8a8a8a",   # muted grey
    "SYS ": "#666666",   # dark grey
}
DEFAULT_COLOR = "#b8b4ac"

def category_color(cat):
    """Return the plot colour for a category string."""
    return CATEGORY_COLORS.get(cat.strip().upper().ljust(4), DEFAULT_COLOR)

# A plain-language explanation for the most common syscalls
SYSCALL_MEANINGS = {
    "read":         "read file contents",
    "write":        "write data",
    "open":         "open file",
    "openat":       "open file",
    "openat2":      "open file",
    "close":        "close file descriptor",
    "stat":         "get file info",
    "fstat":        "get file info (by fd)",
    "lstat":        "get link info",
    "newfstatat":   "get file info (at path)",
    "statx":        "get extended file info",
    "statfs":       "get filesystem info",
    "lseek":        "move file position",
    "access":       "check file permissions",
    "faccessat":    "check file permissions",
    "faccessat2":   "check file permissions",
    "getdents64":   "enumerate directory entries",
    "getdents":     "enumerate directory entries",
    "readlink":     "read symlink target",
    "readlinkat":   "read symlink target",
    "mmap":         "map memory pages",
    "munmap":       "unmap memory pages",
    "mprotect":     "change memory protection",
    "mremap":       "resize memory mapping",
    "brk":          "adjust heap size",
    "madvise":      "advise kernel on memory use",
    "ioctl":        "device control operation",
    "fcntl":        "file descriptor control",
    "dup":          "duplicate file descriptor",
    "dup2":         "duplicate file descriptor",
    "dup3":         "duplicate file descriptor",
    "pipe":         "create pipe",
    "pipe2":        "create pipe",
    "socket":       "create network socket",
    "connect":      "open network connection",
    "accept":       "accept connection",
    "accept4":      "accept connection",
    "bind":         "bind socket to address",
    "listen":       "listen for connections",
    "sendto":       "send network data",
    "recvfrom":     "receive network data",
    "sendmsg":      "send network message",
    "recvmsg":      "receive network message",
    "clone":        "create process/thread",
    "clone3":       "create process/thread",
    "fork":         "create child process",
    "vfork":        "create child process",
    "execve":       "run a new program",
    "execveat":     "run a new program",
    "exit":         "terminate process",
    "exit_group":   "terminate all threads",
    "wait4":        "wait for child process",
    "waitid":       "wait for child process",
    "kill":         "send signal to process",
    "tgkill":       "send signal to thread",
    "getpid":       "get process ID",
    "getppid":      "get parent process ID",
    "gettid":       "get thread ID",
    "getuid":       "get user ID",
    "geteuid":      "get effective user ID",
    "getgid":       "get group ID",
    "getegid":      "get effective group ID",
    "rt_sigaction": "set signal handler",
    "rt_sigprocmask": "change signal mask",
    "rt_sigreturn": "return from signal handler",
    "sigaltstack":  "set signal stack",
    "futex":        "fast user-space lock",
    "set_robust_list": "register robust futex list",
    "get_robust_list": "get robust futex list",
    "epoll_create1": "create epoll instance",
    "epoll_ctl":    "configure epoll watch",
    "epoll_wait":   "wait for I/O events",
    "poll":         "wait for I/O on fds",
    "ppoll":        "wait for I/O on fds",
    "select":       "wait for I/O on fds",
    "nanosleep":    "sleep for a duration",
    "clock_gettime": "read a clock",
    "clock_nanosleep": "sleep until time",
    "gettimeofday": "get wall-clock time",
    "time":         "get current time",
    "arch_prctl":   "set arch-specific thread state",
    "prctl":        "process control operation",
    "prlimit64":    "get/set resource limits",
    "getrlimit":    "get resource limits",
    "setrlimit":    "set resource limits",
    "set_tid_address": "register thread ID address",
    "rseq":         "register restartable sequence",
    "getrandom":    "get random bytes",
    "sysinfo":      "get system statistics",
    "uname":        "get system name/version",
    "sched_yield":  "yield the CPU",
    "sched_getaffinity": "get CPU affinity",
    "membarrier":   "issue memory barrier",
}

def syscall_meaning(name):
    """Return a plain-language meaning for a syscall, or '' if unknown."""
    return SYSCALL_MEANINGS.get(name, "")


#  Data loading 

def load_profile(path):
    """
    Load the JSON produced by --json= and return a pandas DataFrame.

    Columns: number, name, category, count, total_ms, avg_ms
    Also returns the traced program string (VIS-002), or "" if absent
    (older JSON files without the "program" field still work).
    """
    with open(path, "r") as f:
        data = json.load(f)

    total    = data["total_syscalls"]
    unique   = data["unique_syscalls"]
    syscalls = data["syscalls"]
    program  = data.get("program", "")   

    df = pd.DataFrame(syscalls)
    df["category"] = df["category"].str.strip() 
    df["pct"]      = df["count"] / total * 100    # percentage of total calls

    return df, total, unique, program


# Shared distribution calculation 

def compute_distribution_data(df, top_n):

    top  = df.nlargest(top_n, "count")
    rest = df[~df.index.isin(top.index)]

    other_count = len(rest)
    other_pct   = float(rest["pct"].sum()) if other_count > 0 else 0.0

    return {
        "top":         top,
        "rest":        rest,
        "other_count": other_count,
        "other_pct":   other_pct,
        "has_other":   other_count > 0,
    }


#  Chart 1: Top N syscalls by count

def chart_counts(df, top_n, out_path):

    top = df.nlargest(top_n, "count").sort_values("count", ascending=False)
    colors = [category_color(c) for c in top["category"]]

    fig, ax = plt.subplots(figsize=(12, 5))
    bars = ax.bar(top["name"], top["count"], color=colors, edgecolor=INK,
                  linewidth=0.8, zorder=2)

    # Value labels on each bar
    for bar, val in zip(bars, top["count"]):
        ax.text(bar.get_x() + bar.get_width() / 2,
                bar.get_height() + max(top["count"]) * 0.01,
                str(int(val)), ha="center", va="bottom",
                fontsize=8, color=INK, fontweight="bold")

    ax.set_title(f"Top {top_n} Syscalls by Call Count",
                 fontsize=15, fontweight="bold", pad=16, fontfamily=SERIF, color=INK)
    ax.set_xlabel("Syscall", fontsize=11)
    ax.set_ylabel("Number of Calls", fontsize=11)
    ax.set_xticks(range(len(top)))
    ax.set_xticklabels(top["name"], rotation=40, ha="right", fontsize=9)
    ax.yaxis.grid(True, linestyle="-", linewidth=0.6, color=GRID, alpha=1.0, zorder=0)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

    # Legend for category colours
    legend_patches = [
        mpatches.Patch(color=col, label=cat.strip())
        for cat, col in CATEGORY_COLORS.items()
        if cat.strip() in top["category"].values
    ]
    if legend_patches:
        ax.legend(handles=legend_patches, title="Category",
                  loc="upper right", fontsize=8, title_fontsize=8)

    fig.tight_layout()
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  [OK] {out_path}")


# Chart 2: Syscall distribution 

def chart_distribution(df, top_n, out_path, program=""):

    data = compute_distribution_data(df, top_n)
    top  = data["top"]
    rest = data["rest"]

    # Build rows: each syscall name + its percentage, plus an "other" row
    names = list(top["name"])
    pcts  = list(top["pct"])
    cats  = list(top["category"])

    if data["has_other"]:
        names.append(f"other ({data['other_count']})")
        pcts.append(data["other_pct"])
        cats.append("OTHER")

    # Sort ascending so the largest bar ends up at the top of the chart
    order = sorted(range(len(names)), key=lambda k: pcts[k])
    names = [names[k] for k in order]
    pcts  = [pcts[k]  for k in order]
    cats  = [cats[k]  for k in order]
    colors = [category_color(c) if c != "OTHER" else "#b8b4ac" for c in cats]

    fig, ax = plt.subplots(figsize=(11, max(4, len(names) * 0.45)))
    bars = ax.barh(range(len(names)), pcts, color=colors,
                   edgecolor=INK, linewidth=0.8, zorder=2)

    # Percentage label at the end of each bar
    max_pct = max(pcts) if pcts else 1
    for i, (bar, pct) in enumerate(zip(bars, pcts)):
        ax.text(bar.get_width() + max_pct * 0.012,
                bar.get_y() + bar.get_height() / 2,
                f"{pct:.1f}%", va="center", ha="left",
                fontsize=9, fontweight="bold", color=INK)

    # Y labels: syscall name + plain-language meaning 
    ylabels = []
    for n in names:
        base = n.split(" ")[0]               
        meaning = syscall_meaning(base)
        if meaning and not n.startswith("other"):
            ylabels.append(f"{n}  -  {meaning}")
        else:
            ylabels.append(n)
    ax.set_yticks(range(len(names)))
    ax.set_yticklabels(ylabels, fontsize=9)

    title = "Syscall Distribution (% of total calls)"
    if program:
        title += f"\nProgram: {program}"          
    ax.set_title(title, fontsize=15, fontweight="bold", pad=16, fontfamily=SERIF, color=INK)
    ax.set_xlabel("Percentage of total syscalls", fontsize=11)
    ax.set_xlim(0, max_pct * 1.15)
    ax.xaxis.grid(True, linestyle="-", linewidth=0.6, color=GRID, alpha=1.0, zorder=0)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

    # Small category legend 
    present = [c for c in CATEGORY_COLORS if c.strip() in
               [x.strip() for x in cats]]
    legend_patches = [mpatches.Patch(color=CATEGORY_COLORS[c], label=c.strip())
                      for c in present]
    if legend_patches:
        ax.legend(handles=legend_patches, title="Category",
                  loc="lower right", fontsize=8, title_fontsize=8)

    fig.tight_layout()
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  [OK] {out_path}")


#  Chart 3: Calls grouped by category 

def chart_categories(df, out_path):
    
    cat_totals = (
        df.groupby("category")["count"]
        .sum()
        .reset_index()
        .sort_values("count", ascending=True)
    )
    colors = [category_color(c) for c in cat_totals["category"]]

    fig, ax = plt.subplots(figsize=(9, max(3, len(cat_totals) * 0.65)))
    bars = ax.barh(cat_totals["category"], cat_totals["count"],
                   color=colors, edgecolor=INK, linewidth=0.8, zorder=2)

    # Value labels
    for bar, val in zip(bars, cat_totals["count"]):
        ax.text(bar.get_width() + cat_totals["count"].max() * 0.01,
                bar.get_y() + bar.get_height() / 2,
                f"{int(val):,}", va="center", ha="left",
                fontsize=9, fontweight="bold", color=INK)

    ax.set_title("Syscalls by Category",
                 fontsize=15, fontweight="bold", pad=16, fontfamily=SERIF, color=INK)
    ax.set_xlabel("Total Calls", fontsize=11)
    ax.set_ylabel("Category", fontsize=11)
    ax.xaxis.grid(True, linestyle="-", linewidth=0.6, color=GRID, alpha=1.0, zorder=0)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

    fig.tight_layout()
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  [OK] {out_path}")


#  Chart 4: Top N slowest syscalls by average time 

def chart_slowest(df, top_n, out_path):
    
    # Only consider syscalls that were actually called
    called = df[df["count"] > 0].copy()
    top    = called.nlargest(top_n, "avg_ms").sort_values("avg_ms")
    colors = [category_color(c) for c in top["category"]]

    fig, ax = plt.subplots(figsize=(10, max(3, len(top) * 0.6)))
    bars = ax.barh(top["name"], top["avg_ms"],
                   color=colors, edgecolor=INK, linewidth=0.8, zorder=2)

    # Value labels
    for bar, val in zip(bars, top["avg_ms"]):
        ax.text(bar.get_width() + top["avg_ms"].max() * 0.01,
                bar.get_y() + bar.get_height() / 2,
                f"{val:.4f} ms", va="center", ha="left",
                fontsize=8, color=INK)

    ax.set_title(f"Top {top_n} Slowest Syscalls (Average Execution Time)",
                 fontsize=15, fontweight="bold", pad=16, fontfamily=SERIF, color=INK)
    ax.set_xlabel("Average Time (ms)", fontsize=11)
    ax.set_ylabel("Syscall", fontsize=11)
    ax.xaxis.grid(True, linestyle="-", linewidth=0.6, color=GRID, alpha=1.0, zorder=0)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

    # Disclaimer about ptrace overhead
    fig.text(0.98, 0.02,
             "WARNING: Times include ptrace overhead - use for relative comparison only.",
             ha="right", va="bottom", fontsize=7, color=MUTED,
             style="italic")

    # Legend
    legend_patches = [
        mpatches.Patch(color=col, label=cat.strip())
        for cat, col in CATEGORY_COLORS.items()
        if cat.strip() in top["category"].values
    ]
    if legend_patches:
        ax.legend(handles=legend_patches, title="Category",
                  loc="lower right", fontsize=8, title_fontsize=8)

    fig.tight_layout()
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  [OK] {out_path}")


#  Chart 5: Combined overview

def chart_combined(df, top_n, total_calls, unique_calls, out_path, program=""):
    
    fig = plt.figure(figsize=(18, 12))
    fig.patch.set_facecolor(PAPER)

    gs = GridSpec(2, 2, figure=fig, hspace=0.45, wspace=0.35)

    #  Title block 
    prog_label = f"Program: {program}" if program else "System Call Profile"
    fig.suptitle(prog_label, fontsize=24, fontstyle="italic",
                 fontfamily=SERIF, color=INK, y=0.995)
    fig.text(0.5, 0.963,
             f"System Call Profile   -   {total_calls:,} total calls"
             f"   -   {unique_calls} unique syscalls",
             ha="center", va="top", fontsize=11, fontfamily=SANS_STACK,
             color=MUTED, transform=fig.transFigure)

    fig.add_artist(plt.Line2D([0.07, 0.93], [0.948, 0.948],
                              color=INK, linewidth=0.8,
                              transform=fig.transFigure))

    # Panel 1: counts bar 
    ax1  = fig.add_subplot(gs[0, 0])
    top  = df.nlargest(min(top_n, 12), "count").sort_values("count", ascending=False)
    cols = [category_color(c) for c in top["category"]]
    bars = ax1.bar(top["name"], top["count"], color=cols,
                   edgecolor=INK, linewidth=0.7, zorder=2)
    for bar, val in zip(bars, top["count"]):
        ax1.text(bar.get_x() + bar.get_width() / 2,
                 bar.get_height() + max(top["count"]) * 0.015,
                 str(int(val)), ha="center", va="bottom",
                 fontsize=7, fontweight="bold", color=INK)
    ax1.set_title("Call Count (top syscalls)", fontsize=12, fontweight="bold", fontfamily=SERIF, color=INK)
    ax1.set_xticks(range(len(top)))
    ax1.set_xticklabels(top["name"], rotation=40, ha="right", fontsize=7)
    ax1.set_ylabel("Calls", fontsize=9)
    ax1.yaxis.grid(True, linestyle="-", linewidth=0.6, color=GRID, alpha=1.0, zorder=0)
    ax1.set_axisbelow(True)
    ax1.spines["top"].set_visible(False)
    ax1.spines["right"].set_visible(False)

    #  Panel 2: distribution as horizontal % bars 
    ax2     = fig.add_subplot(gs[0, 1])
    dist    = compute_distribution_data(df, top_n)
    bar_top = dist["top"]
    b_names = list(bar_top["name"])
    b_pcts  = list(bar_top["pct"])
    b_cats  = list(bar_top["category"])
    if dist["has_other"]:
        b_names.append(f"other ({dist['other_count']})")
        b_pcts.append(dist["other_pct"])
        b_cats.append("OTHER")
    # sort ascending so largest is on top
    order = sorted(range(len(b_names)), key=lambda k: b_pcts[k])
    b_names = [b_names[k] for k in order]
    b_pcts  = [b_pcts[k]  for k in order]
    b_cats  = [b_cats[k]  for k in order]
    b_colors = [category_color(c) if c != "OTHER" else "#b8b4ac" for c in b_cats]
    bbars = ax2.barh(range(len(b_names)), b_pcts, color=b_colors,
                     edgecolor=INK, linewidth=0.7, zorder=2)
    bmax = max(b_pcts) if b_pcts else 1
    for bar, pct in zip(bbars, b_pcts):
        ax2.text(bar.get_width() + bmax * 0.015,
                 bar.get_y() + bar.get_height() / 2,
                 f"{pct:.1f}%", va="center", ha="left",
                 fontsize=7, fontweight="bold", color=INK)
    ax2.set_yticks(range(len(b_names)))
    ax2.set_yticklabels(b_names, fontsize=8)
    ax2.set_xlim(0, bmax * 1.18)
    ax2.set_title("Distribution (% of total calls)", fontsize=12, fontweight="bold", fontfamily=SERIF, color=INK)
    ax2.set_xlabel("% of calls", fontsize=9)
    ax2.xaxis.grid(True, linestyle="-", linewidth=0.6, color=GRID, alpha=1.0, zorder=0)
    ax2.set_axisbelow(True)
    ax2.spines["top"].set_visible(False)
    ax2.spines["right"].set_visible(False)

    #  Panel 3: category breakdown 
    ax3 = fig.add_subplot(gs[1, 0])
    cat_totals = (
        df.groupby("category")["count"].sum()
        .reset_index().sort_values("count", ascending=True)
    )
    c_colors = [category_color(c) for c in cat_totals["category"]]
    hbars = ax3.barh(cat_totals["category"], cat_totals["count"],
                     color=c_colors, edgecolor=INK, linewidth=0.7, zorder=2)
    for bar, val in zip(hbars, cat_totals["count"]):
        ax3.text(bar.get_width() + cat_totals["count"].max() * 0.01,
                 bar.get_y() + bar.get_height() / 2,
                 f"{int(val):,}", va="center", ha="left",
                 fontsize=8, fontweight="bold", color=INK)
    ax3.set_title("Calls by Category", fontsize=12, fontweight="bold", fontfamily=SERIF, color=INK)
    ax3.set_xlabel("Total Calls", fontsize=9)
    ax3.xaxis.grid(True, linestyle="-", linewidth=0.6, color=GRID, alpha=1.0, zorder=0)
    ax3.set_axisbelow(True)
    ax3.spines["top"].set_visible(False)
    ax3.spines["right"].set_visible(False)

    #  Panel 4: slowest syscalls 
    ax4 = fig.add_subplot(gs[1, 1])
    called   = df[df["count"] > 0]
    slow_top = called.nlargest(min(top_n, 10), "avg_ms").sort_values("avg_ms")
    s_colors = [category_color(c) for c in slow_top["category"]]
    sbars = ax4.barh(slow_top["name"], slow_top["avg_ms"],
                     color=s_colors, edgecolor=INK, linewidth=0.7, zorder=2)
    for bar, val in zip(sbars, slow_top["avg_ms"]):
        ax4.text(bar.get_width() + slow_top["avg_ms"].max() * 0.01,
                 bar.get_y() + bar.get_height() / 2,
                 f"{val:.4f}", va="center", ha="left", fontsize=7, color=INK)
    ax4.set_title("Slowest Syscalls (avg ms)", fontsize=12, fontweight="bold", fontfamily=SERIF, color=INK)
    ax4.set_xlabel("Average Time (ms)", fontsize=9)
    ax4.xaxis.grid(True, linestyle="-", linewidth=0.6, color=GRID, alpha=1.0, zorder=0)
    ax4.set_axisbelow(True)
    ax4.spines["top"].set_visible(False)
    ax4.spines["right"].set_visible(False)
    ax4.text(0.99, -0.18, "WARNING: includes ptrace overhead",
             transform=ax4.transAxes, ha="right", fontsize=6,
             color=MUTED, style="italic")

    fig.savefig(out_path, dpi=150, bbox_inches="tight",
                facecolor=fig.get_facecolor())
    plt.close(fig)
    print(f"  [OK] {out_path}")


#  Automatic behavioral summary

def generate_summary(df, total_calls, unique_calls, program="", top_n=15):
    
    lines = []

    if program:
        lines.append(f"Program traced: {program}")
        lines.append("")

    # Category totals -> dominant category
    cat_totals = df.groupby("category")["count"].sum().sort_values(ascending=False)
    if len(cat_totals) > 0 and total_calls > 0:
        top_cat   = cat_totals.index[0]
        top_share = cat_totals.iloc[0] / total_calls * 100

        cat_words = {
            "FILE": "file-system related",
            "MEM":  "memory management",
            "NET":  "network related",
            "PROC": "process/thread management",
            "SIG":  "signal handling",
            "IPC":  "inter-process communication",
            "TIME": "time/clock related",
            "SYS":  "miscellaneous system",
        }
        desc = cat_words.get(top_cat.strip(), top_cat.strip())
        lines.append(f"Most activity was {desc} ({top_share:.0f}%).")

    # Specific, easy-to-explain syscall counts
    def count_of(name):
        row = df[df["name"] == name]
        return int(row["count"].iloc[0]) if len(row) > 0 else 0

    opens  = count_of("openat") + count_of("open")
    reads  = count_of("read")
    writes = count_of("write")
    dirs   = count_of("getdents64") + count_of("getdents")

    activity = []
    if opens:  activity.append(f"opened files {opens} time{'s' if opens != 1 else ''}")
    if reads:  activity.append(f"read data {reads} time{'s' if reads != 1 else ''}")
    if writes: activity.append(f"wrote data {writes} time{'s' if writes != 1 else ''}")
    if dirs:   activity.append(f"scanned directories {dirs} time{'s' if dirs != 1 else ''}")

    if activity:
        if len(activity) == 1:
            lines.append(f"The program {activity[0]}.")
        else:
            lines.append("The program " + ", ".join(activity[:-1]) +
                         f", and {activity[-1]}.")

    # Memory mapping note
    mmaps = count_of("mmap")
    if mmaps >= 3:
        lines.append(f"Memory mapping activity ({mmaps} mmap calls) was mostly "
                     "caused by loading shared libraries at startup.")

    # "other" group explanation.
    dist = compute_distribution_data(df, top_n)
    n_other = dist["other_count"]
    if dist["has_other"] and total_calls > 0:
        other_share = dist["other_pct"]
        lines.append(
            f"The remaining {n_other} syscall "
            f"type{'s' if n_other != 1 else ''} were grouped into 'other' "
            f"and accounted for {other_share:.1f}% of all observed system "
            f"calls. These are lower-frequency operations grouped together "
            f"to keep the visualization readable."
        )

    # Overall scale
    lines.append("")
    lines.append(f"In total: {total_calls:,} syscalls across "
                 f"{unique_calls} distinct types.")

    return "\n".join(lines)


# ── CLI entry point

def main():
    parser = argparse.ArgumentParser(
        description="Visualize syscall profiler JSON output.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument("json_file",
                        help="Path to profile.json from ./profiler --json=, "
                             "OR a run directory containing profile.json")
    parser.add_argument("--top", type=int, default=15, metavar="N",
                        help="Top N syscalls to show in bar charts (default: 15)")
    parser.add_argument("--out", default=None, metavar="DIR",
                        help="Output directory for PNG files (default: same as JSON)")
    parser.add_argument("--no-combined", action="store_true",
                        help="Skip generating the combined overview image")
    parser.add_argument("--program", default=None, metavar="CMD",
                        help="Override the program name shown on charts "
                             "(normally read from the JSON 'program' field)")
    args = parser.parse_args()

    setup_style()

    json_path = args.json_file
    auto_out_dir = None

    if os.path.isdir(args.json_file):
        candidate = os.path.join(args.json_file, "profile.json")
        if not os.path.isfile(candidate):
            print(f"Error:\nprofile.json not found in run directory:\n"
                  f"{args.json_file}", file=sys.stderr)
            sys.exit(1)
        json_path    = candidate
        auto_out_dir = args.json_file       # default output = the run dir
    elif not os.path.isfile(args.json_file):
        print(f"Error: file not found: {args.json_file}", file=sys.stderr)
        sys.exit(1)

    #  Output directory

    if args.out is not None:
        out_dir = args.out
        os.makedirs(out_dir, exist_ok=True)
    elif auto_out_dir is not None:
        out_dir = auto_out_dir
    else:
        out_dir = os.path.dirname(os.path.abspath(json_path))

    def out(name):
        return os.path.join(out_dir, name)

    #  Load data 
    print(f"\n  Loading: {json_path}")
    df, total_calls, unique_calls, program = load_profile(json_path)
    # CLI override takes precedence over the JSON field 
    if args.program:
        program = args.program
    print(f"  Syscalls: {total_calls:,} total, {unique_calls} unique types")
    if program:
        print(f"  Program : {program}")
    print(f"  Top N   : {args.top}")
    print(f"  Output  : {out_dir}\n")
    print("  Generating charts...")

    #  Generate individual charts 
    chart_counts(df,       args.top, out("syscall_counts.png"))
    chart_distribution(df, args.top, out("syscall_distribution.png"), program)
    chart_categories(df,             out("category_breakdown.png"))
    chart_slowest(df,      args.top, out("slowest_syscalls.png"))

    #  Combined overview 
    if not args.no_combined:
        chart_combined(df, args.top, total_calls, unique_calls,
                       out("syscall_report.png"), program)

    #  behavioral summary (console + text file) 
    summary = generate_summary(df, total_calls, unique_calls, program, args.top)
    summary_path = out("summary.txt")
    with open(summary_path, "w") as sf:
        sf.write(summary + "\n")
    print("")
    print("  ── Behavioral summary ──────────────────────────────")
    for line in summary.splitlines():
        print(f"  {line}")
    print("  ────────────────────────────────────────────────────")
    print(f"  [OK] {summary_path}")

    n_imgs = 4 + (0 if args.no_combined else 1)
    print(f"\n  Done! {n_imgs} PNG files + 1 summary saved to: {out_dir}\n")


if __name__ == "__main__":
    main()

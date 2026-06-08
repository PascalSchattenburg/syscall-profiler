"""
Syscall Profiler — Web UI backend (Phase 004)
=============================================

A tiny, local, read-only Flask app that turns the profiler's on-disk output
into a browsable Web UI. It is an additive *consumer* of existing data:

  * It reads results/runs_index.json (the Phase 001 registry) as the source
    of truth for which runs exist.
  * For a given run it reads that run's own artifacts (profile.json,
    benchmark.json, summary.txt, syscall_report.png) directly from the run
    directory, exactly like Phase 002's dynamic artifact detection.

It NEVER writes to results/, never calls or modifies the C code, and never
changes any file format. The C profiler, run_registry.c, run_artifacts.c,
and the registry/JSON formats are all untouched and remain the single owners
of producing and writing that data.

Run:
    cd webui
    pip install -r requirements.txt
    python app.py
    # then open http://127.0.0.1:5000

Optional: set SYSCALL_RESULTS_DIR to point at a results/ directory elsewhere.
"""

import json
import os

from flask import Flask, jsonify, send_file, abort, render_template

# --------------------------------------------------------------------------
# Paths
# --------------------------------------------------------------------------
# webui/ lives inside the project root; results/ sits next to it by default.
WEBUI_DIR    = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(WEBUI_DIR)
RESULTS_DIR  = os.environ.get(
    "SYSCALL_RESULTS_DIR",
    os.path.join(PROJECT_ROOT, "results"),
)
REGISTRY_PATH = os.path.join(RESULTS_DIR, "runs_index.json")

# Canonical artifact filenames (must match what the profiler/visualizer write).
ARTIFACT_PROFILE       = "profile.json"
ARTIFACT_BENCHMARK     = "benchmark.json"
ARTIFACT_SUMMARY       = "summary.txt"
ARTIFACT_VISUALIZATION = "syscall_report.png"

# PNGs the visualizer can produce — the only files the image route will serve.
ALLOWED_IMAGES = {
    "syscall_report.png",
    "syscall_counts.png",
    "syscall_distribution.png",
    "category_breakdown.png",
    "slowest_syscalls.png",
}

# Human-readable category labels for the profiler's short category codes.
CATEGORY_LABELS = {
    "FILE": "File System",
    "MEM":  "Memory",
    "NET":  "Network",
    "PROC": "Process",
    "SIG":  "Signal",
    "IPC":  "IPC",
    "TIME": "Time",
    "SYS":  "System",
}

app = Flask(__name__, template_folder="templates", static_folder="static")


# --------------------------------------------------------------------------
# Low-level helpers (read-only)
# --------------------------------------------------------------------------
def _read_json(path):
    """Parse a JSON file, or return None if missing/unreadable/invalid."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def _run_dir(entry):
    """
    Absolute path to a run directory from its registry entry.

    The registry stores `path` as e.g. "results/ls/2026-06-06_11-24-36"
    (relative to the project root). We resolve it against PROJECT_ROOT, and
    fall back to RESULTS_DIR/<program>/<timestamp> if that is not present
    (e.g. when SYSCALL_RESULTS_DIR points elsewhere).
    """
    path = entry.get("path", "")
    candidate = os.path.join(PROJECT_ROOT, path)
    if os.path.isdir(candidate):
        return candidate
    return os.path.join(RESULTS_DIR, entry.get("program", ""),
                        entry.get("timestamp", ""))


def _detect_artifacts(run_dir):
    """Dynamic artifact detection — file existence only, never cached."""
    def is_file(name):
        return os.path.isfile(os.path.join(run_dir, name))
    return {
        "profile":       is_file(ARTIFACT_PROFILE),
        "benchmark":     is_file(ARTIFACT_BENCHMARK),
        "summary":       is_file(ARTIFACT_SUMMARY),
        "visualization": is_file(ARTIFACT_VISUALIZATION),
    }


def _status(artifacts):
    """
    Artifact-availability status only (no performance judgement).

      OK         -> the run has been fully processed: profile.json AND
                    syscall_report.png are present.
      INCOMPLETE -> anything required above is missing.

    benchmark.json is optional and never affects status. To change what
    "complete" means, edit only this function.
    """
    if artifacts["profile"] and artifacts["visualization"]:
        return "OK"
    return "INCOMPLETE"


def _derive_from_profile(run_dir):
    """
    Read this run's profile.json and derive the display-only fields the UI
    shows in tables (dominant syscall + dominant category). Returns
    (top_syscall, category_label) with None where data is unavailable.
    """
    profile = _read_json(os.path.join(run_dir, ARTIFACT_PROFILE))
    if not profile:
        return None, None

    syscalls = profile.get("syscalls", []) or []
    top_syscall = None
    if syscalls:
        top = max(syscalls, key=lambda s: s.get("count", 0))
        top_syscall = top.get("name")

    # Dominant category = the category code with the largest total count.
    totals = {}
    for s in syscalls:
        code = (s.get("category") or "").strip()
        totals[code] = totals.get(code, 0) + s.get("count", 0)
    category = None
    if totals:
        code = max(totals, key=totals.get)
        category = CATEGORY_LABELS.get(code, code or None)

    return top_syscall, category


def _overhead_x(run_dir):
    """This run's benchmark overhead multiplier (float) or None if no bench."""
    bench = _read_json(os.path.join(run_dir, ARTIFACT_BENCHMARK))
    if not bench:
        return None
    val = bench.get("overhead_x")
    try:
        return float(val) if val is not None else None
    except (TypeError, ValueError):
        return None


def _enrich(entry):
    """
    Build the full run object the frontend consumes: the immutable registry
    metadata plus dynamically-derived display fields and live artifact state.
    """
    run_dir = _run_dir(entry)
    artifacts = _detect_artifacts(run_dir)
    top_syscall, category = _derive_from_profile(run_dir)
    overhead = _overhead_x(run_dir)

    return {
        # ---- immutable registry metadata (verbatim) ----
        "run_id":          entry.get("run_id"),
        "program":         entry.get("program"),
        "timestamp":       entry.get("timestamp"),
        "path":            entry.get("path"),
        "total_syscalls":  entry.get("total_syscalls"),
        "unique_syscalls": entry.get("unique_syscalls"),
        # ---- derived / dynamic (never stored) ----
        "top_syscall":     top_syscall,
        "category":        category,
        "overhead_x":      overhead,
        "artifacts":       artifacts,
        "status":          _status(artifacts),
    }


def _load_registry():
    """
    Load runs_index.json into a list of raw entries. Missing/empty/invalid
    registry -> [] (a fresh install with no runs is a normal, quiet case).
    """
    data = _read_json(REGISTRY_PATH)
    if not isinstance(data, list):
        return []
    return [e for e in data if isinstance(e, dict) and e.get("run_id")]


def _find_entry(run_id):
    """Return the raw registry entry for a run_id, or None."""
    for entry in _load_registry():
        if entry.get("run_id") == run_id:
            return entry
    return None


# --------------------------------------------------------------------------
# Page route
# --------------------------------------------------------------------------
@app.route("/")
def index():
    """Serve the single-page UI (added in WP2). Friendly message until then."""
    template = os.path.join(WEBUI_DIR, "templates", "index.html")
    if os.path.isfile(template):
        return render_template("index.html")
    return ("<h1>Syscall Profiler Web UI</h1>"
            "<p>Backend is running. The frontend will be served here.</p>"
            "<p>Try <a href='/api/runs'>/api/runs</a>.</p>")


# --------------------------------------------------------------------------
# API
# --------------------------------------------------------------------------
@app.route("/api/runs")
def api_runs():
    """All runs, newest first, each enriched with derived fields + artifacts."""
    runs = [_enrich(e) for e in _load_registry()]
    runs.sort(key=lambda r: (r.get("timestamp") or ""), reverse=True)
    return jsonify(runs)


@app.route("/api/run/<run_id>")
def api_run(run_id):
    """Single run's metadata (+ derived fields + artifacts)."""
    entry = _find_entry(run_id)
    if entry is None:
        abort(404, description="run not found")
    return jsonify(_enrich(entry))


@app.route("/api/run/<run_id>/artifacts")
def api_run_artifacts(run_id):
    """Live artifact availability for a run."""
    entry = _find_entry(run_id)
    if entry is None:
        abort(404, description="run not found")
    return jsonify(_detect_artifacts(_run_dir(entry)))


@app.route("/api/run/<run_id>/summary")
def api_run_summary(run_id):
    """Raw summary.txt content (text/plain), 404 if not generated yet."""
    entry = _find_entry(run_id)
    if entry is None:
        abort(404, description="run not found")
    path = os.path.join(_run_dir(entry), ARTIFACT_SUMMARY)
    if not os.path.isfile(path):
        abort(404, description="summary not available")
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return app.response_class(fh.read(), mimetype="text/plain")
    except OSError:
        abort(404, description="summary not readable")


@app.route("/api/run/<run_id>/benchmark")
def api_run_benchmark(run_id):
    """Parsed benchmark.json, 404 if this run has no benchmark."""
    entry = _find_entry(run_id)
    if entry is None:
        abort(404, description="run not found")
    data = _read_json(os.path.join(_run_dir(entry), ARTIFACT_BENCHMARK))
    if data is None:
        abort(404, description="benchmark not available")
    return jsonify(data)


@app.route("/api/run/<run_id>/profile")
def api_run_profile(run_id):
    """Parsed profile.json, 404 if missing."""
    entry = _find_entry(run_id)
    if entry is None:
        abort(404, description="run not found")
    data = _read_json(os.path.join(_run_dir(entry), ARTIFACT_PROFILE))
    if data is None:
        abort(404, description="profile not available")
    return jsonify(data)


@app.route("/api/run/<run_id>/image/<name>")
def api_run_image(run_id, name):
    """
    Serve one of the visualizer's PNG charts for a run. `name` is restricted
    to the known chart filenames (whitelist) to prevent path traversal.
    """
    if name not in ALLOWED_IMAGES:
        abort(404, description="unknown image")
    entry = _find_entry(run_id)
    if entry is None:
        abort(404, description="run not found")
    path = os.path.join(_run_dir(entry), name)
    if not os.path.isfile(path):
        abort(404, description="image not available")
    return send_file(path, mimetype="image/png")


@app.route("/api/programs")
def api_programs():
    """
    Convenience aggregate for the Programs page: runs grouped by program.
    Derived entirely from the registry + each program's most recent run.
    """
    runs = [_enrich(e) for e in _load_registry()]
    by_program = {}
    for r in runs:
        by_program.setdefault(r["program"], []).append(r)

    programs = []
    for name, prog_runs in by_program.items():
        prog_runs.sort(key=lambda r: (r.get("timestamp") or ""), reverse=True)
        latest = prog_runs[0]
        programs.append({
            "program":     name,
            "run_count":   len(prog_runs),
            "latest":      latest["timestamp"],
            "category":    latest.get("category"),
            "top_syscall": latest.get("top_syscall"),
            "overhead_x":  latest.get("overhead_x"),
            "total_syscalls":  latest.get("total_syscalls"),
            "unique_syscalls": latest.get("unique_syscalls"),
            "status":      latest.get("status"),
        })
    programs.sort(key=lambda p: p["run_count"], reverse=True)
    return jsonify(programs)


if __name__ == "__main__":
    # Local analysis tool: bind to localhost, no debug reloader noise.
    app.run(host="127.0.0.1", port=5000, debug=False)

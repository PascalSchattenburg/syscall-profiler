#!/usr/bin/env bash
#
# generate_demo_data.sh — create a few real profiling runs for the Web UI.
#
# Run from the PROJECT ROOT (the directory that contains this webui/ folder):
#
#     ./webui/generate_demo_data.sh
#
# It builds the profiler if needed, profiles a handful of common programs,
# attaches a benchmark to one of them, and runs the visualizer so the Web UI
# has at least one fully-processed (OK) run and some profile-only (INCOMPLETE)
# runs to show. It only writes under results/ — nothing else is touched.

set -e

# Resolve the project root as the parent of this script's directory, so the
# script works no matter where it is invoked from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

echo "==> Project root: $ROOT"

# 1. Build the profiler if the binary is missing.
if [ ! -x ./profiler ]; then
  echo "==> Building profiler..."
  make
fi

# 2. Profile a few programs (quiet mode keeps output short).
echo "==> Profiling sample programs..."
./profiler -q ls               || true
./profiler -q find /usr -maxdepth 1 || true
./profiler -q true             || true

# 3. Attach a benchmark to the most recent ls run, if one exists.
LS_RUN="$(find results/ls -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort | tail -1 || true)"
if [ -n "$LS_RUN" ]; then
  echo "==> Benchmarking $LS_RUN ..."
  ./profiler --benchmark-run "$LS_RUN" || true
fi

# 4. Visualize the ls run (creates summary.txt + syscall_report.png + charts).
#    Requires the visualizer's deps: pip install -r requirements.txt
if [ -n "$LS_RUN" ]; then
  echo "==> Visualizing $LS_RUN ..."
  python3 visualize.py "$LS_RUN" || {
    echo "    (visualize.py failed — install deps with: pip install -r requirements.txt)"
  }
fi

echo
echo "==> Done. Runs are under results/. Start the Web UI with:"
echo "      cd webui && pip install -r requirements.txt && python app.py"
echo "    then open http://127.0.0.1:5000"

# ============================================================
# Makefile for System Call Profiler & Tracer
# University OS Project
#
# Usage:
#   make          - build the profiler
#   make clean    - remove build artifacts
#   make run      - quick test with 'ls'
#   make test     - run a few quick tests
#   make help     - show this help
# ============================================================

# ---- Compiler settings ----
CC      = gcc

# Flags explained:
#   -Wall     : enable all common warnings
#   -Wextra   : enable extra warnings
#   -g        : include debug symbols (for gdb)
#   -std=gnu99  : use C99 standard (for declarations after statements etc.)
#   -I include : add include/ to the header search path
CFLAGS  = -Wall -Wextra -g -std=gnu99 -I include

# ---- Target binary ----
TARGET  = profiler

# ---- Source files ----
SRCS    = src/main.c         \
          src/tracer.c       \
          src/profiler.c     \
          src/syscall_table.c \
          src/output.c       \
          src/decoder.c      \
          src/filter.c       \
          src/benchmark.c    \
          src/run_artifacts.c

# ---- Object files (same names, in a build/ dir) ----
OBJS    = $(SRCS:.c=.o)

# ============================================================
# Default target: build the profiler
# ============================================================
all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "  [LD]  Linking $@"
	$(CC) $(CFLAGS) -o $@ $^
	@echo ""
	@echo "  Build successful! Run with:  ./profiler <program> [args]"
	@echo "  Example:                     ./profiler ls -la"
	@echo ""

# Compile each .c file to a .o object file
%.o: %.c
	@echo "  [CC]  $<"
	$(CC) $(CFLAGS) -c -o $@ $<

# ============================================================
# Convenience targets
# ============================================================

# Quick test: trace 'ls'
run: $(TARGET)
	@echo "=== Running: ./profiler ls ==="
	./$(TARGET) ls

# Quick test without color (useful for piping to a file)
run-nocolor: $(TARGET)
	./$(TARGET) -n ls

# Run and save CSV
run-csv: $(TARGET)
	./$(TARGET) -c profile.csv ls
	@echo "CSV output saved to profile.csv"
	@cat profile.csv

# Trace a more interesting program
run-find: $(TARGET)
	./$(TARGET) -q find /usr/include -name "*.h" -maxdepth 2

# Trace 'cat' on /etc/hostname
run-cat: $(TARGET)
	./$(TARGET) cat /etc/hostname

# Run all test cases
test: $(TARGET)
	@echo ""
	@echo "=== Test 1: ls ==="
	./$(TARGET) -q ls > /dev/null && echo "PASS"
	@echo ""
	@echo "=== Test 2: echo ==="
	./$(TARGET) -q echo "hello from profiler" > /dev/null && echo "PASS"
	@echo ""
	@echo "=== Test 3: true (minimal syscalls) ==="
	./$(TARGET) -q true > /dev/null && echo "PASS"
	@echo ""
	@echo "All tests passed!"

# ============================================================
# Clean up
# ============================================================
clean:
	@echo "  Cleaning build artifacts..."
	rm -f $(OBJS) $(TARGET) profile.csv
	@echo "  Done."

# ============================================================
# Help
# ============================================================
help:
	@echo ""
	@echo "  make              - Build the profiler"
	@echo "  make run          - Build and test with 'ls'"
	@echo "  make run-csv      - Build, run, export CSV"
	@echo "  make run-find     - Trace 'find' command (many syscalls)"
	@echo "  make test         - Run all tests"
	@echo "  make clean        - Remove build artifacts"
	@echo ""
	@echo "  Direct usage examples:"
	@echo "    ./profiler ls -la"
	@echo "    ./profiler -q cat /etc/os-release"
	@echo "    ./profiler -c results.csv find /usr -name '*.h' -maxdepth 1"
	@echo "    ./profiler -n -q true"
	@echo ""

# Declare phony targets (not real files)
.PHONY: all run run-nocolor run-csv run-find run-cat test clean help

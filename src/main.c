#define _POSIX_C_SOURCE 200809L
/*
 * main.c
 *
 * Entry point for the System Call Profiler & Tracer.
 *
 * USAGE:
 *   ./profiler [OPTIONS] <program> [program-args...]
 *
 * OPTIONS:
 *   -q                  Quiet mode (suppress real-time trace output)
 *   -n                  No color (plain text output)
 *   -c <file>           Export results to CSV file
 *   -h                  Show help
 *   --only=name[,name]  Show ONLY these syscalls (trace + report)
 *   --exclude=name[,…]  Hide these syscalls (trace + report)
 *   --top=N             Show only top N entries in the final report
 *   --json=<file>       Export full results to JSON file  [NEW P10]
 *   --benchmark         Run overhead analysis mode        [NEW P8]
 *
 * EXAMPLES:
 *   ./profiler ls -la
 *   ./profiler -q find /usr -name "*.h"
 *   ./profiler -c out.csv cat /etc/hostname
 *   ./profiler --only=openat,read,write ls
 *   ./profiler --exclude=mmap,mprotect,brk ls
 *   ./profiler --top=5 -q ls
 *   ./profiler --json=results.json -q ls
 *   ./profiler --benchmark ls
 *   ./profiler --benchmark find /usr -maxdepth 2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     /* access(), X_OK */
#include <time.h>       /* time(), localtime_r() — RESULTS-MGMT-001 */
#include <sys/stat.h>   /* mkdir()              — RESULTS-MGMT-001 */
#include <sys/types.h>
#include <errno.h>      /* errno for mkdir      — RESULTS-MGMT-001 */

#include "../include/tracer.h"
#include "../include/profiler.h"
#include "../include/output.h"
#include "../include/filter.h"
#include "../include/benchmark.h"

/* ===============================================================
 * RESULTS-MGMT-001 — Organized results directory helpers
 *
 * These helpers build and create the per-run output directory:
 *
 *     <results_dir>/<program>/<timestamp>/
 *
 * They only deal with path strings and directory creation. They do
 * not touch tracing, profiling, decoding, or any export format.
 * =============================================================== */

/* ---------------------------------------------------------------
 * make_timestamp()
 *
 * Write the current local time into buf as "YYYY-MM-DD_HH-MM-SS".
 * buf should be at least 20 bytes. This format is filesystem-safe
 * (no colons or spaces) and sorts chronologically as plain text.
 * --------------------------------------------------------------- */
static void make_timestamp(char *buf, size_t bufsz)
{
    time_t    now = time(NULL);
    struct tm tm_local;

    localtime_r(&now, &tm_local);
    strftime(buf, bufsz, "%Y-%m-%d_%H-%M-%S", &tm_local);
}

/* ---------------------------------------------------------------
 * basename_of()
 *
 * Derive a clean program name for use as a directory name.
 *
 *   "ls"               -> "ls"
 *   "/usr/bin/python3" -> "python3"
 *   "./profiler"       -> "profiler"
 *
 * Takes the portion after the last '/'. If that would be empty
 * (e.g. the command ends in '/'), falls back to "program".
 * Writes into out (size outsz).
 * --------------------------------------------------------------- */
static void basename_of(const char *program, char *out, size_t outsz)
{
    const char *slash = strrchr(program, '/');
    const char *name  = (slash != NULL) ? slash + 1 : program;

    if (name[0] == '\0')
        name = "program";

    strncpy(out, name, outsz - 1);
    out[outsz - 1] = '\0';
}

/* ---------------------------------------------------------------
 * mkdir_recursive()
 *
 * Create a directory and all missing parent directories, like
 * "mkdir -p". Returns 0 on success, -1 on failure.
 *
 * Works by walking the path and creating each prefix in turn.
 * An already-existing directory (EEXIST) is treated as success.
 * --------------------------------------------------------------- */
static int mkdir_recursive(const char *path)
{
    char   tmp[1024];
    size_t len;
    size_t i;

    if (path == NULL || path[0] == '\0')
        return -1;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    len = strlen(tmp);

    /* Drop any trailing slash so we don't mkdir("") at the end */
    if (len > 1 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    /* Walk the path, creating each intermediate directory */
    for (i = 1; tmp[i] != '\0'; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            tmp[i] = '/';
        }
    }

    /* Create the final directory */
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;

    return 0;
}

/* ---------------------------------------------------------------
 * build_run_dir()
 *
 * Compose the per-run directory path:
 *
 *     <results_dir>/<program>/<timestamp>
 *
 * The program name is cleaned via basename_of(). The result is
 * written into out (size outsz). Returns 0 on success, -1 if the
 * composed path would not fit in the buffer.
 *
 * Note: this only builds the string; mkdir_recursive() creates it.
 * --------------------------------------------------------------- */
static int build_run_dir(const char *results_dir,
                         const char *program,
                         const char *timestamp,
                         char *out, size_t outsz)
{
    char prog_name[256];
    int  n;

    basename_of(program, prog_name, sizeof(prog_name));

    n = snprintf(out, outsz, "%s/%s/%s",
                 results_dir, prog_name, timestamp);

    if (n < 0 || (size_t)n >= outsz)
        return -1;   /* path too long for the buffer */

    return 0;
}


/* ---------------------------------------------------------------
 * make_unique_run_dir()
 *
 * Given a desired run directory path, guarantee uniqueness so that no
 * run ever reuses or overwrites another run's directory — even when two
 * runs start within the same second and therefore share a timestamp.
 *
 * If <path> does not exist yet, it is used as-is. Otherwise we try
 * <path>_2, <path>_3, ... until we find a name that does not exist,
 * and write that into out (size outsz).
 *
 * Returns 0 on success, -1 if no free name could be formed.
 * --------------------------------------------------------------- */
static int make_unique_run_dir(const char *path, char *out, size_t outsz)
{
    struct stat st;
    int         suffix;

    /* Fast path: the plain timestamped directory is free */
    if (stat(path, &st) != 0) {
        if ((size_t)snprintf(out, outsz, "%s", path) >= outsz)
            return -1;
        return 0;
    }

    /* Collision (same-second run): append _2, _3, ... until free */
    for (suffix = 2; suffix < 10000; suffix++) {
        int n = snprintf(out, outsz, "%s_%d", path, suffix);
        if (n < 0 || (size_t)n >= outsz)
            return -1;
        if (stat(out, &st) != 0)
            return 0;   /* this candidate is free */
    }
    return -1;
}


/* ===============================================================
 * RESULTS-MGMT-002 — Link a benchmark to an existing profiling run
 *
 * These helpers support --benchmark-run <dir>, which reads the
 * "program" field from <dir>/profile.json, reconstructs the command,
 * runs the existing benchmark, and stores benchmark.json in <dir>.
 * They do not change benchmark measurement or any export format.
 * =============================================================== */

/* ---------------------------------------------------------------
 * read_program_from_json()
 *
 * Extract the value of the top-level "program" string field from a
 * profile.json file. This is a minimal, dependency-free reader: it
 * scans for the "program" key and copies the quoted string value,
 * un-escaping \" and \\ (the only escapes our writer emits).
 *
 * Returns:
 *    0  success, value written to out (NUL-terminated)
 *   -1  could not open the file
 *   -2  no "program" field found
 * --------------------------------------------------------------- */
static int read_program_from_json(const char *json_path,
                                  char *out, size_t outsz)
{
    FILE  *f;
    char  *buf;
    long   size;
    char  *key;
    char  *p;
    size_t oi = 0;

    f = fopen(json_path, "rb");
    if (f == NULL)
        return -1;

    /* Read the whole file into memory (profile.json is small) */
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return -1; }

    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) { fclose(f); return -1; }

    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf); fclose(f); return -1;
    }
    buf[size] = '\0';
    fclose(f);

    /* Find the "program" key */
    key = strstr(buf, "\"program\"");
    if (key == NULL) { free(buf); return -2; }

    /* Advance to the ':' then to the opening quote of the value */
    p = strchr(key + 9, ':');
    if (p == NULL) { free(buf); return -2; }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != '"') { free(buf); return -2; }
    p++;  /* now at first char of the value */

    /* Copy the string value, handling \" and \\ escapes */
    while (*p != '\0' && *p != '"' && oi < outsz - 1) {
        if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
            out[oi++] = p[1];
            p += 2;
        } else {
            out[oi++] = *p++;
        }
    }
    out[oi] = '\0';

    free(buf);
    return 0;
}

/* ---------------------------------------------------------------
 * parse_command_string()
 *
 * Split a command string into an argv-style array, honoring single
 * and double quotes so that arguments containing spaces are kept
 * together. Backslash escapes the next character.
 *
 *   ls                         -> ["ls"]
 *   python3 test.py            -> ["python3", "test.py"]
 *   cat "my file.txt"          -> ["cat", "my file.txt"]
 *   echo 'a b'                 -> ["echo", "a b"]
 *
 * Tokens are written into the caller-provided storage:
 *   - out_argv[] receives pointers (NULL-terminated)
 *   - store[]    receives the actual token bytes
 *
 * Returns the number of tokens, or -1 on overflow.
 * --------------------------------------------------------------- */
static int parse_command_string(const char *cmd,
                                char *out_argv[], int max_args,
                                char *store, size_t store_sz)
{
    int    argc_out = 0;
    size_t si       = 0;        /* index into store              */
    const char *p   = cmd;

    while (*p != '\0') {
        /* Skip leading whitespace between tokens */
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        if (argc_out >= max_args - 1)
            return -1;  /* leave room for the NULL terminator */

        /* Start a new token */
        out_argv[argc_out++] = &store[si];

        /* Read the token, honoring quotes and backslash escapes */
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            char quote;
            if (*p == '"' || *p == '\'') {
                quote = *p++;
                while (*p != '\0' && *p != quote) {
                    if (*p == '\\' && quote == '"' && p[1] != '\0') {
                        p++;  /* skip backslash, take next literally */
                    }
                    if (si >= store_sz - 1) return -1;
                    store[si++] = *p++;
                }
                if (*p == quote) p++;  /* skip closing quote */
            } else if (*p == '\\' && p[1] != '\0') {
                p++;  /* skip backslash */
                if (si >= store_sz - 1) return -1;
                store[si++] = *p++;
            } else {
                if (si >= store_sz - 1) return -1;
                store[si++] = *p++;
            }
        }
        if (si >= store_sz) return -1;
        store[si++] = '\0';  /* terminate this token */
    }

    out_argv[argc_out] = NULL;
    return argc_out;
}


/* ---------------------------------------------------------------
 * executable_exists()
 *
 * BUG-006 helper: check whether the target program can be found and
 * executed BEFORE we print any banner or start tracing.
 *
 * Two cases, matching how execvp() resolves names:
 *   1. Name contains '/'  → check that exact path directly.
 *   2. Bare name (e.g. "ls") → search each directory in $PATH.
 *
 * Returns 1 if an executable match is found, 0 otherwise.
 * --------------------------------------------------------------- */
static int executable_exists(const char *prog)
{
    /* Case 1: explicit path (absolute or relative) */
    if (strchr(prog, '/') != NULL) {
        return (access(prog, X_OK) == 0) ? 1 : 0;
    }

    /* Case 2: bare name — search $PATH */
    const char *path_env = getenv("PATH");
    if (path_env == NULL || path_env[0] == '\0') {
        return 0;
    }

    char  path_copy[4096];
    strncpy(path_copy, path_env, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    char *dir = strtok(path_copy, ":");
    while (dir != NULL) {
        char candidate[4096];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, prog);
        if (access(candidate, X_OK) == 0) {
            return 1;
        }
        dir = strtok(NULL, ":");
    }
    return 0;
}

/* ---------------------------------------------------------------
 * run_benchmark_for_dir()   — RESULTS-MGMT-002
 *
 * Attach a benchmark to an EXISTING profiling run directory.
 *
 *   1. verify <run_dir> exists
 *   2. verify <run_dir>/profile.json exists
 *   3. read the "program" field from profile.json
 *   4. reconstruct the command (quote-aware)
 *   5. run the EXISTING benchmark implementation
 *   6. write <run_dir>/benchmark.json  (created or replaced)
 *
 * No new timestamp directory is created; profile.json and
 * results.csv are never touched.
 *
 * Returns an exit code (EXIT_SUCCESS / EXIT_FAILURE).
 * --------------------------------------------------------------- */
static int run_benchmark_for_dir(const char *run_dir)
{
    struct stat st;
    char        json_path[1100];
    char        bench_path[1100];
    char        program[512];
    char        store[1024];
    char       *cmd_argv[128];
    int         cmd_argc;
    int         rc;
    double      bench_untraced = 0.0;
    double      bench_traced   = 0.0;

    /* 1. Run directory must exist and be a directory */
    if (stat(run_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "\n  Error: Run directory not found: %s\n\n", run_dir);
        return EXIT_FAILURE;
    }

    /* 2. profile.json must exist inside it */
    if ((size_t)snprintf(json_path, sizeof(json_path),
                         "%s/profile.json", run_dir) >= sizeof(json_path)) {
        fprintf(stderr, "\n  Error: run directory path too long.\n\n");
        return EXIT_FAILURE;
    }
    if (stat(json_path, &st) != 0) {
        fprintf(stderr, "\n  Error: profile.json not found in %s\n\n", run_dir);
        return EXIT_FAILURE;
    }

    /* 3. Read the "program" field */
    rc = read_program_from_json(json_path, program, sizeof(program));
    if (rc == -1) {
        fprintf(stderr, "\n  Error: could not read %s\n\n", json_path);
        return EXIT_FAILURE;
    }
    if (rc == -2 || program[0] == '\0') {
        fprintf(stderr, "\n  Error: Program information missing from profile.json\n\n");
        return EXIT_FAILURE;
    }

    /* 4. Reconstruct the command arguments (quote-aware) */
    cmd_argc = parse_command_string(program, cmd_argv,
                                    (int)(sizeof(cmd_argv)/sizeof(cmd_argv[0])),
                                    store, sizeof(store));
    if (cmd_argc <= 0) {
        fprintf(stderr, "\n  Error: could not reconstruct command from "
                        "program field: \"%s\"\n\n", program);
        return EXIT_FAILURE;
    }

    /* Validate the executable before printing the benchmark banner */
    if (!executable_exists(cmd_argv[0])) {
        fprintf(stderr, "\n  Error: executable not found: %s\n\n", cmd_argv[0]);
        return EXIT_FAILURE;
    }

    output_print_header();
    CPRINT(COLOR_BOLD, "  Benchmarking run: ");
    printf("%s\n", run_dir);
    CPRINT(COLOR_BOLD, "  Program: ");
    printf("%s\n", program);

    /* 5. Run the EXISTING benchmark (logic unchanged) */
    rc = benchmark_run(cmd_argc, cmd_argv, &bench_untraced, &bench_traced);
    if (rc != 0) {
        fprintf(stderr, "  benchmark_run() failed.\n");
        return EXIT_FAILURE;
    }

    /* 6. Write benchmark.json into the same run directory */
    snprintf(bench_path, sizeof(bench_path), "%s/benchmark.json", run_dir);
    output_export_benchmark_json(program, bench_untraced, bench_traced,
                                 bench_path);

    return EXIT_SUCCESS;
}

/* ---------------------------------------------------------------
 * print_usage()
 * --------------------------------------------------------------- */
static void print_usage(const char *progname)
{
    fprintf(stderr,
            "\n"
            "  Usage: %s [OPTIONS] <program> [args...]\n"
            "\n"
            "  Basic options:\n"
            "    -q              Quiet mode (no real-time trace)\n"
            "    -n              No color output\n"
            "    -c <file>       Export CSV results to <file>\n"
            "    -h              Show this help\n"
            "\n"
            "  Filter options:\n"
            "    --only=a,b,c    Show ONLY these syscalls (trace + report)\n"
            "    --exclude=a,b   Hide these syscalls (trace + report)\n"
            "    --top=N         Show only top N entries in the report\n"
            "\n"
            "  Export options:\n"
            "    --json=<file>   Export full results as JSON\n"
            "\n"
            "  Output organization:\n"
            "    --results-dir=<dir>  Base directory for run artifacts\n"
            "                         (default: results/). Each run is saved to\n"
            "                         <dir>/<program>/<timestamp>/\n"
            "\n"
            "  Analysis options:\n"
            "    --benchmark     Measure ptrace overhead vs normal execution\n"
            "    --benchmark-run <run-directory>\n"
            "                    Attach a benchmark to an existing run directory\n"
            "                    (reads program from its profile.json, writes\n"
            "                    benchmark.json into the same directory)\n"
            "\n"
            "  Examples:\n"
            "    %s ls -la\n"
            "    %s --only=openat,read,write ls\n"
            "    %s --exclude=mmap,mprotect,brk -q ls\n"
            "    %s --top=5 -q ls\n"
            "    %s --json=results.json -q ls\n"
            "    %s --results-dir=/mnt/data/profiles ls\n"
            "    %s --benchmark ls\n"
            "    %s --benchmark-run results/ls/2026-06-05_16-03-28/\n"
            "\n",
            progname,
            progname, progname, progname,
            progname, progname, progname, progname, progname);
}

/* ---------------------------------------------------------------
 * main()
 * --------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int         i;
    int         target_start   = -1;
    int         had_only       = 0;
    int         had_exclude    = 0;
    int         do_benchmark   = 0;
    int         do_json        = 0;
    const char *json_filename  = "profile.json";
    char        program_str[512] = "";  /* full target command, for JSON export */
    const char *results_dir    = "results";  /* RESULTS-MGMT-001: base dir   */
    char        run_dir[1024]   = "";         /* <results>/<prog>/<timestamp> */
    char        timestamp[32]   = "";
    char        auto_json[1100] = "";         /* run_dir/profile.json         */
    char        auto_csv[1100]  = "";         /* run_dir/results.csv          */
    syscall_stat_t *stats;
    int         stats_count;
    uint64_t    total;

    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    /*
     * RESULTS-MGMT-002: --benchmark-run <run-directory>
     *
     * This is a standalone mode: it attaches a benchmark to an existing
     * profiling run instead of tracing a new program. It reads the
     * program from <run-directory>/profile.json, runs the existing
     * benchmark, and writes benchmark.json into that same directory.
     * Handled early because it does not take a target program on the
     * command line — the program comes from profile.json.
     */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--benchmark-run") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "\n  Error: --benchmark-run requires a run "
                                "directory.\n\n");
                return EXIT_FAILURE;
            }
            return run_benchmark_for_dir(argv[i + 1]);
        }
    }

    /* ---- Argument parsing ---- */
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        /* ---- Long options ---- */
        if (strncmp(arg, "--", 2) == 0) {

            if (strncmp(arg, "--only=", 7) == 0) {
                if (had_exclude) {
                    fprintf(stderr, "\n  Error: --only and --exclude cannot be combined.\n\n");
                    return EXIT_FAILURE;
                }
                if (filter_set_only(arg + 7) != 0) {
                    /* filter_set_only already printed the specific reason */
                    return EXIT_FAILURE;
                }
                had_only = 1;

            } else if (strncmp(arg, "--exclude=", 10) == 0) {
                if (had_only) {
                    fprintf(stderr, "\n  Error: --only and --exclude cannot be combined.\n\n");
                    return EXIT_FAILURE;
                }
                if (filter_set_exclude(arg + 10) != 0) {
                    /* filter_set_exclude already printed the specific reason */
                    return EXIT_FAILURE;
                }
                had_exclude = 1;

            } else if (strncmp(arg, "--top=", 6) == 0) {
                int n = atoi(arg + 6);
                if (n <= 0) {
                    fprintf(stderr, "\n  Error: --top must be a positive number.\n\n");
                    return EXIT_FAILURE;
                }
                filter_top_n = n;

            } else if (strncmp(arg, "--json=", 7) == 0) {
                do_json       = 1;
                json_filename = arg + 7;

            } else if (strncmp(arg, "--results-dir=", 14) == 0) {
                /* RESULTS-MGMT-001: override the base results directory */
                results_dir = arg + 14;
                if (results_dir[0] == '\0') {
                    fprintf(stderr, "\n  Error: --results-dir requires a directory.\n\n");
                    return EXIT_FAILURE;
                }

            } else if (strcmp(arg, "--benchmark") == 0) {
                do_benchmark = 1;

            } else if (strcmp(arg, "--help") == 0) {
                print_usage(argv[0]);
                return EXIT_SUCCESS;

            } else {
                fprintf(stderr, "\n  Error: unknown option: %s\n\n", arg);
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }

        /* ---- Short options ---- */
        } else if (arg[0] == '-' && arg[1] != '\0') {

            int j;
            for (j = 1; arg[j] != '\0'; j++) {
                switch (arg[j]) {
                    case 'q': show_trace = 0; break;
                    case 'n': use_color  = 0; break;
                    case 'h': print_usage(argv[0]); return EXIT_SUCCESS;
                    case 'c':
                        export_csv = 1;
                        if (arg[j + 1] != '\0') {
                            csv_filename = arg + j + 1;
                        } else if (i + 1 < argc) {
                            csv_filename = argv[++i];
                        } else {
                            fprintf(stderr, "\n  Error: -c requires a filename.\n\n");
                            return EXIT_FAILURE;
                        }
                        j = (int)strlen(arg) - 1;
                        break;
                    default:
                        fprintf(stderr, "\n  Error: unknown option: -%c\n\n", arg[j]);
                        print_usage(argv[0]);
                        return EXIT_FAILURE;
                }
            }

        /* ---- First non-option: start of target program ---- */
        } else {
            target_start = i;
            break;
        }
    }

    if (target_start < 0 || target_start >= argc) {
        fprintf(stderr, "\n  Error: no target program specified.\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* ---- Benchmark mode: run overhead analysis then exit ---- */
    if (do_benchmark) {
        double bench_untraced = 0.0;
        double bench_traced   = 0.0;
        int    bench_rc;

        /* BUG-006: validate before any output here too */
        if (!executable_exists(argv[target_start])) {
            fprintf(stderr, "\n  Error: executable not found: %s\n\n",
                    argv[target_start]);
            return EXIT_FAILURE;
        }
        output_print_header();
        CPRINT(COLOR_BOLD, "  Program: ");
        for (i = target_start; i < argc; i++) {
            printf("%s%s", argv[i], (i < argc - 1) ? " " : "");
            /* Build the command string for the benchmark.json artifact */
            strncat(program_str, argv[i],
                    sizeof(program_str) - strlen(program_str) - 1);
            if (i < argc - 1)
                strncat(program_str, " ",
                        sizeof(program_str) - strlen(program_str) - 1);
        }
        printf("\n");

        /*
         * RESULTS-MGMT-001: create the per-run directory so the benchmark
         * artifact can be stored alongside other run artifacts. The
         * benchmark's own logic and console output are unchanged; we only
         * read back its already-computed numbers via output parameters.
         */
        make_timestamp(timestamp, sizeof(timestamp));
        {
            char desired[1024];
            if (build_run_dir(results_dir, argv[target_start], timestamp,
                              desired, sizeof(desired)) != 0 ||
                make_unique_run_dir(desired, run_dir, sizeof(run_dir)) != 0 ||
                mkdir_recursive(run_dir) != 0) {
                fprintf(stderr, "\n  Warning: could not create results directory "
                                "'%s'; continuing without saving benchmark.json.\n",
                        run_dir[0] ? run_dir : desired);
                run_dir[0] = '\0';
            }
        }

        bench_rc = benchmark_run(argc - target_start, &argv[target_start],
                                 &bench_untraced, &bench_traced);

        /* Persist the benchmark result as an artifact (no behavior change) */
        if (bench_rc == 0 && run_dir[0] != '\0') {
            char bench_json[1100];
            int  n = snprintf(bench_json, sizeof(bench_json),
                              "%s/benchmark.json", run_dir);
            if (n > 0 && (size_t)n < sizeof(bench_json)) {
                output_export_benchmark_json(program_str, bench_untraced,
                                             bench_traced, bench_json);
            }
        }
        return bench_rc;
    }

    /* ---- Normal tracing mode ---- */
    /*
     * BUG-006 fix: validate the target executable BEFORE printing any
     * banner or trace UI. Previously a missing program printed the full
     * interface, then failed deep inside the child with "execvp failed".
     * Now we fail cleanly and early with a clear message.
     */
    if (!executable_exists(argv[target_start])) {
        fprintf(stderr, "\n  Error: executable not found: %s\n\n",
                argv[target_start]);
        return EXIT_FAILURE;
    }

    output_print_header();

    CPRINT(COLOR_BOLD, "  Tracing: ");
    for (i = target_start; i < argc; i++) {
        printf("%s%s", argv[i], (i < argc - 1) ? " " : "");
        /* Build the full command string for the JSON export (VIS-002).
         * strncat with bounds keeps us safely inside program_str. */
        strncat(program_str, argv[i],
                sizeof(program_str) - strlen(program_str) - 1);
        if (i < argc - 1)
            strncat(program_str, " ",
                    sizeof(program_str) - strlen(program_str) - 1);
    }
    printf("\n");

    if (filter_is_active()) {
        char fdesc[256];
        filter_describe(fdesc, sizeof(fdesc));
        CPRINT(COLOR_YELLOW, "  Filter:  ");
        printf("%s\n", fdesc);
    }
    printf("\n");

    /*
     * RESULTS-MGMT-001: create this run's output directory
     *     <results_dir>/<program>/<timestamp>/
     * Every run gets a fresh timestamped directory, so runs never
     * overwrite each other. profile.json and results.csv are written
     * here automatically (see below).
     */
    make_timestamp(timestamp, sizeof(timestamp));
    {
        char desired[1024];
        if (build_run_dir(results_dir, argv[target_start], timestamp,
                          desired, sizeof(desired)) != 0 ||
            make_unique_run_dir(desired, run_dir, sizeof(run_dir)) != 0 ||
            mkdir_recursive(run_dir) != 0) {
            fprintf(stderr, "\n  Warning: could not create results directory "
                            "'%s'; falling back to current directory.\n",
                    run_dir[0] ? run_dir : desired);
            run_dir[0] = '\0';
        } else {
            CPRINT(COLOR_BOLD, "  Results: ");
            printf("%s/\n", run_dir);
        }
    }
    printf("\n");

    /* ---- Run tracer ---- */
    if (tracer_run(argc - target_start, &argv[target_start]) != 0) {
        fprintf(stderr, "tracer_run() failed.\n");
        return EXIT_FAILURE;
    }

    /* ---- Print report ---- */
    stats = profiler_get_stats(&stats_count);
    total = profiler_total_syscalls();
    output_print_report(stats, stats_count, total);

    /*
     * RESULTS-MGMT-001: automatic artifact export.
     * Every run writes profile.json and results.csv into its run
     * directory by default, so each run is a complete, reproducible
     * artifact set. This does not change the export formats.
     */
    if (run_dir[0] != '\0') {
        snprintf(auto_json, sizeof(auto_json), "%s/profile.json", run_dir);
        snprintf(auto_csv,  sizeof(auto_csv),  "%s/results.csv",  run_dir);
        output_export_json(stats, stats_count, total, program_str, auto_json);
        output_export_csv(stats, stats_count, auto_csv);
    }

    /*
     * Explicit --json / -c flags continue to work exactly as before:
     * they write to the user-specified path, in addition to the
     * automatic artifacts above.
     */
    if (do_json)
        output_export_json(stats, stats_count, total, program_str, json_filename);

    if (export_csv)
        output_export_csv(stats, stats_count, csv_filename);

    return EXIT_SUCCESS;
}

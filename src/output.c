/*
 * output.c
 *
 * Handles all formatted output for the profiler.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/output.h"
#include "../include/syscall_table.h"
#include "../include/decoder.h"
#include "../include/filter.h"

/* ---------------------------------------------------------------
 * Global flags
 * --------------------------------------------------------------- */
int use_color        = 1;
int show_trace       = 1;
int export_csv       = 0;
const char *csv_filename = "syscall_profile.csv";

#define CPRINT(color, fmt, ...) \
    do { \
        if (use_color) printf(color fmt COLOR_RESET, ##__VA_ARGS__); \
        else           printf(fmt, ##__VA_ARGS__); \
    } while (0)

/* ---------------------------------------------------------------
 * output_print_header()
 * --------------------------------------------------------------- */
void output_print_header(void)
{
    CPRINT(COLOR_BOLD COLOR_CYAN,
           "╔══════════════════════════════════════════╗\n");
    CPRINT(COLOR_BOLD COLOR_CYAN,
           "║     System Call Profiler & Tracer        ║\n");
    CPRINT(COLOR_BOLD COLOR_CYAN,
           "║     University OS Project                ║\n");
    CPRINT(COLOR_BOLD COLOR_CYAN,
           "╚══════════════════════════════════════════╝\n");
    printf("\n");
}

/* ---------------------------------------------------------------
 * output_trace_entry()
 *
 * Called at syscall ENTRY.
 * --------------------------------------------------------------- */
void output_trace_entry(pid_t child_pid, long syscall_num,
                        const struct user_regs_struct *regs)
{
    char               args[DECODER_BUF_SIZE];
    const char        *name;
    syscall_category_t cat;
    const char        *label;
    const char        *color;

    if (!show_trace) return;

    /* Filter: skip syscalls not in --only or in --exclude list */
    if (!filter_should_show(syscall_num)) return;

    name  = get_syscall_name(syscall_num);
    cat   = get_syscall_category(syscall_num);
    label = get_category_label(cat);
    color = get_category_color(cat);

    /* Decode arguments into args[] */
    decode_syscall_args(child_pid, regs, args, sizeof(args));

    /*
     * Print WITHOUT a trailing newline.
     * output_trace_exit() will append " = <retval>\n".
     * We flush stdout so the partial line appears immediately.
     */
    if (use_color) {
        printf("  %s[%s]%s %-16s(%s)",
               color, label, COLOR_RESET,
               name, args);
    } else {
        printf("  [%s] %-16s(%s)", label, name, args);
    }

    fflush(stdout);
}

/* ---------------------------------------------------------------
 * output_trace_exit()
 *
 * Called at syscall EXIT. Appends " = retval\n" to the line
 * that output_trace_entry() started.
 * --------------------------------------------------------------- */
void output_trace_exit(long syscall_num, long retval)
{
    char retval_str[64];

    if (!show_trace) return;

    /* Filter: if entry was suppressed, exit must be suppressed too */
    if (!filter_should_show(syscall_num)) return;

    decode_retval(syscall_num, retval, retval_str, sizeof(retval_str));

    if (retval < 0) {
        /* Failed syscall — show in red */
        CPRINT(COLOR_RED, " %s\n", retval_str);
    } else {
        /* Success — plain color */
        CPRINT(COLOR_GREEN, " %s\n", retval_str);
    }
}

/* ---------------------------------------------------------------
 * Sorting helpers
 * --------------------------------------------------------------- */
static int compare_by_count_desc(const void *a, const void *b)
{
    const syscall_stat_t *sa = (const syscall_stat_t *)a;
    const syscall_stat_t *sb = (const syscall_stat_t *)b;
    if (sb->call_count > sa->call_count) return  1;
    if (sb->call_count < sa->call_count) return -1;
    return 0;
}

/* ---------------------------------------------------------------
 * print_category_summary() 
 * --------------------------------------------------------------- */
static void print_category_summary(syscall_stat_t *stats, int count,
                                   uint64_t total_calls)
{
    uint64_t cat_counts[CAT_OTHER + 1];
    int i;
    int cats[] = {
        CAT_FILE, CAT_MEMORY, CAT_NETWORK,
        CAT_PROCESS, CAT_SIGNAL, CAT_IPC,
        CAT_TIME, CAT_OTHER
    };
    int num_cats = (int)(sizeof(cats) / sizeof(cats[0]));

    memset(cat_counts, 0, sizeof(cat_counts));
    for (i = 0; i < count; i++) {
        /* Respect filter — skip syscalls excluded by --only or --exclude */
        if (!filter_should_show(stats[i].number)) continue;
        if (stats[i].call_count == 0)             continue; /* BUG-002 */
        syscall_category_t cat = get_syscall_category(stats[i].number);
        cat_counts[cat] += stats[i].call_count;
    }

    printf("\n");
    CPRINT(COLOR_BOLD COLOR_YELLOW, "  SYSCALLS BY CATEGORY\n");
    CPRINT(COLOR_WHITE, "  %-8s  %8s  %6s\n", "CATEGORY", "COUNT", "% TOTAL");
    CPRINT(COLOR_WHITE, "  %-8s  %8s  %6s\n", "────────", "────────", "──────");

    for (i = 0; i < num_cats; i++) {
        int c = cats[i];
        if (cat_counts[c] == 0) continue;
        double pct        = (total_calls > 0)
                            ? 100.0 * cat_counts[c] / total_calls : 0.0;
        const char *label = get_category_label((syscall_category_t)c);
        const char *color = get_category_color((syscall_category_t)c);
        if (use_color)
            printf("  %s[%s]%s  %8llu  %5.1f%%\n",
                   color, label, COLOR_RESET,
                   (unsigned long long)cat_counts[c], pct);
        else
            printf("  [%s]  %8llu  %5.1f%%\n",
                   label, (unsigned long long)cat_counts[c], pct);
    }
}

/* ---------------------------------------------------------------
 * output_print_report()  
 * --------------------------------------------------------------- */
void output_print_report(syscall_stat_t *stats, int count, uint64_t total_calls)
{
    int i;
    syscall_stat_t *sorted;

    if (count == 0 || total_calls == 0) {
        printf("\n[PROFILE] No system calls recorded.\n");
        return;
    }

    sorted = (syscall_stat_t *)malloc(count * sizeof(syscall_stat_t));
    if (sorted == NULL) {
        fprintf(stderr, "output: malloc failed\n");
        return;
    }
    memcpy(sorted, stats, count * sizeof(syscall_stat_t));
    qsort(sorted, count, sizeof(syscall_stat_t), compare_by_count_desc);

    /*
     * when a filter is active, ALL summary statistics
     * (total syscalls, unique count, category totals, percentages)
     * must reflect the FILTERED dataset, not the full one.
     *
     * We compute the filtered totals once here and pass them down
     * to every part of the report. When no filter is active,
     * filter_should_show() returns 1 for everything, so these values
     * equal the full dataset.
     */
    uint64_t filtered_total  = 0;
    int      filtered_unique = 0;
    for (i = 0; i < count; i++) {
        if (!filter_should_show(stats[i].number)) continue;
        if (stats[i].call_count == 0)             continue; /* BUG-002 */
        filtered_total += stats[i].call_count;
        filtered_unique++;
    }

    printf("\n");
    CPRINT(COLOR_BOLD COLOR_YELLOW,
           "══════════════════════════════════════════════════════════════\n");
    CPRINT(COLOR_BOLD COLOR_YELLOW, "  SYSTEM CALL PROFILE REPORT\n");
    CPRINT(COLOR_BOLD COLOR_YELLOW,
           "══════════════════════════════════════════════════════════════\n");
    printf("\n");
    printf("  Total syscalls executed : ");
    CPRINT(COLOR_BOLD, "%llu\n", (unsigned long long)filtered_total);
    printf("  Unique syscall types    : ");
    CPRINT(COLOR_BOLD, "%d\n", filtered_unique);

    print_category_summary(stats, count, filtered_total);

    printf("\n");
    CPRINT(COLOR_BOLD COLOR_WHITE,
           "  %-6s  %-20s %8s  %12s  %12s  %6s\n",
           "CATG", "SYSCALL", "COUNT", "TOTAL(ms)", "AVG(ms)", "%CALLS");
    CPRINT(COLOR_WHITE,
           "  %-6s  %-20s %8s  %12s  %12s  %6s\n",
           "──────", "────────────────────", "────────",
           "────────────", "────────────", "──────");

    {
        int rows_shown = 0;
        for (i = 0; i < count; i++) {
            syscall_stat_t     *s   = &sorted[i];
            syscall_category_t  cat;
            const char         *lbl;
            const char         *col;
            double total_ms, avg_ms, pct;

            /* Skip syscalls excluded by --only or --exclude */
            if (!filter_should_show(s->number)) continue;

            if (s->call_count == 0) continue;

            /* Stop after top-N rows if --top=N is set */
            if (filter_top_n > 0 && rows_shown >= filter_top_n) break;

            cat = get_syscall_category(s->number);
            lbl = get_category_label(cat);
            col = get_category_color(cat);
            total_ms = s->total_time_ns / 1e6;
            avg_ms   = (s->call_count > 0)
                       ? (s->total_time_ns / s->call_count) / 1e6 : 0.0;
            /* BUG-003 fix: percentages use the filtered total */
            pct      = (filtered_total > 0)
                       ? (100.0 * s->call_count / filtered_total) : 0.0;

            if (use_color) printf("  %s[%s]%s  ", col, lbl, COLOR_RESET);
            else           printf("  [%s]  ", lbl);

            if (rows_shown < 3) CPRINT(COLOR_CYAN, "%-20s ", s->name);
            else                printf("%-20s ", s->name);

            printf("%8llu  %12.4f  %12.6f  %5.1f%%\n",
                   (unsigned long long)s->call_count, total_ms, avg_ms, pct);
            rows_shown++;
        }

        /* Note if report was truncated by --top=N */
        if (filter_top_n > 0 && rows_shown >= filter_top_n) {
            CPRINT(COLOR_YELLOW,
                   "  ... (showing top %d — use --top=N to see more)\n",
                   filter_top_n);
        }
    }

    /* Top 5 slowest */
    printf("\n");
    CPRINT(COLOR_BOLD COLOR_YELLOW,
           "  TOP 5 SLOWEST SYSCALLS (by average execution time)\n");
    CPRINT(COLOR_WHITE,
           "  %-22s  %12s\n", "──────────────────────", "────────────");
    CPRINT(COLOR_BOLD COLOR_WHITE,
           "  %-22s  %12s\n", "SYSCALL", "AVG(ms)");

    {
        syscall_stat_t *by_avg =
            (syscall_stat_t *)malloc(count * sizeof(syscall_stat_t));
        if (by_avg) {
            int j, top, n_filtered = 0;

            /* Build the filtered working set */
            for (i = 0; i < count; i++) {
                if (!filter_should_show(stats[i].number)) continue;
                if (stats[i].call_count == 0)             continue;
                by_avg[n_filtered++] = stats[i];
            }

            top = n_filtered < 5 ? n_filtered : 5;
            for (i = 0; i < top; i++) {
                int    max_idx = i;
                double max_avg = (by_avg[i].call_count > 0)
                                 ? by_avg[i].total_time_ns / by_avg[i].call_count
                                 : 0.0;
                for (j = i + 1; j < n_filtered; j++) {
                    double avg_j = (by_avg[j].call_count > 0)
                                   ? by_avg[j].total_time_ns / by_avg[j].call_count
                                   : 0.0;
                    if (avg_j > max_avg) { max_avg = avg_j; max_idx = j; }
                }
                if (max_idx != i) {
                    syscall_stat_t tmp = by_avg[i];
                    by_avg[i] = by_avg[max_idx];
                    by_avg[max_idx] = tmp;
                }
                double avg_ms_top = (by_avg[i].call_count > 0)
                    ? (by_avg[i].total_time_ns / by_avg[i].call_count) / 1e6
                    : 0.0;
                CPRINT(COLOR_RED, "  %-22s  ", by_avg[i].name);
                printf("%12.6f ms\n", avg_ms_top);
            }

            /* If the filter left nothing to show, say so clearly */
            if (n_filtered == 0) {
                CPRINT(COLOR_WHITE, "  (no syscalls match the active filter)\n");
            }

            free(by_avg);
        }
    }

    printf("\n");
    CPRINT(COLOR_BOLD COLOR_YELLOW,
           "══════════════════════════════════════════════════════════════\n");
    printf("\n");
    free(sorted);
}

/* ---------------------------------------------------------------
 * output_export_csv()  (unchanged from Step 1)
 * --------------------------------------------------------------- */
void output_export_csv(syscall_stat_t *stats, int count, const char *filename)
{
    int i;
    FILE *f = fopen(filename, "w");
    if (f == NULL) { perror("output_export_csv: fopen"); return; }

    fprintf(f, "syscall_number,syscall_name,category,"
               "call_count,total_time_ms,avg_time_ms\n");

    for (i = 0; i < count; i++) {
        syscall_stat_t    *s   = &stats[i];
        syscall_category_t cat = get_syscall_category(s->number);
        double total_ms, avg_ms;

        if (s->call_count == 0) continue;

        total_ms = s->total_time_ns / 1e6;
        avg_ms   = (s->total_time_ns / s->call_count) / 1e6;
        fprintf(f, "%ld,%s,%s,%llu,%.6f,%.6f\n",
                s->number, s->name, get_category_label(cat),
                (unsigned long long)s->call_count, total_ms, avg_ms);
    }
    fclose(f);
    printf("  [CSV] Results exported to: %s\n\n", filename);
}

/* ---------------------------------------------------------------
 * output_export_json()
 *
 * Export the full profiling results as a JSON file.
 * --------------------------------------------------------------- */
void output_export_json(syscall_stat_t *stats, int count,
                        uint64_t total_calls, const char *program,
                        const char *run_id, const char *timestamp,
                        const char *filename)
{
    int    i;
    int    valid_count = 0;     /* unique syscalls with count > 0 */
    uint64_t valid_total = 0;   /* sum of call counts (count > 0)  */
    int    written = 0;         /* array entries written so far    */
    FILE  *f = fopen(filename, "w");

    (void)total_calls;  /* recomputed below to stay consistent with BUG-002 */

    if (f == NULL) {
        perror("output_export_json: fopen");
        return;
    }

    for (i = 0; i < count; i++) {
        if (stats[i].call_count == 0) continue;
        valid_total += stats[i].call_count;
        valid_count++;
    }

    fprintf(f, "{\n");
    
    if (run_id != NULL)
        fprintf(f, "  \"run_id\": \"%s\",\n", run_id);
    if (timestamp != NULL)
        fprintf(f, "  \"timestamp\": \"%s\",\n", timestamp);
    
    fprintf(f, "  \"program\": \"");
    if (program != NULL) {
        const char *p;
        for (p = program; *p != '\0'; p++) {
            if (*p == '"' || *p == '\\') fputc('\\', f);
            fputc(*p, f);
        }
    }
    fprintf(f, "\",\n");
    fprintf(f, "  \"total_syscalls\": %llu,\n",
            (unsigned long long)valid_total);
    fprintf(f, "  \"unique_syscalls\": %d,\n", valid_count);
    fprintf(f, "  \"syscalls\": [\n");

    for (i = 0; i < count; i++) {
        syscall_stat_t    *s       = &stats[i];
        syscall_category_t cat;
        const char        *cat_lbl;
        double             total_ms, avg_ms;
        const char        *comma;

        /* BUG-002: skip syscalls that never completed (count == 0) */
        if (s->call_count == 0) continue;

        cat      = get_syscall_category(s->number);
        cat_lbl  = get_category_label(cat);
        total_ms = s->total_time_ns / 1e6;
        avg_ms   = (s->total_time_ns / s->call_count) / 1e6;

        written++;
        comma = (written < valid_count) ? "," : "";

        fprintf(f,
                "    {\n"
                "      \"number\":   %ld,\n"
                "      \"name\":     \"%s\",\n"
                "      \"category\": \"%s\",\n"
                "      \"count\":    %llu,\n"
                "      \"total_ms\": %.6f,\n"
                "      \"avg_ms\":   %.6f\n"
                "    }%s\n",
                s->number,
                s->name,
                cat_lbl,
                (unsigned long long)s->call_count,
                total_ms,
                avg_ms,
                comma);
    }

    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    fclose(f);
    printf("  [JSON] Results exported to: %s\n\n", filename);
}

/* ---------------------------------------------------------------
 * output_export_benchmark_json()
 *
 * persist a benchmark result as a JSON artifact so
 * it can live in the run directory alongside profile.json / results.csv.
 * --------------------------------------------------------------- */
void output_export_benchmark_json(const char *program,
                                  double untraced_ms, double traced_ms,
                                  const char *run_id, const char *timestamp,
                                  const char *filename)
{
    FILE  *f;
    double overhead_pct = 0.0;
    double overhead_x   = 0.0;

    if (untraced_ms > 0.0) {
        overhead_pct = (traced_ms - untraced_ms) / untraced_ms * 100.0;
        overhead_x   = traced_ms / untraced_ms;
    }

    f = fopen(filename, "w");
    if (f == NULL) {
        perror("output_export_benchmark_json: fopen");
        return;
    }

    fprintf(f, "{\n");
    
    if (run_id != NULL)
        fprintf(f, "  \"run_id\": \"%s\",\n", run_id);
    if (timestamp != NULL)
        fprintf(f, "  \"timestamp\": \"%s\",\n", timestamp);
    fprintf(f, "  \"program\": \"");
    if (program != NULL) {
        const char *p;
        for (p = program; *p != '\0'; p++) {
            if (*p == '"' || *p == '\\') fputc('\\', f);
            fputc(*p, f);
        }
    }
    fprintf(f, "\",\n");
    fprintf(f, "  \"untraced_ms\":  %.6f,\n", untraced_ms);
    fprintf(f, "  \"traced_ms\":    %.6f,\n", traced_ms);
    fprintf(f, "  \"overhead_pct\": %.2f,\n", overhead_pct);
    fprintf(f, "  \"overhead_x\":   %.2f\n",  overhead_x);
    fprintf(f, "}\n");

    fclose(f);
    printf("  [BENCHMARK] Results exported to: %s\n", filename);
}

/*
 * filter.c
 *
 * Filtering logic for the System Call Profiler.
 *
 * This module answers one question: "should this syscall be shown?"
 * Everything else (parsing CLI args, printing, tracing) stays in
 * the modules that already own those responsibilities.
 *
 * FILTER MODES:
 * -------------
 * MODE_NONE     No filter active — show everything (default).
 * MODE_ONLY     Show ONLY the listed syscalls.
 * MODE_EXCLUDE  Show everything EXCEPT the listed syscalls.
 *
 * These modes are mutually exclusive. If the user passes both
 * --only and --exclude, main.c reports an error before we get here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/filter.h"
#include "../include/syscall_table.h"

/* 
 * Internal state
 */

typedef enum {
    MODE_NONE    = 0,
    MODE_ONLY    = 1,
    MODE_EXCLUDE = 2
} filter_mode_t;

static filter_mode_t filter_mode                      = MODE_NONE;
static char          filter_names[FILTER_MAX_NAMES][32]; /* syscall name strings */
static int           filter_count                     = 0;

/* Global: top-N limit (0 = show all) — declared extern in filter.h */
int filter_top_n = 0;

/* 
 * syscall_name_is_valid()
 *
 * Returns 1 if the name is a valid syscall, 0 otherwise.
 */
static int syscall_name_is_valid(const char *name)
{
    long n;
    for (n = 0; n < MAX_SYSCALL_NUM; n++) {
        const char *known = get_syscall_name(n);
        
        if (known != NULL && strcmp(known, "unknown") != 0 &&
            strcmp(known, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 
 * parse_name_list()
 *
 * Parse a comma-separated syscall name list into filter_names[].
*/
static int parse_name_list(const char *list)
{
    char  buf[1024];
    char *token;
    int   count = 0;

    if (list == NULL || list[0] == '\0') {
        fprintf(stderr, "  Error: no valid syscall names specified.\n");
        return -1;
    }

    /* Work on a copy — strtok modifies the string */
    strncpy(buf, list, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    token = strtok(buf, ",");
    while (token != NULL) {
       
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') { *end = '\0'; end--; }

        if (strlen(token) == 0) {
            token = strtok(NULL, ",");
            continue;
        }

        if (count >= FILTER_MAX_NAMES) {
            fprintf(stderr, "filter: too many names (max %d)\n", FILTER_MAX_NAMES);
            return -1;
        }
        
        if (!syscall_name_is_valid(token)) {
            fprintf(stderr, "  Error: unknown syscall filter: %s\n", token);
            return -1;
        }

        strncpy(filter_names[count], token, 31);
        filter_names[count][31] = '\0';
        count++;

        token = strtok(NULL, ",");
    }

    if (count == 0) {
        fprintf(stderr, "  Error: no valid syscall names specified.\n");
        return -1;
    }

    return count;
}

int filter_set_only(const char *list)
{
    int n = parse_name_list(list);
    if (n < 0) return -1;
    filter_mode  = MODE_ONLY;
    filter_count = n;
    return 0;
}

int filter_set_exclude(const char *list)
{
    int n = parse_name_list(list);
    if (n < 0) return -1;
    filter_mode  = MODE_EXCLUDE;
    filter_count = n;
    return 0;
}

/*
 * filter_should_show()
 *
 * The single yes/no decision for a given syscall.
 *
 * Logic:
 *   MODE_NONE    → always show
 *   MODE_ONLY    → show if name is IN the list
 *   MODE_EXCLUDE → show if name is NOT IN the list
 */
int filter_should_show(long syscall_num)
{
    const char *name;
    int         i;
    int         in_list;

    if (filter_mode == MODE_NONE) return 1;

    name = get_syscall_name(syscall_num);

    /* Linear search — list is small max 64 entries.*/
    in_list = 0;
    for (i = 0; i < filter_count; i++) {
        if (strcmp(filter_names[i], name) == 0) {
            in_list = 1;
            break;
        }
    }

    if (filter_mode == MODE_ONLY)    return  in_list;
    if (filter_mode == MODE_EXCLUDE) return !in_list;

    return 1;
}

int filter_is_active(void)
{
    return (filter_mode != MODE_NONE || filter_top_n > 0);
}

void filter_describe(char *buf, int bufsz)
{
    int  i;
    char tmp[512] = "";

    if (filter_mode == MODE_ONLY)    strncat(tmp, "only: ",    sizeof(tmp) - strlen(tmp) - 1);
    if (filter_mode == MODE_EXCLUDE) strncat(tmp, "exclude: ", sizeof(tmp) - strlen(tmp) - 1);

    for (i = 0; i < filter_count; i++) {
        strncat(tmp, filter_names[i], sizeof(tmp) - strlen(tmp) - 1);
        if (i < filter_count - 1)
            strncat(tmp, ", ", sizeof(tmp) - strlen(tmp) - 1);
    }

    if (filter_top_n > 0) {
        char top_str[32];
        if (filter_mode != MODE_NONE)
            strncat(tmp, " | ", sizeof(tmp) - strlen(tmp) - 1);
        snprintf(top_str, sizeof(top_str), "top %d", filter_top_n);
        strncat(tmp, top_str, sizeof(tmp) - strlen(tmp) - 1);
    }

    snprintf(buf, bufsz, "%s", tmp);
}

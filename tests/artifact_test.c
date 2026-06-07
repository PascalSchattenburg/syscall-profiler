/* tests/artifact_test.c  —  manual verification only (NOT part of the build)
 *
 * Prints the three dynamic artifact states for a supplied run directory.
 *
 * Build:  gcc -Wall -Wextra -std=gnu99 -I include \
 *             tests/artifact_test.c src/run_artifacts.c -o artifact_test
 * Run:    ./artifact_test results/ls/2026-06-06_11-24-36
 */
#include <stdio.h>
#include "run_artifacts.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <run_dir>\n", argv[0]);
        return 1;
    }

    RunArtifacts a = run_detect_artifacts(argv[1]);

    printf("has_profile       = %d\n", a.has_profile);
    printf("has_benchmark     = %d\n", a.has_benchmark);
    printf("has_visualization = %d\n", a.has_visualization);
    return 0;
}

#include <stdio.h>

#include "run_registry.h"

int main(void)
{
    RunRegistry registry = registry_load("results");

    printf("runs = %zu\n", registry.count);

    RunInfo *ls_run =
        registry_find_by_program(&registry, "ls");

    if (ls_run != NULL)
    {
        printf("run_id    = %s\n", ls_run->run_id);
        printf("program   = %s\n", ls_run->program);
        printf("timestamp = %s\n", ls_run->timestamp);

        RunArtifacts a =
            registry_get_artifacts(ls_run);

        printf("profile   = %d\n", a.has_profile);
        printf("benchmark = %d\n", a.has_benchmark);
        printf("visual    = %d\n", a.has_visualization);
    }

    registry_free(&registry);

    return 0;
}

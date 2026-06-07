#include <stdio.h>

#include "run_registry.h"

int main(void)
{
    RunRegistry registry = registry_load("results");

    printf("runs = %zu\n", registry.count);

    RunInfo *run =
        registry_find_by_id(
            &registry,
            "run_e15969ab");

    if (run != NULL)
    {
        printf("run_id    = %s\n", run->run_id);
        printf("program   = %s\n", run->program);
        printf("timestamp = %s\n", run->timestamp);
        printf("path      = %s\n", run->path);

        RunArtifacts artifacts =
            registry_get_artifacts(run);

        printf("profile   = %d\n",
               artifacts.has_profile);

        printf("benchmark = %d\n",
               artifacts.has_benchmark);

        printf("visual    = %d\n",
               artifacts.has_visualization);
    }
    else
    {
        printf("Run not found.\n");
    }

    registry_free(&registry);

    return 0;
}

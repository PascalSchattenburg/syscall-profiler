#ifndef RUN_REGISTRY_H
#define RUN_REGISTRY_H

#include <stddef.h>      
#include "run_artifacts.h"  

/*
 * One run's metadata, mirroring exactly the fields stored per entry in
 * runs_index.json.
 */
typedef struct {
    char run_id[32];                          
    char program[256];     
    char timestamp[64];              
    char path[512];        

    long total_syscalls;   
    long unique_syscalls;  
} RunInfo;

/*
 * The loaded registry
 */
typedef struct {
    RunInfo *runs;
    size_t   count;
} RunRegistry;
/*
 * Load the run registry from <results_dir>/runs_index.json.
 */
RunRegistry registry_load(const char *results_dir);

/*
 * Find the single run with the given run_id.
 */
RunInfo *registry_find_by_id(RunRegistry *registry, const char *run_id);

/*
 * Find the first run whose `program` field matches `program`.
 */
RunInfo *registry_find_by_program(RunRegistry *registry, const char *program);

void registry_free(RunRegistry *registry);


RunArtifacts registry_get_artifacts(const RunInfo *run);

#endif 

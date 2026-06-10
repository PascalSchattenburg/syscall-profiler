#ifndef RUN_ARTIFACTS_H
#define RUN_ARTIFACTS_H

/*
 * Aggregate artifact state for a single run directory.
 */
typedef struct {
    int has_profile;        
    int has_benchmark;      
    int has_visualization;  
} RunArtifacts;

RunArtifacts run_detect_artifacts(const char *run_dir);

int run_has_profile(const char *run_dir);
int run_has_benchmark(const char *run_dir);
int run_has_visualization(const char *run_dir);

#endif

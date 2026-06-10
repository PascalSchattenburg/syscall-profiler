#ifndef RUN_ARTIFACTS_H
#define RUN_ARTIFACTS_H

/*
 * Aggregate artifact state for a single run directory.
 *
 * Each field is a boolean (0 / 1). The aggregate exists because a caller
 * (especially a future WebUI/API) usually wants all three states at once,
 * and computing them together lets run_detect_artifacts() resolve a run
 * in a single, self-contained pass instead of three independent ones.
 */
typedef struct {
    int has_profile;        
    int has_benchmark;      
    int has_visualization;  
} RunArtifacts;

RunArtifacts run_detect_artifacts(const char *run_dir);

/*
 * Convenience single-artifact queries.
 *
 * Each returns 1 if the artifact exists, else 0. They delegate to
 * run_detect_artifacts() so the existence rules live in exactly one
 * place. Prefer run_detect_artifacts() when you need more than one
 * answer, to avoid repeating the directory scan.
 */
int run_has_profile(const char *run_dir);
int run_has_benchmark(const char *run_dir);
int run_has_visualization(const char *run_dir);

#endif /* RUN_ARTIFACTS_H */

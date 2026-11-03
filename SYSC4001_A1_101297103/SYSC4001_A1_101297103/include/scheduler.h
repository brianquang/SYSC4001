#ifndef SYSC4001_SCHEDULER_H
#define SYSC4001_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>

#define MAX_PROCESSES 256

typedef enum {
    ALG_FCFS,
    ALG_RR,
    ALG_SRTF
} Algorithm;

typedef enum {
    STATE_NEW,
    STATE_READY,
    STATE_RUNNING,
    STATE_WAITING,
    STATE_TERMINATED
} ProcessState;

typedef enum {
    REASON_ARRIVAL,
    REASON_DISPATCH,
    REASON_CPU_BURST_COMPLETE,
    REASON_IO_COMPLETE,
    REASON_QUANTUM_EXPIRED,
    REASON_SRTF_PREEMPTION,
    REASON_PROCESS_COMPLETE
} TransitionReason;

typedef enum {
    ACTIVITY_CPU,
    ACTIVITY_CONTEXT_SWITCH,
    ACTIVITY_IDLE
} TimelineActivity;

typedef struct {
    int pid;
    int arrival_time;
    int cpu_burst;
    int io_burst;
    int num_cpu_bursts;
} ProcessSpec;

typedef struct {
    Algorithm algorithm;
    int quantum;
    int context_switch;
} SimulationConfig;

typedef struct OutputContext OutputContext;

bool parse_algorithm(const char *text, Algorithm *algorithm);
const char *algorithm_name(Algorithm algorithm);

/*
 * Supplied input helper. main.c calls this before run_simulation(), so student
 * scheduling code receives a validated array sorted by ascending PID.
 */
bool load_process_specs(const char *path, ProcessSpec **specs, size_t *count);

/* Supplied output-file management. main.c opens and closes the context. */
OutputContext *outputs_open(const char *directory);
void outputs_close(OutputContext *output);

/*
 * Supplied transition writer. Update your PCB/queues first, then call this
 * once for the state change at the timestamp when it becomes effective.
 */
void log_transition(OutputContext *output,
                    int time,
                    int pid,
                    ProcessState old_state,
                    ProcessState new_state,
                    TransitionReason reason);

/*
 * Supplied timeline writer. Call exactly once for every simulated interval
 * [start_time, start_time + 1). Adjacent identical ticks are merged for you.
 * Use pid 0 for ACTIVITY_CONTEXT_SWITCH and ACTIVITY_IDLE.
 */
void record_timeline_tick(OutputContext *output,
                          int start_time,
                          TimelineActivity activity,
                          int pid);

/*
 * Supplied metrics writer. Call once after every process has terminated.
 * It derives response/completion observations from log_transition(), derives
 * workload timing from record_timeline_tick(), and writes both metrics.csv and
 * summary.csv. Students do not calculate or format individual metric rows.
 */
bool write_metrics(OutputContext *output,
                   const ProcessSpec *specs,
                   size_t count);

/*
 * Implement this function in src/student_scheduler.c. You may add source files,
 * headers, structures, and helper functions. Do not change the function
 * signature or the supplied CSV logging functions.
 *
 * specs  - read-only array of count validated process descriptions, sorted by
 *          ascending PID; copy them into your PCBs rather than modifying them.
 * count  - number of entries in specs.
 * config - read-only algorithm, RR quantum, and context-switch settings; the
 *          quantum affects scheduling only for ALG_RR.
 * output - open context to pass to the supplied logging and metrics functions;
 *          main.c owns and closes it.
 *
 * Return true only after the simulation and metrics have completed
 * successfully; return false if the simulation or output generation fails.
 */
bool run_simulation(const ProcessSpec *specs,
                    size_t count,
                    const SimulationConfig *config,
                    OutputContext *output);

#endif

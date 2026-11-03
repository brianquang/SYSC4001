#define _POSIX_C_SOURCE 200809L

#include "scheduler.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>

typedef struct {
    int pid;
    int first_run_time;
    int completion_time;
} MetricObservation;

struct OutputContext {
    FILE *transitions;
    FILE *timeline;
    FILE *metrics;
    FILE *summary;
    bool interval_open;
    int interval_start;
    int interval_end;
    TimelineActivity interval_activity;
    int interval_pid;
    MetricObservation observations[MAX_PROCESSES];
    size_t observation_count;
    int makespan;
    int cpu_busy_time;
    int context_switch_time;
    int idle_time;
    bool metrics_written;
};

static char *trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text)) {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }

    return text;
}

static bool parse_integer(const char *text, int *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    while (end != NULL && isspace((unsigned char)*end)) {
        end++;
    }

    if (errno != 0 || end == text || end == NULL || *end != '\0' ||
        parsed < 0 || parsed > 2147483647L) {
        return false;
    }

    *value = (int)parsed;
    return true;
}

static bool parse_process_line(char *line, ProcessSpec *spec)
{
    char *fields[5];
    char *cursor = line;
    size_t i;

    /* Split exactly five fields without silently skipping empty CSV cells. */
    for (i = 0; i < 5; i++) {
        char *comma = strchr(cursor, ',');
        if ((i < 4 && comma == NULL) || (i == 4 && comma != NULL)) {
            return false;
        }
        if (comma != NULL) *comma = '\0';
        fields[i] = trim(cursor);
        if (*fields[i] == '\0') return false;
        if (comma != NULL) cursor = comma + 1;
    }

    return parse_integer(fields[0], &spec->pid) &&
           parse_integer(fields[1], &spec->arrival_time) &&
           parse_integer(fields[2], &spec->cpu_burst) &&
           parse_integer(fields[3], &spec->io_burst) &&
           parse_integer(fields[4], &spec->num_cpu_bursts);
}

static int compare_specs_by_pid(const void *left, const void *right)
{
    const ProcessSpec *a = left;
    const ProcessSpec *b = right;

    if (a->pid < b->pid) {
        return -1;
    }
    if (a->pid > b->pid) {
        return 1;
    }
    return 0;
}

bool load_process_specs(const char *path, ProcessSpec **specs, size_t *count)
{
    FILE *file = fopen(path, "r");
    ProcessSpec *items = NULL;
    size_t used = 0;
    size_t capacity = 0;
    char line[1024];
    bool header_seen = false;
    size_t line_number = 0;

    if (file == NULL) {
        fprintf(stderr, "Could not open input file '%s'.\n", path);
        return false;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *content;
        ProcessSpec spec;
        size_t i;

        line_number++;
        content = trim(line);
        if (*content == '\0' || *content == '#') {
            continue;
        }

        if (!header_seen) {
            const char *expected =
                "pid,arrival_time,cpu_burst,io_burst,num_cpu_bursts";
            if (strcmp(content, expected) != 0) {
                fprintf(stderr,
                        "Input line %zu must be the exact CSV header:\n%s\n",
                        line_number,
                        expected);
                fclose(file);
                free(items);
                return false;
            }
            header_seen = true;
            continue;
        }

        if (!parse_process_line(content, &spec)) {
            fprintf(stderr, "Invalid process record on input line %zu.\n", line_number);
            fclose(file);
            free(items);
            return false;
        }

        if (spec.pid <= 0 || spec.cpu_burst <= 0 || spec.num_cpu_bursts <= 0 ||
            (spec.num_cpu_bursts > 1 && spec.io_burst <= 0)) {
            fprintf(stderr, "Out-of-range process value on input line %zu.\n", line_number);
            fclose(file);
            free(items);
            return false;
        }

        for (i = 0; i < used; i++) {
            if (items[i].pid == spec.pid) {
                fprintf(stderr, "Duplicate PID %d on input line %zu.\n", spec.pid, line_number);
                fclose(file);
                free(items);
                return false;
            }
        }

        if (used == MAX_PROCESSES) {
            fprintf(stderr, "Input contains more than %d processes.\n", MAX_PROCESSES);
            fclose(file);
            free(items);
            return false;
        }

        if (used == capacity) {
            size_t new_capacity = capacity == 0 ? 8 : capacity * 2;
            ProcessSpec *resized = realloc(items, new_capacity * sizeof(*items));
            if (resized == NULL) {
                fprintf(stderr, "Out of memory while reading input.\n");
                fclose(file);
                free(items);
                return false;
            }
            items = resized;
            capacity = new_capacity;
        }

        items[used++] = spec;
    }

    if (ferror(file)) {
        fprintf(stderr, "Error while reading input '%s'.\n", path);
        fclose(file);
        free(items);
        return false;
    }
    fclose(file);

    if (!header_seen || used == 0) {
        fprintf(stderr, "Input must contain a header and at least one process.\n");
        free(items);
        return false;
    }

    qsort(items, used, sizeof(*items), compare_specs_by_pid);
    *specs = items;
    *count = used;
    return true;
}

bool parse_algorithm(const char *text, Algorithm *algorithm)
{
    if (strcmp(text, "fcfs") == 0) {
        *algorithm = ALG_FCFS;
    } else if (strcmp(text, "rr") == 0) {
        *algorithm = ALG_RR;
    } else if (strcmp(text, "srtf") == 0) {
        *algorithm = ALG_SRTF;
    } else {
        return false;
    }
    return true;
}

const char *algorithm_name(Algorithm algorithm)
{
    switch (algorithm) {
        case ALG_FCFS: return "FCFS";
        case ALG_RR: return "RR";
        case ALG_SRTF: return "SRTF";
    }
    return "UNKNOWN";
}

static const char *state_name(ProcessState state)
{
    switch (state) {
        case STATE_NEW: return "NEW";
        case STATE_READY: return "READY";
        case STATE_RUNNING: return "RUNNING";
        case STATE_WAITING: return "WAITING";
        case STATE_TERMINATED: return "TERMINATED";
    }
    return "UNKNOWN";
}

static const char *reason_name(TransitionReason reason)
{
    switch (reason) {
        case REASON_ARRIVAL: return "ARRIVAL";
        case REASON_DISPATCH: return "DISPATCH";
        case REASON_CPU_BURST_COMPLETE: return "CPU_BURST_COMPLETE";
        case REASON_IO_COMPLETE: return "IO_COMPLETE";
        case REASON_QUANTUM_EXPIRED: return "QUANTUM_EXPIRED";
        case REASON_SRTF_PREEMPTION: return "SRTF_PREEMPTION";
        case REASON_PROCESS_COMPLETE: return "PROCESS_COMPLETE";
    }
    return "UNKNOWN";
}

static const char *activity_name(TimelineActivity activity)
{
    switch (activity) {
        case ACTIVITY_CPU: return "CPU";
        case ACTIVITY_CONTEXT_SWITCH: return "CONTEXT_SWITCH";
        case ACTIVITY_IDLE: return "IDLE";
    }
    return "UNKNOWN";
}

static bool ensure_directory(const char *path)
{
    struct stat info;

    if (stat(path, &info) == 0) {
        if (S_ISDIR(info.st_mode)) {
            return true;
        }
        fprintf(stderr, "Output path exists but is not a directory: %s\n", path);
        return false;
    }

#ifdef _WIN32
    if (_mkdir(path) != 0) {
#else
    if (mkdir(path, 0777) != 0) {
#endif
        fprintf(stderr, "Could not create output directory '%s'.\n", path);
        return false;
    }
    return true;
}

static FILE *open_output_file(const char *directory, const char *filename)
{
    char path[2048];

    if (snprintf(path, sizeof(path), "%s/%s", directory, filename) >= (int)sizeof(path)) {
        fprintf(stderr, "Output path is too long.\n");
        return NULL;
    }

    return fopen(path, "w");
}

OutputContext *outputs_open(const char *directory)
{
    OutputContext *output;

    if (!ensure_directory(directory)) {
        return NULL;
    }

    output = calloc(1, sizeof(*output));
    if (output == NULL) {
        return NULL;
    }

    output->transitions = open_output_file(directory, "transitions.csv");
    output->timeline = open_output_file(directory, "timeline.csv");
    output->metrics = open_output_file(directory, "metrics.csv");
    output->summary = open_output_file(directory, "summary.csv");

    if (output->transitions == NULL || output->timeline == NULL ||
        output->metrics == NULL || output->summary == NULL) {
        fprintf(stderr, "Could not create one or more CSV output files.\n");
        outputs_close(output);
        return NULL;
    }

    fprintf(output->transitions, "time,pid,old_state,new_state,reason\n");
    fprintf(output->timeline, "start,end,activity,pid\n");
    fprintf(output->metrics,
            "pid,arrival_time,completion_time,turnaround_time,waiting_time,"
            "response_time\n");
    fprintf(output->summary,
            "process_count,makespan,cpu_busy_time,context_switch_time,idle_time,"
            "cpu_utilization,throughput,average_turnaround,average_waiting,"
            "average_response\n");

    return output;
}

static void flush_timeline_interval(OutputContext *output)
{
    if (output == NULL || !output->interval_open || output->timeline == NULL) {
        return;
    }

    fprintf(output->timeline,
            "%d,%d,%s,",
            output->interval_start,
            output->interval_end,
            activity_name(output->interval_activity));
    if (output->interval_activity == ACTIVITY_CPU) {
        fprintf(output->timeline, "%d", output->interval_pid);
    }
    fputc('\n', output->timeline);
    output->interval_open = false;
}

void outputs_close(OutputContext *output)
{
    if (output == NULL) {
        return;
    }

    flush_timeline_interval(output);
    if (output->transitions != NULL) fclose(output->transitions);
    if (output->timeline != NULL) fclose(output->timeline);
    if (output->metrics != NULL) fclose(output->metrics);
    if (output->summary != NULL) fclose(output->summary);
    free(output);
}

static MetricObservation *find_observation(OutputContext *output,
                                           int pid,
                                           bool create)
{
    size_t i;

    for (i = 0; i < output->observation_count; i++) {
        if (output->observations[i].pid == pid) {
            return &output->observations[i];
        }
    }

    if (!create || output->observation_count == MAX_PROCESSES) {
        return NULL;
    }

    output->observations[output->observation_count].pid = pid;
    output->observations[output->observation_count].first_run_time = -1;
    output->observations[output->observation_count].completion_time = -1;
    return &output->observations[output->observation_count++];
}

void log_transition(OutputContext *output,
                    int time,
                    int pid,
                    ProcessState old_state,
                    ProcessState new_state,
                    TransitionReason reason)
{
    MetricObservation *observation;

    fprintf(output->transitions,
            "%d,%d,%s,%s,%s\n",
            time,
            pid,
            state_name(old_state),
            state_name(new_state),
            reason_name(reason));

    observation = find_observation(output, pid, true);
    if (observation == NULL) {
        return;
    }
    if (reason == REASON_DISPATCH && observation->first_run_time < 0) {
        observation->first_run_time = time;
    }
    if (reason == REASON_PROCESS_COMPLETE) {
        observation->completion_time = time;
    }
}

void record_timeline_tick(OutputContext *output,
                          int start_time,
                          TimelineActivity activity,
                          int pid)
{
    switch (activity) {
        case ACTIVITY_CPU:
            output->cpu_busy_time++;
            break;
        case ACTIVITY_CONTEXT_SWITCH:
            output->context_switch_time++;
            break;
        case ACTIVITY_IDLE:
            output->idle_time++;
            break;
    }
    if (start_time + 1 > output->makespan) {
        output->makespan = start_time + 1;
    }

    if (output->interval_open && output->interval_end == start_time &&
        output->interval_activity == activity && output->interval_pid == pid) {
        output->interval_end++;
        return;
    }

    flush_timeline_interval(output);
    output->interval_open = true;
    output->interval_start = start_time;
    output->interval_end = start_time + 1;
    output->interval_activity = activity;
    output->interval_pid = pid;
}

bool write_metrics(OutputContext *output,
                   const ProcessSpec *specs,
                   size_t count)
{
    double average_turnaround = 0.0;
    double average_waiting = 0.0;
    double average_response = 0.0;
    int final_completion = 0;
    size_t i;

    if (output == NULL || specs == NULL || count == 0 ||
        count > MAX_PROCESSES || output->metrics_written) {
        return false;
    }

    for (i = 0; i < count; i++) {
        MetricObservation *observation =
            find_observation(output, specs[i].pid, false);
        int turnaround_time;
        int waiting_time;
        int response_time;
        long long cpu_time;
        long long io_time;

        if (observation == NULL || observation->first_run_time < 0 ||
            observation->completion_time < 0) {
            fprintf(stderr,
                    "Cannot calculate metrics for PID %d: missing DISPATCH or "
                    "PROCESS_COMPLETE transition.\n",
                    specs[i].pid);
            return false;
        }

        turnaround_time = observation->completion_time - specs[i].arrival_time;
        response_time = observation->first_run_time - specs[i].arrival_time;
        cpu_time = (long long)specs[i].cpu_burst * specs[i].num_cpu_bursts;
        io_time = (long long)specs[i].io_burst * (specs[i].num_cpu_bursts - 1);
        if (cpu_time + io_time > turnaround_time) {
            fprintf(stderr, "Cannot calculate metrics: service exceeds turnaround for PID %d.\n",
                    specs[i].pid);
            return false;
        }
        waiting_time = turnaround_time - cpu_time - io_time;

        if (turnaround_time < 0 || response_time < 0 || waiting_time < 0) {
            fprintf(stderr,
                    "Cannot calculate metrics for PID %d: inconsistent "
                    "transition timestamps.\n",
                    specs[i].pid);
            return false;
        }

        fprintf(output->metrics,
                "%d,%d,%d,%d,%d,%d\n",
                specs[i].pid,
                specs[i].arrival_time,
                observation->completion_time,
                turnaround_time,
                waiting_time,
                response_time);

        if (observation->completion_time > final_completion) {
            final_completion = observation->completion_time;
        }
        average_turnaround += turnaround_time;
        average_waiting += waiting_time;
        average_response += response_time;
    }

    if (final_completion != output->makespan ||
        output->cpu_busy_time + output->context_switch_time + output->idle_time !=
            output->makespan) {
        fprintf(stderr,
                "Cannot calculate summary metrics: timeline does not match "
                "process completion time.\n");
        return false;
    }

    average_turnaround /= count;
    average_waiting /= count;
    average_response /= count;

    fprintf(output->summary,
            "%d,%d,%d,%d,%d,%.2f,%.4f,%.2f,%.2f,%.2f\n",
            (int)count,
            output->makespan,
            output->cpu_busy_time,
            output->context_switch_time,
            output->idle_time,
            100.0 * output->cpu_busy_time / output->makespan,
            (double)count / output->makespan,
            average_turnaround,
            average_waiting,
            average_response);

    /* Detect buffered write failures before main reports success. */
    flush_timeline_interval(output);
    FILE *files[] = {output->transitions, output->timeline,
                     output->metrics, output->summary};
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (fflush(files[i]) != 0 || ferror(files[i])) {
            fprintf(stderr, "Could not write CSV output.\n");
            return false;
        }
    }
    output->metrics_written = true;
    return true;
}

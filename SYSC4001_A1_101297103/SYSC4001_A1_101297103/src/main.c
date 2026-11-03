#include "scheduler.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s -i INPUT.csv -a fcfs|rr|srtf -q QUANTUM "
            "-c CONTEXT_SWITCH -o OUTPUT_DIR\n",
            program);
}

static bool parse_nonnegative_int(const char *text, int *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
        return false;
    }

    *value = (int)parsed;
    return true;
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *output_directory = NULL;
    Algorithm algorithm = ALG_FCFS;
    bool algorithm_set = false;
    int quantum = 0;
    int context_switch = 0;
    bool quantum_set = false;
    bool context_switch_set = false;
    ProcessSpec *specs = NULL;
    size_t count = 0;
    OutputContext *output = NULL;
    SimulationConfig config;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            input_path = argv[++i];
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            algorithm_set = parse_algorithm(argv[++i], &algorithm);
            if (!algorithm_set) {
                fprintf(stderr, "Unknown scheduling algorithm: %s\n", argv[i]);
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "-q") == 0 && i + 1 < argc) {
            if (!parse_nonnegative_int(argv[++i], &quantum) || quantum == 0) {
                fprintf(stderr, "Quantum must be a positive integer.\n");
                return EXIT_FAILURE;
            }
            quantum_set = true;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            if (!parse_nonnegative_int(argv[++i], &context_switch)) {
                fprintf(stderr, "Context-switch duration must be a nonnegative integer.\n");
                return EXIT_FAILURE;
            }
            context_switch_set = true;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_directory = argv[++i];
        } else {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (input_path == NULL || output_directory == NULL || !algorithm_set ||
        !quantum_set || !context_switch_set) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!load_process_specs(input_path, &specs, &count)) {
        return EXIT_FAILURE;
    }

    output = outputs_open(output_directory);
    if (output == NULL) {
        free(specs);
        return EXIT_FAILURE;
    }

    config.algorithm = algorithm;
    config.quantum = quantum;
    config.context_switch = context_switch;

    if (!run_simulation(specs, count, &config, output)) {
        outputs_close(output);
        free(specs);
        return EXIT_FAILURE;
    }

    outputs_close(output);
    free(specs);

    printf("Simulation complete: %s\n", algorithm_name(algorithm));
    return EXIT_SUCCESS;
}

#include "scheduler.h"
#include <stdlib.h>
#include <stdio.h>

/*
 * PCB (Process Control Block): stores the input specification for one process
 * plus the information the simulator needs while that process runs.
 */
typedef struct {
    ProcessSpec spec;
    ProcessState state;                  
    int remaining_cpu;                    
    int completed_cpu_bursts;             
    int io_completion_time;              
    int quantum_used;                    
    int ready_arrival_order;              
} PCB;

/* Simple array-backed queue of pointers to PCBs that are READY to run. */
typedef struct {
    PCB *items[MAX_PROCESSES];
    int count;
} ReadyQueue;

/* Keeps track of the simulation: its processes, ready queue, current time,
*  and any process waiting for a context switch to finish. 
*/
typedef struct {
    size_t process_count;
    PCB *running;
    ReadyQueue ready_queue;
    int time;
    int context_remaining;
    int next_ready_order;
} Simulation;

/* Add a process pointer to the end of the ready queue; */
static bool enqueue(ReadyQueue *q, PCB *pcb)
{
    /*Queue is full, return false*/
    if (q->count >= MAX_PROCESSES) {
        return false;
    }

    /* Store at the current end, then increase the number of queued processes*/
    q->items[q->count++] = pcb;
    return true;
}

/* Remove and return the process pointer at the front of the ready queue; */
static PCB *dequeue(ReadyQueue *q)
{
    if (q->count == 0) {
        return NULL;
    }

    PCB *pcb = q->items[0];                  // Store the first process pointer to return it later

    // Shift all remaining process pointers down by one position to fill the gap
    for (int i = 1; i < q->count; i++) {
        q->items[i - 1] = q->items[i];
    }

    q->count--;
    return pcb;
}

/* Return true when no process is waiting in the ready queue. */
static bool queue_empty(const ReadyQueue *q)
{
    return q->count == 0;
}

/* Initialize one PCB for each process specification. */
static void initialize_pcbs(PCB *pcbs, const ProcessSpec *specs, size_t count)
{
    
    for (size_t i = 0; i < count; i++) {
        pcbs[i].spec = specs[i];                      
        pcbs[i].state = STATE_NEW;                    
        pcbs[i].remaining_cpu = specs[i].cpu_burst;
        pcbs[i].completed_cpu_bursts = 0;
        pcbs[i].io_completion_time = -1;
        pcbs[i].ready_arrival_order = -1;
        pcbs[i].quantum_used = 0;
    }
}

// Print the contents of a PCB for debugging purposes.
static void print_pcb(const PCB *pcb)
{
    printf("PID: %d\n", pcb->spec.pid);
    printf("State: %d\n", pcb->state);
    printf("Remaining CPU: %d\n", pcb->remaining_cpu);
    printf("Completed CPU Bursts: %d\n", pcb->completed_cpu_bursts);
    printf("I/O Completion Time: %d\n", pcb->io_completion_time);
    printf("Quantum Used: %d\n", pcb->quantum_used);
    printf("Ready Arrival Order: %d\n", pcb->ready_arrival_order);
}

// Add a PCB to the ready queue and update its state and arrival order.
static bool make_ready(PCB *pcb, ReadyQueue *ready, int *next_order)
{
    pcb->state = STATE_READY;
    pcb->ready_arrival_order = (*next_order)++;      // Increment the next order for the next process that will be added to the ready queue
    return enqueue(ready, pcb);
}

// Move newly arrived processes from NEW to READY.
static bool admit_arrivals(PCB *pcbs, size_t count, int time, ReadyQueue *ready, int *next_order, OutputContext *output)
{
    for (size_t i = 0; i < count; i++) {

        // If the process is in the NEW state and its arrival time matches the current time, move it to the READY state and log the transition.
        if (pcbs[i].state == STATE_NEW && pcbs[i].spec.arrival_time == time) {
            if (!make_ready(&pcbs[i], ready, next_order)) {
                return false;
            }
            log_transition(output, time, pcbs[i].spec.pid, STATE_NEW, STATE_READY, REASON_ARRIVAL);
        }
    }
    return true;
}

// Complete any I/O operations that finish at the current time and move those processes to the ready queue.
static bool complete_io(PCB *pcbs, size_t count, int time, ReadyQueue *ready, int *next_order, OutputContext *output)
{
    for (size_t i = 0; i < count; i++) {
        
        // If the process is in the WAITING state and its I/O completion time matches the current time, move it to the READY state and log the transition.
        if (pcbs[i].state == STATE_WAITING && pcbs[i].io_completion_time == time) {
            if (!make_ready(&pcbs[i], ready, next_order)) {
                return false;
            }
            log_transition(output, time, pcbs[i].spec.pid, STATE_WAITING, STATE_READY, REASON_IO_COMPLETE);
        }
    }
    return true;
}

// Resolve the CPU interval that ended at the current time, updating the running process and ready queue as needed.
static bool resolve_cpu_interval(PCB **running, int *terminated_count, int time, Algorithm algorithm, int quantum, ReadyQueue *ready, int *next_order, OutputContext *output)
{
    PCB *process = *running;
    
    // If there is no running process, return true to indicate that the CPU is idle.
    if (process == NULL) {
        return true;
    }

    // If the running process has completed its CPU burst, update its state and log the transition. If it has completed all its CPU bursts, mark it as terminated; otherwise, move it to the WAITING state for I/O.
    if (process->remaining_cpu == 0) {
        process->completed_cpu_bursts++;

        // If the process has completed all its CPU bursts, mark it as terminated and log the transition.
        if (process->completed_cpu_bursts >= process->spec.num_cpu_bursts) {
            process->state = STATE_TERMINATED;
            log_transition(output, time, process->spec.pid, STATE_RUNNING, STATE_TERMINATED, REASON_PROCESS_COMPLETE);
            (*terminated_count)++;
            *running = NULL;
        }

        // Otherwise, move the process to the WAITING state for I/O and log the transition.
        else {
            process->state = STATE_WAITING;
            process->io_completion_time = time + process->spec.io_burst;
            process->remaining_cpu = process->spec.cpu_burst;
            process->quantum_used = 0;
            log_transition(output, time, process->spec.pid, STATE_RUNNING, STATE_WAITING, REASON_CPU_BURST_COMPLETE);
            *running = NULL;
        }

        // Return true to indicate that the CPU interval has been resolved.
        return true;
    }

    // If the running process has not completed its CPU burst, check if it has exceeded its time quantum. If so, move it to the READY state and log the transition.
    if (algorithm == ALG_RR && process->quantum_used >= quantum) {
        if (!make_ready(process, ready, next_order)) {
            return false;
        }
        log_transition(output, time, process->spec.pid, STATE_RUNNING, STATE_READY, REASON_QUANTUM_EXPIRED);
        *running = NULL;
    }

    return true;
}

// Find the ready process with the least remaining CPU time
static int best_srtf_index(const ReadyQueue *ready)
{
    // Ready queue is empty, return -1 to indicate no process is available
    if (ready->count == 0) {
        return -1;
    }

    int best = 0;  

    // Iterate through the ready queue to find the process with the least remaining CPU time
    for (int i = 1; i < ready->count; i++) {
        PCB *candidate = ready->items[i];
        PCB *current_best = ready->items[best];

        // Compare remaining CPU times, arrival order, and PID to determine the best candidate
        if (candidate->remaining_cpu < current_best->remaining_cpu) {
            best = i;
        }
        // If remaining CPU times are equal, compare arrival order
        else if (candidate->remaining_cpu == current_best->remaining_cpu) {
            // If arrival orders are equal, compare PIDs
            if (candidate->ready_arrival_order < current_best->ready_arrival_order) {
                best = i;
            }
            // If arrival orders are equal, compare PIDs
            else if (candidate->ready_arrival_order == current_best->ready_arrival_order &&
                     candidate->spec.pid < current_best->spec.pid) {
                best = i;
            }
        }
    }
    return best;    // Return the index of the process with the least remaining CPU time in the ready queue
}

// Select the next process to run based on the scheduling algorithm
static PCB *select_next_process(ReadyQueue *ready, Algorithm algorithm)
{
    // If the ready queue is empty, return NULL to indicate no process is available
    if (ready->count == 0) {
        return NULL;
    }

    // For non-SRTF algorithms, simply dequeue the next process from the ready queue
    if (algorithm != ALG_SRTF) {
        return dequeue(ready);
    }

    int index = best_srtf_index(ready);
    PCB *selected = ready->items[index];

    // Remove the selected process from the ready queue by shifting the remaining processes down
    for (int i = index + 1; i < ready->count; i++) {
        ready->items[i - 1] = ready->items[i];
    }

    ready->count--;
    return selected;
}

// Check if the currently running process should be preempted by a new process in the ready queue based on the SRTF algorithm
static bool maybe_preempt_srtf(PCB **running, ReadyQueue *ready_queue, int *next_order, OutputContext *output, int time)
{
    // If there is no running process or the ready queue is empty, no preemption is needed
    if (*running == NULL || queue_empty(ready_queue)) {
        return true;
    }

    int index = best_srtf_index(ready_queue);
    PCB *candidate = ready_queue->items[index];

    // If the candidate process has more remaining CPU time than the currently running process, no preemption is needed
    if (candidate->remaining_cpu >= (*running)->remaining_cpu) {
        return true;
    }

    PCB *process = *running;

    // If the candidate process has less remaining CPU time, preempt the currently running process and move it to the ready queue
    if (!make_ready(process, ready_queue, next_order)) {
        return false;
    }

    // Log the preemption transition from RUNNING to READY due to SRTF preemption
    log_transition(output, time, process->spec.pid, STATE_RUNNING, STATE_READY, REASON_SRTF_PREEMPTION);

    *running = NULL; 
    return true; 
}

static bool dispatch_process(PCB **running, ReadyQueue *ready, Algorithm algorithm, OutputContext *output, int time)
{
    // If the ready queue is empty, set the running process to NULL and return true
    if (queue_empty(ready)) {
        *running = NULL;
        return true;
    }

    *running = select_next_process(ready, algorithm);
    if (*running == NULL) {
        return true;
    }

    (*running)->state = STATE_RUNNING;
    (*running)->quantum_used = 0;

    // Log the transition from READY to RUNNING due to dispatching the selected process
    log_transition(output, time, (*running)->spec.pid, STATE_READY, STATE_RUNNING, REASON_DISPATCH);

    return true;
}

// Run the simulation for the given process specifications and configuration, logging transitions and writing metrics to the output context.
bool run_simulation(const ProcessSpec *specs, size_t count, const SimulationConfig *config, OutputContext *output)
{
    // Validate input parameters and configuration settings
    if (specs == NULL || config == NULL || output == NULL) {
        return false;
    }

    // Validate the number of processes and context switch time
    if (count > MAX_PROCESSES || config->context_switch < 0) {
        return false;
    }

    // Validate the quantum for Round Robin scheduling
    if (config->algorithm == ALG_RR && config->quantum <= 0) {
        return false;
    }

    // If there are no processes to simulate, write metrics and return
    if (count == 0) {
        return write_metrics(output, specs, count);
    }

    // Allocate memory for the PCBs and initialize them based on the provided process specifications
    PCB *pcbs = malloc(count * sizeof(PCB));
    if (pcbs == NULL) {
        return false;
    }

    initialize_pcbs(pcbs, specs, count);

    ReadyQueue ready = {0};                   // Initialize the ready queue to be empty
    PCB *running = NULL;                      // Pointer to the currently running process

    int time = 0;                             // Initialize the simulation time to 0
    int terminated_count = 0;                 // Initialize the count of terminated processes to 0
    int next_order = 0;                       // Initialize the next order for processes entering the ready queue to 0

    bool context_active = false;              // Flag to indicate if a context switch is currently in progress
    int context_remaining = 0;                // Remaining time for the context switch
 
    // Main simulation loop: continue until all processes have terminated
    while (terminated_count < (int)count) {
        /* 1. Resolve the CPU interval that ended at the current time */
        if (!resolve_cpu_interval(&running, &terminated_count, time, config->algorithm, config->quantum, &ready, &next_order, output)) {
            free(pcbs);
            return false;
        }

        /* Exit loop if all processes have terminated */
        if (terminated_count == (int)count) {
            break;
        }

        /* 2. Admit arrivals in ascending PID order */
        if (!admit_arrivals(pcbs, count, time, &ready, &next_order, output)) {
            free(pcbs);
            return false;
        }

        /* 3. Complete I/O in ascending PID order */
        if (!complete_io(pcbs, count, time, &ready, &next_order, output)) {
            free(pcbs);
            return false;
        }

        /* 4. Apply SRTF preemption if needed */
        if (config->algorithm == ALG_SRTF) {
            if (!maybe_preempt_srtf(&running, &ready, &next_order, output, time)) {
                free(pcbs);
                return false;
            }
        }

        // 5. Determine the current activity for the timeline and manage context switching
        TimelineActivity activity = ACTIVITY_IDLE;
        int activity_pid = 0;

    
        if (running != NULL) {
            /* Process is currently running on CPU */
            activity = ACTIVITY_CPU;
            activity_pid = running->spec.pid;
        }
        else if (context_active && context_remaining > 0) {
            /* Context switch is still in progress */
            activity = ACTIVITY_CONTEXT_SWITCH;
            context_remaining--;
        }
        else if (context_active && context_remaining == 0) {
            /* Context switch just completed */
            context_active = false;
            
            // Dispatch the next process from the ready queue to run on the CPU
            if (!dispatch_process(&running, &ready, config->algorithm, output, time)) {
                free(pcbs);
                return false;
            }
            // If a process was dispatched, update the activity to CPU and set the activity PID
            if (running != NULL) {
                activity = ACTIVITY_CPU;
                activity_pid = running->spec.pid;
            }
            /* else: remains ACTIVITY_IDLE */
        }
    
        else if (!queue_empty(&ready)) {
            /* No process running and no context switch in progress */
            if (config->context_switch > 0) {
                /* Start a new context switch */
                context_active = true;
                context_remaining = config->context_switch - 1;
                activity = ACTIVITY_CONTEXT_SWITCH;
            }
            else {
                /* Zero context-switch cost: dispatch immediately */
                if (!dispatch_process(&running, &ready, config->algorithm, output, time)) {
                    free(pcbs);
                    return false;
                }
                // If a process was dispatched, update the activity to CPU and set the activity PID
                if (running != NULL) {
                    activity = ACTIVITY_CPU;
                    activity_pid = running->spec.pid;
                }
            }
        }
        /* else: CPU is idle */

        /* 6. Record exactly one timeline tick for [time, time+1) */
        record_timeline_tick(output, time, activity, activity_pid);

        /* 7. Account for the CPU interval that will end at time + 1 */
        if (activity == ACTIVITY_CPU && running != NULL) {
            running->remaining_cpu--;
        
            if (config->algorithm == ALG_RR) {
                running->quantum_used++;
            }
        }

        time++;
    }

    // Write the simulation metrics to the output context and free allocated memory
    bool success = write_metrics(output, specs, count);

    free(pcbs);
    return success;
}

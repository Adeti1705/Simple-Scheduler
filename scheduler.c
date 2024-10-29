// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <stdbool.h>
// #include <unistd.h>
// #include <sys/wait.h>
// #include <sys/time.h>
// #include <signal.h>
// #include <sys/mman.h>
// #include <sys/stat.h>
// #include <fcntl.h>
// #include <semaphore.h>
// #include <errno.h>

// #define MAX_SUBMIT 25

// struct Process {
//     int pid, priority;
//     bool submit, queue, completed;
//     char command[100];
//     struct timeval start;
//     //int time_slice_count;
//     //bool in_queue;
//     unsigned long execution_time, wait_time, vruntime;
//     //int arrival_time;
// };

// struct history_struct {
//     int history_count, ncpu, tslice;
//     sem_t mutex;
//     struct Process history[100];
// };

// struct Queue {
//     int front, rear, size, capacity;
//     struct Process **table;
// };

// struct PriorityQueue {
//     int size, capacity;
//     struct Process **heap;
// };

// int shm_fd;
// volatile sig_atomic_t terminate_flag = 0;
// struct history_struct *p_table;
// struct Queue *running_queue;
// struct PriorityQueue *ready_queue;
// //int total_elapsed_time = 0;

// bool is_queue_empty(struct Queue *q) {
//     return q->size == 0;
// }

// bool is_queue_full(struct Queue *q) {
//     return q->size == q->capacity;
// }

// void enqueue(struct Queue *q, struct Process *proc) {
//     if (is_queue_full(q)) return;
//     q->table[q->rear] = proc;
//     q->rear = (q->rear + 1) % q->capacity;
//     q->size++;
// }

// struct Process* dequeue(struct Queue *q) {
//     if (is_queue_empty(q)) return NULL;
//     struct Process *proc = q->table[q->front];
//     q->front = (q->front + 1) % q->capacity;
//     q->size--;
//     return proc;
// }

// // Priority Queue operations
// bool is_pqueue_empty(struct PriorityQueue *pq) {
//     return pq->size == 0;
// }

// bool is_pqueue_full(struct PriorityQueue *pq) {
//     return pq->size == pq->capacity;
// }

// void swap_processes(struct Process **a, struct Process **b) {
//     struct Process *temp = *a;
//     *a = *b;
//     *b = temp;
// }
// void cleanup_and_exit() {
//     printf("\nTerminating scheduler...\n");

//     free(running_queue->table);
//     free(running_queue);
//     free(ready_queue->heap);
//     free(ready_queue);
//     if (sem_destroy(&p_table->mutex) == -1){
//         perror("shm_destroy");
//         exit(1);
//     }
//     if (munmap(p_table, sizeof(struct history_struct)) < 0) {
//         perror("munmap");
//     }

//     if (close(shm_fd) == -1) {
//         perror("close");
//     }

//     exit(0);
// }
// void heapify_up(struct PriorityQueue *pq, int index) {
//     while (index > 0) {
//         int parent = (index - 1) / 2;
//         if (pq->heap[index]->execution_time < pq->heap[parent]->execution_time) {
//             swap_processes(&pq->heap[index], &pq->heap[parent]);
//             index = parent;
//         } else {
//             break;
//         }
//     }
// }

// void heapify_down(struct PriorityQueue *pq, int index) {
//     int min_index = index;
//     int left_child = 2 * index + 1;
//     int right_child = 2 * index + 2;

//     if (left_child < pq->size && pq->heap[left_child]->execution_time < pq->heap[min_index]->execution_time) {
//         min_index = left_child;
//     }

//     if (right_child < pq->size && pq->heap[right_child]->execution_time < pq->heap[min_index]->execution_time) {
//         min_index = right_child;
//     }

//     if (min_index != index) {
//         swap_processes(&pq->heap[index], &pq->heap[min_index]);
//         heapify_down(pq, min_index);
//     }
// }

// void pq_insert(struct PriorityQueue *pq, struct Process *proc) {
//     if (is_pqueue_full(pq)) return;
//     pq->heap[pq->size] = proc;
//     heapify_up(pq, pq->size);
//     pq->size++;
// }

// struct Process* pq_extract_min(struct PriorityQueue *pq) {
//     if (is_pqueue_empty(pq)) return NULL;
//     struct Process *min_proc = pq->heap[0];
//     pq->heap[0] = pq->heap[pq->size - 1];
//     pq->size--;
//     heapify_down(pq, 0);
//     return min_proc;
// }
// void execute_process(struct Process *proc) {
//     pid_t pid = fork();
//     if (pid < 0) {
//         perror("fork");
//         return;
//     } else if (pid == 0) {
//         // Child process
//         char *args[2] = {proc->command, NULL};
//         execvp(args[0], args);
//         perror("execvp");
//         exit(1);
//     } else {
//         // Parent process (scheduler)
//         proc->pid = pid;
//         printf("Started process %s with PID %d\n", proc->command, pid);
//     }
// }

// void schedule_processes(int cpu_count, int time_slice) {
//     while (1) {
//         unsigned int remaining_sleep = sleep(time_slice / 1000);

//         if (sem_wait(&p_table->mutex) == -1) {
//             perror("sem_wait");
//             exit(1);
//         }
//         if (terminate_flag && is_queue_empty(running_queue) && is_pqueue_empty(ready_queue)) {
//             cleanup_and_exit();
//         }

//         // Check for new processes and add them to ready queue
//         for (int i = 0; i < p_table->history_count+1; i++) {
//             if (p_table->history[i].submit && !p_table->history[i].completed && !p_table->history[i].queue) {
//                 printf("Adding process %d (PID: %d) to ready queue\n", i, p_table->history[i].pid);
//                 p_table->history[i].queue = true;
//                 pq_insert(ready_queue, &p_table->history[i]);
//             }
//         }

//         // Checking running queue and pausing the processes if they haven't terminated
//         if (!is_queue_empty(running_queue)) {
//             for (int i = 0; i < cpu_count; i++) {
//                 if (!is_queue_empty(running_queue)) {
//                     struct Process *proc = dequeue(running_queue);
//                     if (!proc->completed) {
//                         pq_insert(ready_queue, proc);
//                         struct timeval end;
//                         gettimeofday(&end, NULL);
//                         proc->execution_time += (end.tv_sec - proc->start.tv_sec) * 1000000 + (end.tv_usec - proc->start.tv_usec);
//                         proc->vruntime += proc->execution_time * proc->priority;
//                         if (kill(proc->pid, SIGSTOP) == -1) {

//                             perror("kill");
//                             exit(1);
//                         }
//                     }
//                 }
//             }
//         }

//         // Move processes from ready to running queue
//         while (!is_pqueue_empty(ready_queue) && running_queue->size < cpu_count) {
//             struct Process *proc = pq_extract_min(ready_queue);
//             struct timeval now;
//             gettimeofday(&now, NULL);
//             proc->wait_time += (now.tv_sec - proc->start.tv_sec) * 1000000 + (now.tv_usec - proc->start.tv_usec);
//             gettimeofday(&proc->start, NULL);
//             if (kill(proc->pid, SIGCONT) == -1) {
//                 printf("nigger");
//                 perror("kill");
//                 exit(1);
//             }
//             enqueue(running_queue, proc);
//         }

//         if (sem_post(&p_table->mutex) == -1) {
//             perror("sem_post");
//             exit(1);
//         }
//     }
// }

//         // // Update waiting time for processes in ready queue
//         // for (int i = 0; i < ready_queue->size; i++) {
//         //     ready_queue->heap[i]->wait_time += time_slice;
//         // }

//         // Process running queue
//         // int running_count = running_queue->size;
//         // for (int i = 0; i < running_count; i++) {
//         //     struct Process *proc = dequeue(running_queue);
//         //     if (proc && !proc->completed) {
//         //         proc->time_slice_count++;
//         //         proc->execution_time += time_slice;

//         //         // Check if process has completed
//         //         int status;
//         //         pid_t result = waitpid(proc->pid, &status, WNOHANG);
//         //         if (result == proc->pid) {
//         //             proc->completed = true;
//         //             printf("Process %d (PID: %d) completed\n", i, proc->pid);
//         //         } else if (result == 0) {
//         //             // Process is still running
//         //             if (proc->time_slice_count >= 10) {  // Every 10 time slices
//         //                 pq_insert(ready_queue, proc);
//         //                 kill(proc->pid, SIGSTOP);
//         //                 printf("Process %d (PID: %d) moved to ready queue\n", i, proc->pid);
//         //             } else {
//         //                 enqueue(running_queue, proc);
//         //             }
//         //         } else {
//         //             perror("waitpid");
//         //         }
//         //     }
//         // }

// static void signal_handler(int signum) {
//     if (signum == SIGINT) {
//         terminate_flag = 1;
//     }
// }

// int main() {
//     struct sigaction sa;
//     memset(&sa, 0, sizeof(sa));
//     sa.sa_handler = signal_handler;
//     if (sigaction(SIGINT, &sa, NULL) == -1) {
//         perror("sigaction");
//         exit(1);
//     }

//     shm_fd = shm_open("shm", O_RDWR, 0666);
//     if (shm_fd == -1) {
//         perror("shm_open");
//         exit(1);
//     }

//     p_table = mmap(NULL, sizeof(struct history_struct), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
//     if (p_table == MAP_FAILED) {
//         perror("mmap");
//         exit(1);
//     }

//     // // Wait for the shell to initialize the shared memory
//     // while (p_table->ncpu == 0 || p_table->tslice == 0) {
//     //     usleep(100000);  // Sleep for 100ms
//     // }

//     int cpu_count = p_table->ncpu;
//     int time_slice = p_table->tslice;

//     ready_queue = malloc(sizeof(struct PriorityQueue));
//      if (ready_queue == NULL){
//         perror("malloc");
//         exit(1);
//     }
//     ready_queue->size = 0;
//     ready_queue->capacity = MAX_SUBMIT;
//     ready_queue->heap =(struct Process **) malloc(ready_queue->capacity * sizeof(struct Process));
//     if (ready_queue->heap== NULL){
//         perror("malloc");
//         exit(1);
//     }

//     for (int i=0; i<ready_queue->capacity; i++){
//         ready_queue->heap[i] = (struct Process *)malloc(sizeof(struct Process));
//         if (ready_queue->heap[i] == NULL){
//             perror("malloc");
//             exit(1);
//         }
//     }
//     running_queue = (struct Queue *) (malloc(sizeof(struct Queue)));
//     running_queue->front = running_queue->rear = running_queue->size = 0;
//     running_queue->capacity = cpu_count+1;
//     running_queue->table = (struct Process **) malloc(running_queue->capacity * sizeof(struct Process));
//     for (int i=0; i<running_queue->capacity; i++){
//         running_queue->table[i] = (struct Process *)malloc(sizeof(struct Process));
//         if (running_queue->table[i] == NULL){
//             perror("malloc");
//             exit(1);
//         }
//     }

//      // initialising a semaphore
//     if (sem_init(&p_table->mutex, 1, 1) == -1){
//         perror("sem_init");
//         exit(1);
//     }
//     //creating daemon process
//     if(daemon(1, 1)){
//         perror("daemon");
//         exit(1);
//     }
//     schedule_processes(cpu_count, time_slice);

//     cleanup_and_exit();
//     return 0;
// }
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <errno.h>

#define MAX_SUBMIT 25

struct Process
{
    int pid, priority;
    bool submit, queue, completed;
    char command[100];
    struct timeval start;
    int time_slice_count;
    bool in_queue;
    unsigned long execution_time, wait_time, vruntime;
    int arrival_time;
};

struct history_struct
{
    int history_count, ncpu, tslice;
    sem_t mutex;
    struct Process history[100];
};

struct Queue
{
    int front, rear, size, capacity;
    struct Process **items;
};

struct PriorityQueue
{
    int size, capacity;
    struct Process **items;
};

int shm_fd;
volatile sig_atomic_t terminate_flag = 0;
struct history_struct *p_table;
struct Queue *running_queue;
struct PriorityQueue *ready_queue;
int total_elapsed_time = 0;

bool is_queue_empty(struct Queue *q)
{
    return q->size == 0;
}

bool is_queue_full(struct Queue *q)
{
    return q->size == q->capacity;
}

void enqueue(struct Queue *q, struct Process *proc)
{
    if (is_queue_full(q))
        return;
    q->items[q->rear] = proc;
    q->rear = (q->rear + 1) % q->capacity;
    q->size++;
}

struct Process *dequeue(struct Queue *q)
{
    if (is_queue_empty(q))
        return NULL;
    struct Process *proc = q->items[q->front];
    q->front = (q->front + 1) % q->capacity;
    q->size--;
    return proc;
}

// Priority Queue operations
bool is_pqueue_empty(struct PriorityQueue *pq)
{
    return pq->size == 0;
}

bool is_pqueue_full(struct PriorityQueue *pq)
{
    return pq->size == pq->capacity;
}

void swap_processes(struct Process **a, struct Process **b)
{
    struct Process *temp = *a;
    *a = *b;
    *b = temp;
}

void heapify_up(struct PriorityQueue *pq, int index)
{
    while (index > 0)
    {
        int parent = (index - 1) / 2;
        if (pq->items[index]->execution_time < pq->items[parent]->execution_time)
        {
            swap_processes(&pq->items[index], &pq->items[parent]);
            index = parent;
        }
        else
        {
            break;
        }
    }
}

void heapify_down(struct PriorityQueue *pq, int index)
{
    int min_index = index;
    int left_child = 2 * index + 1;
    int right_child = 2 * index + 2;

    if (left_child < pq->size && pq->items[left_child]->execution_time < pq->items[min_index]->execution_time)
    {
        min_index = left_child;
    }

    if (right_child < pq->size && pq->items[right_child]->execution_time < pq->items[min_index]->execution_time)
    {
        min_index = right_child;
    }

    if (min_index != index)
    {
        swap_processes(&pq->items[index], &pq->items[min_index]);
        heapify_down(pq, min_index);
    }
}

void pq_insert(struct PriorityQueue *pq, struct Process *proc)
{
    if (is_pqueue_full(pq))
        return;
    pq->items[pq->size] = proc;
    heapify_up(pq, pq->size);
    pq->size++;
}

struct Process *pq_extract_min(struct PriorityQueue *pq)
{
    if (is_pqueue_empty(pq))
        return NULL;
    struct Process *min_proc = pq->items[0];
    pq->items[0] = pq->items[pq->size - 1];
    pq->size--;
    heapify_down(pq, 0);
    return min_proc;
}
void execute_process(struct Process *proc)
{
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork");
        return;
    }
    else if (pid == 0)
    {
        // Child process
        char *args[2] = {proc->command, NULL};
        execvp(args[0], args);
        perror("execvp");
        exit(1);
    }
    else
    {
        // Parent process (scheduler)
        proc->pid = pid;
        printf("Started process %s with PID %d\n", proc->command, pid);
    }
}

// void schedule_processes(int cpu_count, int time_slice)
// {
//     while (1)
//     {
//         usleep(time_slice * 1000);
//         total_elapsed_time += time_slice;

//         if (sem_wait(&p_table->mutex) == -1)
//         {
//             perror("sem_wait");
//             exit(1);
//         }

//         // Check for new processes and add them to ready queue
//         for (int i = 0; i < p_table->history_count; i++)
//         {
//             printf("nigerr");
//             if (p_table->history[i].submit && !p_table->history[i].completed && !p_table->history[i].in_queue)
//             {
//                 printf("Adding process %d (PID: %d) to ready queue\n", i, p_table->history[i].pid);
//                 p_table->history[i].in_queue = true;
//                 pq_insert(ready_queue, &p_table->history[i]);
//             }
//         }

//         // Update waiting time for processes in ready queue
//         for (int i = 0; i < ready_queue->size; i++)
//         {
//             ready_queue->items[i]->wait_time += time_slice;
//         }

//         // Process running queue
//         int running_count = running_queue->size;
// for (int i = 0; i < running_count; i++) {
//     struct Process *proc = dequeue(running_queue);
//     if (proc && !proc->completed) {
//         proc->time_slice_count++;
//         proc->execution_time += time_slice;

//         // Check if process has completed
//         int status;
//         pid_t result = waitpid(proc->pid, &status, WNOHANG);
//         if (result == proc->pid) {
//             proc->completed = true;
//             printf("Process %d (PID: %d) completed\n", i, proc->pid);
//         } else if (result == 0) {
//             // Process is still running
//             if (proc->time_slice_count >= 10) {  // Every 10 time slices
//                 pq_insert(ready_queue, proc);
//                 kill(proc->pid, SIGSTOP);
//                 printf("Process %d (PID: %d) moved to ready queue\n", i, proc->pid);
//             } else {
//                 enqueue(running_queue, proc);
//             }
//         } else {
//             perror("waitpid");
//         }
//     }
// }
void schedule_processes(int cpu_count, int time_slice) {
    while (1) {
        usleep(time_slice * 1000);  // Convert ms to μs
        total_elapsed_time += time_slice;

        if (sem_wait(&p_table->mutex) == -1) {
            perror("sem_wait");
            exit(1);
        }

        if (terminate_flag && is_queue_empty(running_queue) && is_pqueue_empty(ready_queue)) {
            sem_post(&p_table->mutex);
            break;
        }

        // Check for new processes and add them to ready queue
        for (int i = 0; i < p_table->history_count; i++) {
            if (p_table->history[i].submit && !p_table->history[i].completed && !p_table->history[i].queue) {
                p_table->history[i].queue = true;
                p_table->history[i].arrival_time = total_elapsed_time;
                printf("Adding process %d to ready queue\n", p_table->history[i].pid);
                pq_insert(ready_queue, &p_table->history[i]);
            }
        }

        // Update waiting time for processes in ready queue
        for (int i = 0; i < ready_queue->size; i++) {
            ready_queue->items[i]->wait_time += time_slice;
        }

        // Move processes from running to ready queue if time slice expired
        int running_count = running_queue->size;
        for (int i = 0; i < running_count; i++) {
            struct Process *proc = dequeue(running_queue);
            if (!proc->completed) {
                proc->execution_time += time_slice;

                // Check if process has completed
                if (kill(proc->pid, 0) == -1 && errno == ESRCH) {
                    proc->completed = true;
                    printf("Process %d completed\n", proc->pid);
                    continue;
                }

                if (proc->execution_time % time_slice == 0) {
                    printf("Moving process %d back to ready queue\n", proc->pid);
                    pq_insert(ready_queue, proc);
                    if (kill(proc->pid, SIGSTOP) == -1) {
                        perror("kill SIGSTOP");
                    }
                } else {
                    enqueue(running_queue, proc);
                }
            }
        }

        // Move processes from ready to running queue
        while (running_queue->size < cpu_count && !is_pqueue_empty(ready_queue)) {
            struct Process *proc = pq_extract_min(ready_queue);
            printf("Moving process %d to running queue\n", proc->pid);
            if (kill(proc->pid, SIGCONT) == -1) {
                perror("kill SIGCONT");
            }
            enqueue(running_queue, proc);
        }

        if (sem_post(&p_table->mutex) == -1) {
            perror("sem_post");
            exit(1);
        }
    }
}
static void signal_handler(int signum)
{
    if (signum == SIGINT)
    {
        terminate_flag = 1;
    }
}

void cleanup_and_exit()
{
    printf("\nTerminating scheduler...\n");

    free(running_queue->items);
    free(running_queue);
    free(ready_queue->items);
    free(ready_queue);

    if (sem_destroy(&p_table->mutex) == -1)
    {
        perror("sem_destroy");
    }
    if (munmap(p_table, sizeof(struct history_struct)) < 0)
    {
        perror("munmap");
    }

    if (close(shm_fd) == -1)
    {
        perror("close");
    }
    if (shm_unlink("shm") == -1)
    {
        perror("shm_unlink");
    }

    exit(0);
}

int main()
{
    printf("schedulerrrrr\n");
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    if (sigaction(SIGINT, &sa, NULL) == -1)
    {
        perror("sigaction");
        exit(1);
    }

    shm_fd = shm_open("shm", O_RDWR, 0666);
    if (shm_fd == -1)
    {
        perror("shm_open");
        exit(1);
    }

    p_table = mmap(NULL, sizeof(struct history_struct), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (p_table == MAP_FAILED)
    {
        perror("mmap");
        exit(1);
    }

    // Wait for the shell to initialize the shared memory
    while (p_table->ncpu == 0 || p_table->tslice == 0)
    {
        usleep(100000); // Sleep for 100ms
    }

    int cpu_count = p_table->ncpu;
    int time_slice = p_table->tslice;

    ready_queue = malloc(sizeof(struct PriorityQueue));
    ready_queue->size = 0;
    ready_queue->capacity = MAX_SUBMIT;
    ready_queue->items = malloc(ready_queue->capacity * sizeof(struct Process *));

    running_queue = malloc(sizeof(struct Queue));
    running_queue->front = running_queue->rear = running_queue->size = 0;
    running_queue->capacity = cpu_count;
    running_queue->items = malloc(running_queue->capacity * sizeof(struct Process *));
    for (int i = 0; i < running_queue->capacity; i++) {
        running_queue->items[i] = malloc(sizeof(struct Process));
    }

    schedule_processes(cpu_count, time_slice);

    cleanup_and_exit();
    return 0;
}
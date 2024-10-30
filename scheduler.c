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
    bool submit, completed;
    char command[100];
    struct timeval start;
    int time_slice_count;
    bool in_queue;
    unsigned long execution_time, wait_time;
    // int arrival_time;
};

struct history_struct
{
    int history_count, ncpu, tslice;
    sem_t mutex;
    struct Process history[100];
};

struct Queue
{
    // no curr
    int front, rear, size, capacity;
    struct Process **items1;
};

struct PriorityQueue
{
    int size, capacity;
    struct Process **items2;
};

int shm_fd;
// volatile sig_atomic_t terminate_flag = 0;
bool terminate_flag = false;
struct history_struct *p_table;
struct Queue *running_queue;
struct PriorityQueue *ready_queue;
// int total_elapsed_time = 0;

bool is_queue_empty(struct Queue *q)
{
    return q->front == q->rear;
}
int next_head(struct Queue *q)
{
    if (q->front == q->capacity - 1)
    {
        return 0;
    }
    return q->front + 1;
}
int next_tail(struct Queue *q)
{
    if (q->rear == q->capacity - 1)
    {
        return 0;
    }
    return q->rear + 1;
}

bool is_queue_full(struct Queue *q)
{
    return next_tail(q) == q->front;
}

void enqueue(struct Queue *q, struct Process *proc)
{
    if (is_queue_full(q))
        return;
    q->size++;
    q->items1[q->rear] = proc;
    q->rear = next_tail(q);
}

void dequeue(struct Queue *q)
{
    if (is_queue_empty(q))
        return;

    // struct Process *proc = q->items1[q->front];
    q->front = next_head(q);
    q->size--;
    return;
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
        if (pq->items2[index]->priority < pq->items2[parent]->priority)
        {
            swap_processes(&pq->items2[index], &pq->items2[parent]);
            index = parent;
        }
        else
        {
            break;
        }
    }
}
//heap on the basis of execution time changing to vruntime
void heapify_down(struct PriorityQueue *pq, int index)
{
    int min_index = index;
    int left_child = 2 * index + 1;
    int right_child = 2 * index + 2;

    if (left_child < pq->size && pq->items2[left_child]->priority < pq->items2[min_index]->priority)
    {
        min_index = left_child;
    }

    if (right_child < pq->size && pq->items2[right_child]->priority < pq->items2[min_index]->priority)
    {
        min_index = right_child;
    }

    if (min_index != index)
    {
        swap_processes(&pq->items2[index], &pq->items2[min_index]);
        heapify_down(pq, min_index);
    }
}

void pq_insert(struct PriorityQueue *pq, struct Process *proc)
{
    if (is_pqueue_full(pq))
        return;
    pq->items2[pq->size] = proc;
    heapify_up(pq, pq->size);
    pq->size++;
}

struct Process *pq_extract_min(struct PriorityQueue *pq)
{
    if (is_pqueue_empty(pq))
        return NULL;
    struct Process *min_proc = pq->items2[0];
    pq->items2[0] = pq->items2[pq->size - 1];
    pq->size--;
    heapify_down(pq, 0);
    return min_proc;
}
void start_time(struct timeval *start)
{
    gettimeofday(start, 0);
}

// function to get time duration since start time
unsigned long end_time(struct timeval *start)
{
    struct timeval end;
    unsigned long t;

    gettimeofday(&end, 0);
    t = ((end.tv_sec * 1000) + end.tv_usec/1000) - ((start->tv_sec/1000) + start->tv_usec/1000);
    return t;
}


void cleanup_and_exit()
{
    printf("\nTerminating scheduler...\n");

    free(running_queue->items1);
    free(running_queue);
    free(ready_queue->items2);
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
void schedule_processes(int cpu_count, int time_slice)
{
    unsigned long total_elapsed_time = 0;
    while (1)
    {
        sleep(time_slice/1000);
        //printf("hi after sleep\n");
        total_elapsed_time += time_slice;
        if (sem_wait(&p_table->mutex) == -1)
        {
            perror("sem_wait4");
            exit(1);
        }
        
        if (terminate_flag && is_queue_empty(running_queue) && is_pqueue_empty(ready_queue))
        {
            printf("nigeerrrrrrrrrrffffffffff...\n");
            cleanup_and_exit();
            sem_post(&p_table->mutex);
            break;
        }

        // Check for new processes and add them to ready queue
        for (int i = 0; i < p_table->history_count; i++)
        {
            if (p_table->history[i].submit && !p_table->history[i].completed && !p_table->history[i].in_queue)

            {
                //printf("Scheduler found new process: %s, PID: %d\n", p_table->history[i].command, p_table->history[i].pid);
                if (ready_queue->size + cpu_count < ready_queue->capacity - 1)
                {
                    p_table->history[i].in_queue = true;
                    // p_table->history[i].arrival_time = total_elapsed_time;
                    printf("Adding process %d to ready queue\n", p_table->history[i].pid);
                    pq_insert(ready_queue, &p_table->history[i]);
                }
                else
                {
                    break;
                }
            }
        }
        if (!is_queue_empty(running_queue))
        {
            for (int i = 0; i < cpu_count; i++)
            {
                if (!is_queue_empty(running_queue))
                {   struct Process *proc = running_queue->items1[running_queue->front];
                    if (!proc->completed)
                    {
                        pq_insert(ready_queue,proc);
                        proc->execution_time += time_slice;
                        start_time(&proc->start);
                        if (kill(proc->pid, SIGSTOP) == -1)
                        {
                            perror("kill");
                            exit(1);
                        }
                        dequeue(running_queue);
                    }
                    else
                    {
                        dequeue(running_queue);
                    }
                }
            }
        }
        if (!is_pqueue_empty(ready_queue))
        {
            for (int i = 0; i < cpu_count; i++)
            {
                if (!is_pqueue_empty(ready_queue))
                {
                    struct Process *proc = pq_extract_min(ready_queue);
                    printf("gonna start running process %d\n", proc->pid);
                    proc->wait_time += end_time(&proc->start)-time_slice;
                    start_time(&proc->start);
                    if (kill(proc->pid, SIGCONT) == -1)
                    {
                        perror("kill");
                        exit(1);
                    }
                    enqueue(running_queue, proc);
                }
            }
        }
        if (sem_post(&p_table->mutex) == -1)
        {
            perror("sem_post");
            exit(1);
        }
         
    }
}

static void signal_handler(int signum)
{
    if (signum == SIGINT)
    {
        terminate_flag = true;
    }
}

int main()
{
    //printf("schedulerrrrr\n");
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
    ready_queue->items2 = (struct Process **)malloc(ready_queue->capacity * sizeof(struct Process *));
    for (int i = 0; i < ready_queue->capacity; i++)
    {
        ready_queue->items2[i] = (struct Process *)malloc(sizeof(struct Process));
    }

    running_queue = malloc(sizeof(struct Queue));
    running_queue->front = running_queue->rear = running_queue->size = 0;
    running_queue->capacity = cpu_count;
    running_queue->items1 = (struct Process **)malloc(running_queue->capacity * sizeof(struct Process));
    for (int i = 0; i < running_queue->capacity; i++)
    {
        running_queue->items1[i] = (struct Process *)malloc(sizeof(struct Process));
    }

    //creating daemon process
    // 

    schedule_processes(cpu_count, time_slice);

    cleanup_and_exit();
    return 0;
}
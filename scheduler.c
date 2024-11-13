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

//struct definitions
struct Process
{
    int pid, priority;
    bool submit, in_queue, completed;
    char command[100];
    struct timeval start;
    unsigned long exe_time, wait_time;
};

struct hist
{
    int history_count, ncpu, time_slice;
    sem_t mutex;
    struct Process history[100];
};

struct queue
{
    int head, tail, capacity, curr;
    struct Process **items;
};

struct pqueue
{
    int size, capacity;
    struct Process **min_heap;
};

int shm_fd;
bool ending = false;
struct hist *p_table;
struct queue *running;
struct pqueue *waiting;

// check if queue is empty
bool is_qempty(struct queue *q)
{
    return q->head == q->tail;
}

// check if queue is full
bool q_full(struct queue *q)
{
    return (q->tail + 1) % q->capacity == q->head;;
}

// add a process to the running queue
void running_enqueue(struct queue *q, struct Process *proc)
{
    if (q_full(q))
    {
        printf("rqueue overflow\n");
        return;
    }
    q->items[q->tail] = proc;
    q->tail = (q->tail + 1) % q->capacity; 
    q->curr++;
}

// remove a process from the running queue
void running_dequeue(struct queue *q)
{
    if (is_qempty(q))
    {
        printf("rqueue underflow\n");
        return;
    }
    q->head = (q->head + 1) % q->capacity;
    q->curr--;
}

// pqueue methods
// check if priority queue is empty
bool is_pqempty(struct pqueue *pq)
{
    return pq->size == 0;
}

// check if priority queue is full
bool pqueue_full(struct pqueue *pq)
{
    return pq->size == pq->capacity;
}

void swap_p(struct Process *a, struct Process *b)
{
    struct Process temp = *a;
    *a = *b;
    *b = temp;
}

// move a process up the heap to maintain heap property
void heapifyup(struct pqueue *pq, int index)
{
    while (index > 0)
    {
        int parent = (index - 1) / 2;
        if (pq->min_heap[index]->priority < pq->min_heap[parent]->priority)
        {
            swap_p(pq->min_heap[index], pq->min_heap[parent]);
            index = parent;
        }
        else
        {
            break;
        }
    }
}

// move a process down the heap to maintain heap property
void heapifydown(struct pqueue *pq, int index)
{
    int leftChild = 2 * index + 1;
    int rightChild = 2 * index + 2;
    int smallest = index;

    if (leftChild < pq->size && pq->min_heap[leftChild]->priority < pq->min_heap[smallest]->priority)
    {
        smallest = leftChild;
    }

    if (rightChild < pq->size && pq->min_heap[rightChild]->priority < pq->min_heap[smallest]->priority)
    {
        smallest = rightChild;
    }

    if (smallest != index)
    {
        swap_p(pq->min_heap[index], pq->min_heap[smallest]);
        heapifydown(pq, smallest);
    }
}

// add a process to the waiting queue
void waiting_enqueue(struct pqueue *pq, struct Process *proc)
{
    if (pq->size < pq->capacity)
    {
        pq->min_heap[pq->size] = proc;
        heapifyup(pq, pq->size);
        pq->size++;
    }
}

// remove the process with highest priority from waiting queue
struct Process *extract_min(struct pqueue *pq)
{
    if (pq->size > 0)
    {
        struct Process *removed = pq->min_heap[0];
        pq->min_heap[0] = pq->min_heap[pq->size - 1];
        pq->size--;
        heapifydown(pq, 0);
        return removed;
    }
    return NULL;
}

// to record start time
void start_time(struct timeval *start)
{
    gettimeofday(start, 0);
}

// function to calculate elapsed time
unsigned long end_time(struct timeval *start)
{
    struct timeval end;
    unsigned long t;

    gettimeofday(&end, 0);
    t = ((end.tv_sec / 1000) + end.tv_usec*1000) - ((start->tv_sec / 1000) + start->tv_usec*1000);
   //t = (end.tv_sec - start->tv_sec) * 1000 + (end.tv_usec - start->tv_usec) / 1000;

    return t;
}

//to terminate schduler
void terminate()
{
    printf("terminating scheduler\n");
    // cleanups for malloc
    free(running->items);
    free(running);
    free(waiting->min_heap);
    free(waiting);
    // destroying the semaphore
    if (sem_destroy(&p_table->mutex) == -1)
    {
        perror("shm_destroy");
        exit(1);
    }
    // unmapping shared memory segment followed by a "close" call
    if (munmap(p_table, sizeof(struct hist)) < 0)
    {
        printf("Error unmapping\n");
        perror("munmap");
        exit(1);
    }
    if (close(shm_fd) == -1)
    {
        perror("close");
        exit(1);
    }
    exit(0);
}

//main scheduler code- chks the queues and schedules the processes
void scheduler(int ncpu, int time_slice)
{
    while (true)
    {
       sleep(time_slice / 1000);
       //usleep(time_slice * 1000); 
        if (sem_wait(&p_table->mutex) == -1)
        {
            perror("sem_wait");
            exit(1);
        }
        // this if-block - scheduler terminates after natural endingination of all processes
        if (ending && is_qempty(running) && is_pqempty(waiting))
        {
            terminate();
        }

        // adding process to ready queue if they have submit true
        for (int i = 0; i < p_table->history_count; i++)
        {
            if (p_table->history[i].submit && !p_table->history[i].completed && !p_table->history[i].in_queue)
            {
                if (waiting->size + ncpu < waiting->capacity - 1)
                {
                    p_table->history[i].in_queue = true;
                    waiting_enqueue(waiting, &p_table->history[i]);
                }
                else
                {
                    break;
                }
            }
        }

        // checking if the running queue and stopping it after tslice
        if (!is_qempty(running))
        {
            for (int i = 0; i < ncpu; i++)
            {
                if (!is_qempty(running))
                {
                    if (!running->items[running->head]->completed)
                    {
                        struct Process *proc = running->items[running->head];
                        waiting_enqueue(waiting, proc);
                        proc->exe_time += time_slice;
                        start_time(&proc->start);

                        

                        if (kill(proc->pid, SIGSTOP) == -1)
                        {
                            perror("kill");
                            exit(1);
                        }
                    }
                    running_dequeue(running);
                }
            }
        }

        // adding processes to running queue based on ncpu
        if (!is_pqempty(waiting))
        {
            for (int i = 0; i < ncpu; i++)
            {
                if (!is_pqempty(waiting))
                {
                    struct Process *proc = extract_min(waiting);
                    proc->wait_time += end_time(&proc->start);
                    start_time(&proc->start);
                    if (kill(proc->pid, SIGCONT) == -1)
                    {
                        perror("kill");
                        exit(1);
                    }
                    running_enqueue(running, proc);
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

// signal handler
static void sign_handler(int signum)
{

    if (signum == SIGINT)
    {
        ending = true;
    }
}

// main function
int main()
{
    struct sigaction sig;
    if (memset(&sig, 0, sizeof(sig)) == 0)
    {
        perror("memset");
        exit(1);
    }
    sig.sa_handler = sign_handler;
    if (sigaction(SIGINT, &sig, NULL) == -1)
    {
        perror("sigaction");
        exit(1);
    }

    // accessing the shm in read-write mode
    shm_fd = shm_open("shm", O_RDWR, 0666);
    if (shm_fd == -1)
    {
        perror("shm_open");
        exit(1);
    }
    p_table = mmap(NULL, sizeof(struct hist), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (p_table == MAP_FAILED)
    {
        perror("mmap");
        exit(1);
    }
    int ncpu = p_table->ncpu;
    int time_slice = p_table->time_slice;

    waiting = (struct pqueue *)(malloc(sizeof(struct pqueue)));
    if (waiting == NULL)
    {
        perror("malloc");
        exit(1);
    }
    waiting->size = 0;
    waiting->capacity = 100;
    waiting->min_heap = (struct Process **)malloc(waiting->capacity * sizeof(struct Process));
    if (waiting->min_heap == NULL)
    {
        perror("malloc");
        exit(1);
    }
    for (int i = 0; i < waiting->capacity; i++)
    {
        waiting->min_heap[i] = (struct Process *)malloc(sizeof(struct Process));
        if (waiting->min_heap[i] == NULL)
        {
            perror("malloc");
            exit(1);
        }
    }

    running = (struct queue *)(malloc(sizeof(struct queue)));
    if (running == NULL)
    {
        perror("malloc");
        exit(1);
    }
    running->head = running->tail = running->curr = 0;
    running->capacity = ncpu + 1;
    running->items = (struct Process **)malloc(running->capacity * sizeof(struct Process));
    if (running->items == NULL)
    {
        perror("malloc");
        exit(1);
    }
    for (int i = 0; i < running->capacity; i++)
    {
        running->items[i] = (struct Process *)malloc(sizeof(struct Process));
        if (running->items[i] == NULL)
        {
            perror("malloc");
            exit(1);
        }
    }

    // initialising a semaphore
    if (sem_init(&p_table->mutex, 1, 1) == -1)
    {
        perror("sem_init");
        exit(1);
    }
    // daemon process
    if (daemon(1, 1))
    {
        perror("daemon");
        exit(1);
    }
    scheduler(ncpu, time_slice);
    terminate();
    return 0;
}

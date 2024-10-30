// header files
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
//struct process 
struct Process
{
    int pid, priority;
    bool submit, in_queue, completed; // flags
    char command[100];
    struct timeval start;
    unsigned long exe_time, wait_time;
};
//struct history
struct hist
{
    int history_count, ncpu, time_slice;
    sem_t mutex; // semaphore
    struct Process history[100];
};

int shm_fd, sch_pid;
struct hist *p_table;

void start_time(struct timeval *start)
{
    gettimeofday(start, 0);
}

unsigned long end_time(struct timeval *start)
{
    struct timeval end;
    unsigned long t;

    gettimeofday(&end, 0);
    t = ((end.tv_sec / 1000) + end.tv_usec*1000) - ((start->tv_sec / 1000) + start->tv_usec*1000);
    return t;
}
//prints add to history -- -1 if pid is not valid
void print_history()
{
    if (sem_wait(&p_table->mutex) == -1)
    {
        perror("sem_wait");
        exit(1);
    }
    if (p_table->history_count > 0)
    {
        printf("\nCommand History:\n");
        for (int i = 0; i < p_table->history_count; i++)
        {
            printf("Command %d: %s\n", i + 1, p_table->history[i].command);
            printf("PID: %d\n", p_table->history[i].pid);
            printf("Execution Time: %ld\n", p_table->history[i].exe_time);
            printf("Waiting time: %ld \n", p_table->history[i].wait_time);
            printf("\n");
        }
    }
    if (sem_post(&p_table->mutex) == -1)
    {
        perror("sem_post");
        exit(1);
    }
}
// handler sigint and sigchld sigint prints print history, sigchld updates history
static void my_handler(int signum, siginfo_t *info, void *context)
{
    if (signum == SIGINT)
    {
        printf("\nCaught SIGINT signal for termination\n");
        if (kill(sch_pid, SIGINT) == -1)
        {
            perror("kill");
            exit(1);
        }
        printf("Exiting simple shell...\n");
        print_history();

        if (sem_destroy(&p_table->mutex) == -1)
        {
            perror("shm_destroy");
            exit(1);
        }
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
        if (shm_unlink("shm") == -1)
        {
            perror("shm_unlink");
            exit(1);
        }
        exit(0);
    }
    else if (signum == SIGCHLD)
    {
        pid_t cur_pid = info->si_pid;
        if (sch_pid == cur_pid)
        {
            return;
        }
        sem_wait(&p_table->mutex);
        for (int i = 0; i < p_table->history_count; i++)
        {
            if (p_table->history[i].pid == cur_pid && !p_table->history[i].completed)
            {
                // printf("Process %s with PID %d completed\n", p_table->history[i].command, cur_pid);
                p_table->history[i].exe_time += p_table->time_slice;
                p_table->history[i].completed = true;
                break;
            }
        }
        sem_post(&p_table->mutex);
    }
}
// piping cmds
int forker(char *command, int command_fd, int output_fd)
{
    int stat = fork();
    if (stat < 0)
    {
        printf("fork() failed.\n");
        exit(1);
    }
    else if (stat == 0)
    {
        // child process
        if (command_fd != STDIN_FILENO)
        {
            if (dup2(command_fd, STDIN_FILENO) == -1)
            {
                perror("dup2");
                exit(1);
            }
            if (close(command_fd) == -1)
            {
                perror("close");
                exit(1);
            }
        }
        if (output_fd != STDOUT_FILENO)
        {
            if (dup2(output_fd, STDOUT_FILENO) == -1)
            {
                perror("dup2");
                exit(1);
            }
            if (close(output_fd) == -1)
            {
                perror("close");
                exit(1);
            }
        }

        char *c[11]; 
        int argument_count = 0;
        char *token = strtok(command, " ");
        while (token != NULL)
        {
            c[argument_count++] = token;
            token = strtok(NULL, " ");
        }
        c[argument_count] = NULL;
        if (execvp(c[0], c) == -1)
        {
            perror("execvp");
            printf("invalid command.\n");
            exit(1);
        }
        exit(0);
    }
    else
    {
        return stat;
    }
}

// checking for the pipe and & 
int pipes(char *command)
{
    // separate pipe commands 
    int command_count = 0;
    char *commands[100];
    char *token = strtok(command, "|");
    while (token != NULL)
    {
        commands[command_count++] = token;
        token = strtok(NULL, "|");
    }
   
    int i, before_c = STDIN_FILENO;
    int pipes[2], child_pids[command_count];
    for (i = 0; i < command_count - 1; i++)
    {
        if (pipe(pipes) == -1)
        {
            perror("pipe");
            exit(1);
        }

        if ((child_pids[i] = forker(commands[i], before_c, pipes[1])) < 0)
        {
            perror("forker");
            exit(1);
        }

        if (close(pipes[1]) == -1)
        {
            perror("close");
            exit(1);
        }
        before_c = pipes[0];
    }

    bool bkg_proc = 0;
    if (commands[i][strlen(commands[i]) - 1] == '&')
    {
        commands[i][strlen(commands[i]) - 1] = '\0';
        bkg_proc = 1;
    }
    if ((child_pids[i] = forker(commands[i], before_c, STDOUT_FILENO)) < 0)
    {
        perror("forker");
        exit(1);
    }

    if (sem_wait(&p_table->mutex) == -1)
    {
        perror("sem_wait");
        exit(1);
    }
    p_table->history[p_table->history_count].pid = child_pids[i];
    if (sem_post(&p_table->mutex) == -1)
    {
        perror("sem_post");
        exit(1);
    }
    if (!bkg_proc)
    {
        for (i = 0; i < command_count; i++)
        {
            int ret;
            int pid = waitpid(child_pids[i], &ret, 0);
            if (pid < 0)
            {
                perror("waitpid");
                exit(1);
            }
            if (!WIFEXITED(ret))
            {
                printf("Abnormal %d\n", pid);
            }
        }
    }
    else
    {
        printf("%d %s\n", child_pids[command_count - 1], command);
    }
}
// main function for submit command- executes the command and stops the child process immediately
int submit_process(char *command)
{
    int priority, stat;
    char *arguments[11]; 
    int argument_count = 0;
    char *token = strtok(command, " ");
    token = strtok(NULL, " ");
    while (token != NULL)
    {
        arguments[argument_count++] = token;
        token = strtok(NULL, " ");
    }
    if (argument_count > 1)
    {
        priority = atoi(arguments[--argument_count]);
        if (priority < 1 || priority > 4)
        {
            printf("invalid input for submit command");
            p_table->history[p_table->history_count].completed = true;
            return -1;
        }
        p_table->history[p_table->history_count].priority = priority;
    }
    arguments[argument_count] = NULL;

    stat = fork();
    if (stat < 0)
    {
        printf("fork() failed.\n");
        exit(1);
    }
    else if (stat == 0)
    {
        if (execvp(arguments[0], arguments) == -1)
        {
            perror("execvp");
            printf("Not a valid/supported command.\n");
            exit(1);
        }
        exit(0);
    }
    else
    {
        if (kill(stat, SIGSTOP) == -1)
        {
            perror("kill");
            exit(1);
        }
        return stat;
    }
}
// here we r checking for commands 
int launch(char *command)
{

    if (strncmp(command, "submit", 6) == 0)
    {
        if (sem_wait(&p_table->mutex) == -1)
        {
            perror("sem_wait");
            exit(1);
        }
        p_table->history[p_table->history_count].submit = true;
        p_table->history[p_table->history_count].completed = false;
        p_table->history[p_table->history_count].priority = 1;
        p_table->history[p_table->history_count].in_queue = false;
        p_table->history[p_table->history_count].pid = submit_process(command);
        start_time(&p_table->history[p_table->history_count].start);
        if (sem_post(&p_table->mutex) == -1)
        {
            perror("sem_post");
            exit(1);
        }
        return 1;
    }

    if (strcmp(command, "history") == 0)
    {
        if (sem_wait(&p_table->mutex) == -1)
        {
            perror("sem_wait");
            exit(1);
        }
        for (int i = 0; i < p_table->history_count + 1; i++)
        {
            printf("%s\n", p_table->history[i].command);
        }
        if (sem_post(&p_table->mutex) == -1)
        {
            perror("sem_post");
            exit(1);
        }
        return 1;
    }

    if (strcmp(command, "") == 0)
    {
        p_table->history_count--;
        return 1;
    }
    if (strcmp(command, "exit") == 0)
    {
        return 0;
    }

    int stat;
    stat = pipes(command);
    return stat;
}

// setting n creating a new shared memory object using "shm_open"

struct hist *setup()
{
    
    shm_fd = shm_open("shm", O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1)
    {
        perror("shm_open");
        exit(1);
    }
    if (ftruncate(shm_fd, sizeof(struct hist)) == -1)
    {
        perror("ftruncate");
        exit(1);
    }
    p_table = mmap(NULL, sizeof(struct hist), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (p_table == MAP_FAILED)
    {
        perror("mmap");
        exit(1);
    }
}

void sig_handler()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = my_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1)
    {
        perror("Error setting up SIGINT handler");
        exit(1);
    }

    if (sigaction(SIGCHLD, &sa, NULL) == -1)
    {
        perror("Error setting up SIGCHLD handler");
        exit(1);
    }
}
void update_ptable(struct hist *p_table, char *cmd)
{
    if (sem_wait(&p_table->mutex) == -1)
    {
        perror("sem_wait3");
        exit(1);
    }
    strcpy(p_table->history[p_table->history_count].command, cmd);
    p_table->history[p_table->history_count].pid = -1;
    p_table->history[p_table->history_count].submit = false;
    p_table->history[p_table->history_count].wait_time = 0;
    p_table->history[p_table->history_count].exe_time = 0;
    start_time(&p_table->history[p_table->history_count].start);
    if (sem_post(&p_table->mutex) == -1)
    {
        perror("sem_post");
        exit(1);
    }
}

// main function runs in a while do loop 
int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("invalid command");
        exit(1);
    }
    p_table = setup();

    p_table->history_count = 0;
    int cpu = atoi(argv[1]);
    if (cpu == 0)
    {
        printf("number of cpus must be > 0\n");
        exit(1);
    }
    p_table->ncpu = cpu;
    int t = atoi(argv[2]);
    if (t == 0)
    {
        printf("time slice must be > 0 \n");
        exit(1);
    }
    p_table->time_slice = t;
    if (sem_init(&p_table->mutex, 1, 1) == -1)
    {
        perror("sem_init");
        exit(1);
    }

    //printf("Initializing scheduler\n");
    pid_t pid;
    if ((pid = fork()) < 0)
    {
        printf("fork() failed.\n");
        exit(1);
    }
    if (pid == 0)
    {
        if (execvp("./scheduler", ("./scheduler", NULL)) == -1)
        {
            printf("error scheduler\n");
            exit(1);
        }
        if (munmap(p_table, sizeof(struct hist)) < 0)
        {
            printf("Error in unmapping\n");
            exit(1);
        }
        if (close(shm_fd) == -1)
        {
            perror("close");
            exit(1);
        }
        exit(0);
    }
    else
    {
        sch_pid = pid;
    }

    sig_handler();
    char current_dir[100];

    printf("---------Initializing shell and scheduling-----\n");
    int stat;
    do
    {
        getcwd(current_dir, sizeof(current_dir));
        printf(">%s>> ", current_dir);

        char *cmd;
        cmd = (char *)malloc(100);
        if (fgets(cmd, 100, stdin) == NULL)
        {
            perror("fgets");
            exit(1);
        }
        if (strlen(cmd) > 0 && cmd[strlen(cmd) - 1] == '\n')
        {
            cmd[strlen(cmd) - 1] = '\0';
        }
        if (strncmp(cmd, "exit", 4) == 0)
        {
            free(cmd);
            break;
        }

        update_ptable(p_table, cmd);
        stat = launch(cmd);
        if (!p_table->history[p_table->history_count].submit)
        {
            p_table->history[p_table->history_count].exe_time = end_time(&p_table->history[p_table->history_count].start);
        }
        p_table->history_count++;
        if (sem_post(&p_table->mutex) == -1)
        {
            perror("sem_post");
            exit(1);
        }

    } while (stat);

    printf("terminating shell...\n");

    print_history();
    // destroying the semaphore and unlinking
    if (sem_destroy(&p_table->mutex) == -1)
    {
        perror("shm_destroy");
        exit(1);
    }
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
    if (shm_unlink("shm") == -1)
    {
        perror("shm_unlink");
        exit(1);
    }
    return 0;
}
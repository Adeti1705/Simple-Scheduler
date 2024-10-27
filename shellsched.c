#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/mman.h>

char history[100][100];
int pid_history[100];
long time_history[100][2];
int c_hist = 0;

struct Process
{
    int pid, priority;
    bool submit, queue, completed;
    char command[100];
    struct timeval start;
    unsigned long execution_time, wait_time, vruntime;
};

struct history_struct
{
    int history_count, ncpu, tslice;
    sem_t mutex;
    struct Process history[100];
};
int shm_fd, sch_pid;
struct history_struct *p_table;

void start_time(struct timeval *start){
    gettimeofday(start, 0);
}

unsigned long end_time(struct timeval *start){
    struct timeval end;
    unsigned long t;

    gettimeofday(&end, 0);
    t = ((end.tv_sec*1000000) + end.tv_usec) - ((start->tv_sec*1000000) + start->tv_usec);
    return t/1000;
}

// to add a command to history
// void add_to_history(const char *command, int pid, long start_time_ms, long end_time_ms)
// {
//     if (c_hist < 100)
//     {
//         strcpy(history[c_hist], command);
//         pid_history[c_hist] = pid;
//         time_history[c_hist][0] = start_time_ms;
//         time_history[c_hist][1] = end_time_ms;
//         c_hist++;
//     }
//     else
//     {
//         printf("Can't add more than 100 commands to history.\n");
//     }
// }

// to print command history
void print_history()
{
    printf("\nCommand History:\n");
    for (int i = 0; i < p_table->history_count; i++)
    {
        printf("Command %d: %s\n", i + 1, p_table->history[i].command);
        printf("PID: %d\n", p_table->history[i].pid);
        printf("Execution Time: %ld\n", p_table->history[i].execution_time);
        printf("Waiting time: %ld \n", p_table->history[i].wait_time);
        printf("\n");
    }
}

void my_handler(int signum, siginfo_t *info, void *ptr) {
    if (signum == SIGINT) {
        printf("Terminating scheduler and shell\n");
        if (kill(sch_pid, SIGINT) == -1) {
            printf("Error terminating scheduler\n");
            exit(1);
        }

        sem_wait(&p_table->mutex);
        print_history();
        sem_post(&p_table->mutex);

        if (sem_destroy(&p_table->mutex) == -1) {
            printf("Error destroying semaphore\n");
            exit(1);
        }
        if (munmap(p_table, sizeof(struct history_struct)) < 0) {
            printf("Error unmapping shared memory\n");
            exit(1);
        }
        if (close(shm_fd) == -1) {
            printf("Error closing shared memory\n");
            exit(1);
        }
        if (shm_unlink("shm") == -1) {
            printf("Error unlinking shared memory\n");
            exit(1);
        }
    } else if (signum == SIGCHLD) {
        pid_t cur_pid = info->si_pid;
        if (sch_pid == cur_pid) {
            return;
        }
        sem_wait(&p_table->mutex);
        for (int i = 0; i < p_table->history_count; i++) {
            if (p_table->history[i].pid == cur_pid) {
                p_table->history[i].execution_time += end_time(&p_table->history[i].start);
                p_table->history[i].completed = true;
                break;
            }
        }
        sem_post(&p_table->mutex);
    }
}

void sig_handler() {
    struct sigaction sig;
    memset(&sig, 0, sizeof(sig));
    sig.sa_sigaction = my_handler;
    sig.sa_flags = SA_SIGINFO | SA_NOCLDSTOP | SA_RESTART;
    if (sigaction(SIGINT, &sig, NULL) != 0) {
        printf("Signal handling failed.\n");
        exit(1);
    }
    if (sigaction(SIGCHLD, &sig, NULL) != 0) {
        printf("Signal handling failed.\n");
        exit(1);
    }
}

// returns current time in microseconds(us)
long current_time()
{
    struct timeval t;
    if (gettimeofday(&t, NULL) != 0)
    {
        printf("Error in getting the time.\n");
        exit(1);
    }
    long epoch_microsec = t.tv_sec * 1000000;
    return epoch_microsec + t.tv_usec;
}
// executes commands with pipes
int pipe_execute(char ***commands)
{
    int inputfd = STDIN_FILENO;
    int lastChildPID = -1;
    int i = 0;

    while (commands[i] != NULL)
    {
        int pipefd[2];
        if (pipe(pipefd) == -1)
        {
            perror("Pipe failed");
            exit(1);
        }

        int pid = fork();
        if (pid < 0)
        {
            perror("Fork failed");
            exit(EXIT_FAILURE);
        }
        else if (pid == 0)
        {
            // Child process
            if (inputfd != STDIN_FILENO)
            {
                dup2(inputfd, STDIN_FILENO);
                close(inputfd);
            }
            if (commands[i + 1] != NULL)
            {
                dup2(pipefd[1], STDOUT_FILENO);
            }
            close(pipefd[0]);
            close(pipefd[1]);
            execvp(commands[i][0], commands[i]);
            perror("execvp failed");
            exit(EXIT_FAILURE);
        }
        else
        {
            // Update process table with pid for the current piped command
            if (sem_wait(&p_table->mutex) == -1)
            {
                perror("sem_wait");
                exit(1);
            }
            p_table->history[p_table->history_count].pid = pid;
            p_table->history[p_table->history_count].completed = false;
            if (sem_post(&p_table->mutex) == -1)
            {
                perror("sem_post");
                exit(1);
            }

            close(pipefd[1]);
            if (inputfd != STDIN_FILENO)
            {
                close(inputfd);
            }
            inputfd = pipefd[0];
            lastChildPID = pid;
            i++;
        }
    }

    int status;
    while (wait(&status) > 0)
    {
    }
    return lastChildPID;
}

int launch2(char **command_line, bool background)
{
    int pid = fork();
    if (pid < 0)
    {
        printf("Fork failed.\n");
        return -1;
    }
    else if (pid == 0)
    {
        if (strcmp(command_line[0], "history") == 0)
        {
            print_history();
            exit(0);
        }
        execvp(command_line[0], command_line);
        printf("Command not found: %s\n", command_line[0]);
        exit(1);
    }
    else
    {
        // Updating global array with pid in process table (for non-piped processes)
        if (sem_wait(&p_table->mutex) == -1)
        {
            perror("sem_wait");
            exit(1);
        }
        p_table->history[p_table->history_count].pid = pid;
        p_table->history[p_table->history_count].completed = false; // Setting process as incomplete initially
        if (sem_post(&p_table->mutex) == -1)
        {
            perror("sem_post");
            exit(1);
        }

        if (!background)
        {
            // Parent waits for non-& commands
            int status;
            waitpid(pid, &status, 0);
        }
        else
        {
            printf("Started background process with PID: %d\n", pid);
        }
    }
    return pid;
}

// parses command string into array of strings
char **break_delim(char *cmd_line, char *delim)
{
    char **word_array = (char **)malloc(100 * sizeof(char *));
    if (word_array == NULL)
    {
        printf("Error in allocating memory for command.\n");
        exit(1);
    }
    char *word = strtok(cmd_line, delim);
    int i = 0;
    while (word != NULL)
    {
        word_array[i] = word;
        i++;
        word = strtok(NULL, delim);
    }
    word_array[i] = NULL;
    return word_array;
}


// to split commands into array for each pipe segment
char ***pipe_manager(char **cmds)
{
    char ***commands = (char ***)malloc(sizeof(char **) * 100);
    if (commands == NULL)
    {
        printf("Failed to allocate memory\n");
        exit(1);
    }

    int j = 0;
    for (int i = 0; cmds[i] != NULL; i++)
    {
        commands[j] = break_delim(cmds[i], " \n");
        j++;
    }
    commands[j] = NULL;
    return commands;
}

int submit_process(char *command, char *priority)
{
    // checking if priority is specified
    int priority_int = atoi(priority);
    if (priority_int < 1 || priority_int > 4)
    {
        printf("invalid input for submit command");
        p_table->history[p_table->history_count].completed = true;
        return -1;
    }
    p_table->history[p_table->history_count].priority = priority_int;

    char *exec_command[2] = {command, NULL};

    int status;
    status = fork();
    if (status < 0)
    {
        printf("fork() failed.\n");
        exit(1);
    }
    else if (status == 0)
    {
        // child process
        if (execvp(exec_command[0], exec_command) == -1)
        {
            perror("execvp");
            printf("Not a valid/supported command.\n");
            exit(1);
        }
        exit(0);
    }
    else
    {
        // parent process returns pid and stops child
        if (kill(status, SIGSTOP) == -1)
        {
            perror("kill");
            exit(1);
        }
        return status;
    }
}


// checks if command has pipes
bool hasPipes(char *str)
{
    for (int i = 0; str[i] != '\0'; i++)
    {
        if (str[i] == '|')
        {
            return true;
        }
    }
    return false;
}


int shell_proc(char *cmd)
{
    bool background = false;
    size_t len = strlen(cmd);
    if (len > 0 && cmd[len - 2] == '&')
    {
        background = true;
        cmd[len - 2] = '\0';
    }
    char *cmd_copy = strdup(cmd);
    int pid=-1;
    if (hasPipes(cmd))
    {
        char **command_1 = break_delim(cmd, "|");
        char ***command_2 = pipe_manager(command_1);
        pid = pipe_execute(command_2); // pipe execution not yet supporting background
    }
    else
    {
        char **command = break_delim(cmd, " \n");
        pid = launch2(command, background); // pass the background flag
    }

    // only add to history for foreground processes
    // if (!background)
    // {
    //     add_to_history(cmd_copy, pid, start_time, current_time());
    // }
    return pid;
}

// executes command
int launch(char *command_line, bool background)
{
    int pid = fork();
    if (pid < 0)
    {
        printf("Fork failed.\n");
        return -1;
    }
    else if (pid == 0)
    {
        if (strncmp(command_line, "history",7) == 0)
        {
            print_history();
            exit(0);
        }
        if (strncmp(command_line, "submit",6) == 0)
        {
            if (sem_wait(&p_table->mutex) == -1)
            {
                perror("sem_wait");
                exit(1);
            }
            p_table->history[p_table->history_count].submit = true;
            p_table->history[p_table->history_count].completed = false;
            p_table->history[p_table->history_count].priority = 1;
            p_table->history[p_table->history_count].queue = false;
            start_time(&p_table->history[p_table->history_count].start);
            char** command=break_delim(command_line, " ");
            int command_count = 0;
            while (command[command_count] != NULL)
            {
                command_count++;
            }

            if (command_count == 3)
            {
                p_table->history[p_table->history_count].pid = submit_process(command[1], command[2]);
            }
            else
            {
                p_table->history[p_table->history_count].pid = submit_process(command[1], "1"); // defualt priority
            }

            start_time(&p_table->history[p_table->history_count].start);
            if (sem_post(&p_table->mutex) == -1)
            {
                perror("sem_post");
                exit(1);
            }
            return 1;
        }
        if (strncmp(command_line, "exit",4) == 0)
        {
            return 0;
        }

        int status;
        status = shell_proc(command_line);
        return status;
    }
    else
    {
        if (!background)
        {
            // parent waits for non-& commands
            int status;
            waitpid(pid, &status, 0);
        }
        else
        {
            printf("Started background process with PID: %d\n", pid);
        }
    }
    return pid;
}







struct history_struct *setup()
{
    // Unlink any previous shared memory
    shm_unlink("shm");

    shm_fd = shm_open("shm", O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1)
    {
        perror("shm_open");
        exit(1);
    }

    // Set desired size for shared memory segment
    if (ftruncate(shm_fd, sizeof(struct history_struct)) == -1)
    {
        perror("ftruncate");
        exit(1);
    }

    // Map the shared memory into process's address space
    p_table = mmap(NULL, sizeof(struct history_struct), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (p_table == MAP_FAILED)
    {
        perror("mmap");
        exit(1);
    }

    // Initialize semaphore
    if (sem_init(&p_table->mutex, 1, 1) == -1)
    {
        perror("sem_init");
        exit(1);
    }
    return p_table;
}

void update_ptable(struct history_struct *p_table)
{
    if (sem_wait(&p_table->mutex) == -1)
    {
        perror("sem_wait");
        exit(1);
    }

    p_table->history[p_table->history_count].pid = -1;
    p_table->history[p_table->history_count].submit = false;
    p_table->history[p_table->history_count].wait_time = 0;
    p_table->history[p_table->history_count].execution_time = 0;
    p_table->history[p_table->history_count].vruntime = 0;
    start_time(&p_table->history[p_table->history_count].start);


    if (sem_post(&p_table->mutex) == -1)
    {
        perror("sem_post");
        exit(1);
    }
}


// main shell loop
int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("invalid input parameters");
        exit(1);
    }
    p_table = setup();

    p_table->history_count = 0;
    if (atoi(argv[1]) == 0)
    {
        printf("invalid argument for number of CPU");
        exit(1);
    }
    p_table->ncpu = atoi(argv[1]);

    if (atoi(argv[2]) == 0)
    {
        printf("invalid argument for time quantum");
        exit(1);
    }
    p_table->tslice = atoi(argv[2]);
    printf("Forking child proc for scheduler\n");
    int stat = fork();
    if (stat < 0)
    {
        printf("Forking failed");
        exit(1);
    }
    if (stat == 0)
    {
        execvp("./scheduler", ("./scheduler", NULL));
        printf("execvp failed");
        exit(1);
        if (munmap(p_table, sizeof(struct history_struct)) < 0)
        {
            printf("Error unmapping\n");
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
        sch_pid = stat;
    }
    sig_handler(); // handles signal
    char *cmd;
    char current_dir[100];

    printf("\n Shell Starting...----------------------------------\n");
    while (1)
    {
        getcwd(current_dir, sizeof(current_dir)); // gets current directory
        printf(">%s>>> ", current_dir);
        cmd = (char *)malloc(100);
        fgets(cmd, 100, stdin);

        if (strlen(cmd) > 0 && cmd[strlen(cmd) - 1] == '\n')
        {
            cmd[strlen(cmd) - 1] = '\0';
        }

        if (sem_wait(&p_table->mutex) == -1)
        {
            perror("sem_wait");
            exit(1);
        }

        strcpy(p_table->history[p_table->history_count].command, cmd);

        if (sem_post(&p_table->mutex) == -1)
        {
            perror("sem_post");
            exit(1);
        }

        update_ptable(p_table);
        bool background = false;
        launch(cmd, background);
        free(cmd);
    }
    return 0;
}

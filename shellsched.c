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
#include <errno.h>


struct Process {
    int pid, priority;
    bool submit, queue, completed;
    char command[100];
    struct timeval start;
    int time_slice_count;
    bool in_queue; 
    unsigned long execution_time, wait_time, vruntime;
    int arrival_time;
};
struct history_struct {
    int history_count, ncpu, tslice;
    sem_t mutex;
    struct Process history[100];
};

int shm_fd, sch_pid;
struct history_struct *p_table;

void start_time(struct timeval *start) {
    gettimeofday(start, 0);
}

// unsigned long end_time(struct timeval *start) {
//     struct timeval end;
//     unsigned long t;

//     gettimeofday(&end, 0);
//     t = ((end.tv_sec * 1000000) + end.tv_usec) - ((start->tv_sec * 1000000) + start->tv_usec);
//     return t / 1000;
// }

void print_history() {
    printf("\nCommand History:\n");
    for (int i = 0; i < p_table->history_count; i++) {
        printf("Command %d: %s\n", i + 1, p_table->history[i].command);
        printf("PID: %d\n", p_table->history[i].pid);
        printf("Execution Time: %ld\n", p_table->history[i].execution_time);
        printf("Waiting time: %ld \n", p_table->history[i].wait_time);
        printf("\n");
    }
}

void my_handler(int signum, siginfo_t *info, void *ptr) {
    if (signum == SIGINT) {
        print_history();
        exit(0);
    } else if (signum == SIGCHLD) {
        pid_t cur_pid = info->si_pid;
        if (sch_pid == cur_pid) {
            return;
        }
        sem_wait(&p_table->mutex);
        for (int i = 0; i < p_table->history_count; i++) {
            if (p_table->history[i].pid == cur_pid && !p_table->history[i].completed) {
                // struct timeval end;
                // gettimeofday(&end, NULL);
                p_table->history[i].execution_time = p_table->history[i].time_slice_count * p_table->tslice;
               // p_table->history[i].execution_time = (end.tv_sec - p_table->history[i].start.tv_sec) * 1000 +(end.tv_usec - p_table->history[i].start.tv_usec) / 1000;
                p_table->history[i].completed = true;
                break;
            }
        }
        sem_post(&p_table->mutex);
    }
}

void sig_handler() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = my_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Error setting up SIGINT handler");
        exit(1);
    }

    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("Error setting up SIGCHLD handler");
        exit(1);
    }
}

int pipe_execute(char ***commands) {
    int inputfd = STDIN_FILENO;
    int lastChildPID = -1;
    int i = 0;

    while (commands[i] != NULL) {
        int pipefd[2];
        if (pipe(pipefd) == -1) {
            perror("Pipe failed");
            exit(1);
        }

        int pid = fork();
        if (pid < 0) {
            perror("Fork failed");
            exit(EXIT_FAILURE);
        } else if (pid == 0) {
            signal(SIGINT, SIG_DFL);  // Reset SIGINT handler to default in child
            if (inputfd != STDIN_FILENO) {
                dup2(inputfd, STDIN_FILENO);
                close(inputfd);
            }
            if (commands[i + 1] != NULL) {
                dup2(pipefd[1], STDOUT_FILENO);
            }
            close(pipefd[0]);
            close(pipefd[1]);
            execvp(commands[i][0], commands[i]);
            perror("execvp failed");
            exit(EXIT_FAILURE);
        } else {
            close(pipefd[1]);
            if (inputfd != STDIN_FILENO) {
                close(inputfd);
            }
            inputfd = pipefd[0];
            lastChildPID = pid;
            i++;
        }
    }

    int status;
    while (wait(&status) > 0) {
    }
    return lastChildPID;
}

int launch2(char **command_line, bool background) {
    int pid = fork();
    if (pid < 0) {
        printf("Fork failed.\n");
        return -1;
    } else if (pid == 0) {
        signal(SIGINT, SIG_DFL);  // Reset SIGINT handler to default in child
        if (strcmp(command_line[0], "history") == 0) {
            print_history();
            exit(0);
        }
        execvp(command_line[0], command_line);
        printf("Command not found: %s\n", command_line[0]);
        exit(1);
    } else {
        if (!background) {
            int status;
            waitpid(pid, &status, 0);
        } else {
            printf("Started background process with PID: %d\n", pid);
        }
    }
    return pid;
}

char **break_delim(char *cmd_line, char *delim) {
    char **word_array = (char **)malloc(100 * sizeof(char *));
    if (word_array == NULL) {
        printf("Error in allocating memory for command.\n");
        exit(1);
    }
    char *word = strtok(cmd_line, delim);
    int i = 0;
    while (word != NULL) {
        word_array[i] = word;
        i++;
        word = strtok(NULL, delim);
    }
    word_array[i] = NULL;
    return word_array;
}

char ***pipe_manager(char **cmds) {
    char ***commands = (char ***)malloc(sizeof(char **) * 100);
    if (commands == NULL) {
        printf("Failed to allocate memory\n");
        exit(1);
    }

    int j = 0;
    for (int i = 0; cmds[i] != NULL; i++) {
        commands[j] = break_delim(cmds[i], " \n");
        j++;
    }
    commands[j] = NULL;
    return commands;
}
int submit_process(char *command, char *priority) {
    int priority_int = atoi(priority);
    if (priority_int < 1 || priority_int > 4) {
        printf("Invalid input for submit command\n");
        return -1;
    }

    // Create a new Process structure
    struct Process new_process;
    strcpy(new_process.command, command);
    new_process.priority = priority_int;
    // new_process.pid = -1;  // Will be set when actually executed
    // new_process.submit = true;
    // new_process.completed = false;
    // new_process.queue = false;
    // new_process.execution_time = 0;
    // new_process.wait_time = 0;
    // new_process.arrival_time = time(NULL);  // Current time

     new_process.pid = fork();
    if (new_process.pid == 0) {
        // Child process
        char *args[] = {command, NULL};
        execvp(command, args);
        perror("execvp");
        exit(1);
    } else if (new_process.pid > 0) {
        // Parent process
        new_process.submit = true;
        new_process.completed = false;
        new_process.queue = false;
        new_process.in_queue = false;
        new_process.execution_time = 0;
        new_process.wait_time = 0;
        new_process.arrival_time = time(NULL);

        if (sem_wait(&p_table->mutex) == -1) {
            perror("sem_wait");
            return -1;
        }

        if (p_table->history_count < 100) {
            p_table->history[p_table->history_count] = new_process;
            p_table->history_count++;
            printf("Process submitted successfully. PID: %d\n", new_process.pid);
        } else {
            printf("Process queue is full. Cannot submit more processes.\n");
            kill(new_process.pid, SIGKILL);
        }

        if (sem_post(&p_table->mutex) == -1) {
            perror("sem_post");
            return -1;
        }

        return new_process.pid;
    } else {
        perror("fork");
        return -1;
    }
}
bool hasPipes(char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '|') {
            return true;
        }
    }
    return false;
}

int shell_proc(char *cmd) {
    bool background = false;
    size_t len = strlen(cmd);
    if (len > 0 && cmd[len - 2] == '&') {
        background = true;
        cmd[len - 2] = '\0';
    }
    
    int pid = -1;
    if (hasPipes(cmd)) {
        char **command_1 = break_delim(cmd, "|");
        char ***command_2 = pipe_manager(command_1);
        pid = pipe_execute(command_2);
    } else {
        char **command = break_delim(cmd, " \n");
        pid = launch2(command, background);
    }

    return pid;
}

void add_to_history(char *cmd, int pid) {
    if (sem_wait(&p_table->mutex) == -1) {
        perror("sem_wait");
        exit(1);
    }
    printf("Adding process %s ", cmd);
    int index = p_table->history_count;
    strcpy(p_table->history[index].command, cmd);
    p_table->history[index].pid = pid;
    p_table->history[index].completed = false;
    p_table->history[index].submit = strncmp(cmd, "submit", 6) == 0;
    // p_table->history[index].execution_time = 0;
    // p_table->history[index].wait_time = 0;
    p_table->history[index].time_slice_count = 0;
    p_table->history[index].in_queue = false;
    gettimeofday(&p_table->history[index].start, NULL);
    if (sem_post(&p_table->mutex) == -1) {
        perror("sem_post");
        exit(1);
    }

}

int launch(char *command_line, bool background) {
    int pid = fork();
    if (pid < 0) {
        printf("Fork failed.\n");
        return -1;
    } else if (pid == 0) {
        //signal(SIGINT, SIG_DFL);  // Reset SIGINT handler to default in child
        if (strncmp(command_line, "history", 7) == 0) {
            print_history();
            exit(0);                       //terminates child               
        }
        if (strncmp(command_line, "submit", 6) == 0) {
            char **command = break_delim(command_line, " ");
            int command_count = 0;
            while (command[command_count] != NULL) {
                command_count++;
            }

            int submit_pid;
            if (command_count == 3) {
                submit_pid = submit_process(command[1], command[2]);
            } else {
                submit_pid = submit_process(command[1], "1");
            }
            exit(submit_pid);
        }

        int status;
        status = shell_proc(command_line);
        exit(status);
    } else {
        //add_to_history(command_line, pid);  // Add all commands to history

        if (!background) {
            int st;
            waitpid(pid, &st, 0);
        } else {
            printf("Started background process with PID: %d\n", pid);
        }
    }
    printf("returing from launch\n");
    return pid;
}

struct history_struct *setup() {
    shm_fd = shm_open("shm", O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        if (errno == EEXIST) {
            shm_fd = shm_open("shm", O_RDWR, 0666);
            if (shm_fd == -1) {
                perror("shm_open");
                exit(1);
            }
        } else {
            perror("shm_open");
            exit(1);
        }
    }

    if (ftruncate(shm_fd, sizeof(struct history_struct)) == -1) {
        perror("ftruncate");
        exit(1);
    }

    p_table = mmap(NULL, sizeof(struct history_struct), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (p_table == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    if (sem_init(&p_table->mutex, 1, 1) == -1) {
        perror("sem_init");
        exit(1);
    }
    return p_table;
}

void update_ptable(struct history_struct *p_table) {
    if (sem_wait(&p_table->mutex) == -1) {
        perror("sem_wait");
        exit(1);
    }
    p_table->history[p_table->history_count].time_slice_count = 0;
    p_table->history[p_table->history_count].pid = -1;
    p_table->history[p_table->history_count].submit = false;
    p_table->history[p_table->history_count].wait_time = 0;
    p_table->history[p_table->history_count].execution_time = 0;
    p_table->history[p_table->history_count].vruntime = 0;
    start_time(&p_table->history[p_table->history_count].start);
    //p_table->history_count++;

    if (sem_post(&p_table->mutex) == -1) {
        perror("sem_post");
        exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("invalid input parameters");
        exit(1);
    }
    p_table = setup();

    p_table->history_count = 0;
    if (atoi(argv[1]) == 0) {
        printf("invalid argument for number of CPU");
        exit(1);
    }
    p_table->ncpu = atoi(argv[1]);

    if (atoi(argv[2]) == 0) {
        printf("invalid argument for time quantum");
        exit(1);
    }
    p_table->tslice = atoi(argv[2]);
    
    sig_handler();  // Set up signal handlers

    printf("Forking child proc for scheduler\n");
    int stat = fork();
    if (stat < 0) {
        perror("Forking failed");
        exit(1);
    }
    if (stat == 0) {
        char *scheduler_args[] = {"./scheduler", NULL};
        execv("./scheduler", scheduler_args);
        perror("execv failed");
        exit(1);
    } else {
        sch_pid = stat;
    }
    char *cmd;
    char current_dir[100];

    printf("\n Shell Starting...----------------------------------\n");
    while (1) {
        getcwd(current_dir, sizeof(current_dir));
        printf(">%s>>> ", current_dir);
        cmd = (char *)malloc(100);
        if (cmd == NULL) {
            perror("Memory allocation failed");
            exit(1);
        }

        if (fgets(cmd, 100, stdin) == NULL) {
            if (feof(stdin)) {
                printf("\nEnd of input. Exiting...\n");
                free(cmd);
                break;
            } else {
                perror("Error reading input");
                free(cmd);
                continue;
            }
        }

        if (strlen(cmd) > 0 && cmd[strlen(cmd) - 1] == '\n') {
            cmd[strlen(cmd) - 1] = '\0';
        }

        if (strlen(cmd) == 0) {
            free(cmd);
            continue;
        }

        if (strcmp(cmd, "exit") == 0) {
            free(cmd);
            break;
        }

        update_ptable(p_table);
        bool background = false;
        //launch(cmd, background);
        int pid = launch(cmd, background);
        add_to_history(cmd, pid);
        free(cmd);
    }

    // Cleanup code
    if (kill(sch_pid, SIGINT) == -1) {
        perror("Error terminating scheduler");
    }

    sem_wait(&p_table->mutex);
    print_history();
    sem_post(&p_table->mutex);

    if (sem_destroy(&p_table->mutex) == -1) {
        perror("Error destroying semaphore");
    }
    if (munmap(p_table, sizeof(struct history_struct)) < 0) {
        perror("Error unmapping shared memory");
    }
    if (close(shm_fd) == -1) {
        perror("Error closing shared memory");
    }
    if (shm_unlink("shm") == -1) {
        if (errno != ENOENT) {
            perror("Error unlinking shared memory");
        }
    }

    return 0;
}
// the cmds are adding to history but not printing??

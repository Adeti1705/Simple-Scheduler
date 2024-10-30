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
    bool submit, completed;
    char command[100];
    bool running;
    struct timeval start;
    bool in_queue;
    unsigned long execution_time, wait_time;
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

unsigned long end_time(struct timeval *start) {
    struct timeval end;
    unsigned long t;

    gettimeofday(&end, 0);
    t = ((end.tv_sec * 1000000) + end.tv_usec) - ((start->tv_sec * 1000000) + start->tv_usec);
    return t / 1000;
}

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
                printf("Process %s with PID %d completed\n", p_table->history[i].command, cur_pid);
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
            signal(SIGINT, SIG_DFL); // Reset SIGINT handler to default in child
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
        //signal(SIGINT, SIG_DFL); // Reset SIGINT handler to default in child
        // if (strcmp(command_line[0], "history") == 0) {
        //     print_history();
        //     exit(0);
        // }
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
    char ***commands = (char ***)malloc(sizeof(char *) * 100);
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
        perror("sem_wait2");
        exit(1);
    }
    //printf("Adding process %s ", cmd);
    int index = p_table->history_count;
    strcpy(p_table->history[index].command, cmd);
    p_table->history[index].pid = pid;
    p_table->history[index].completed = false;
    p_table->history[index].submit = strncmp(cmd, "submit", 6) == 0;
    p_table->history[index].in_queue = false;
    p_table->history_count++;
    gettimeofday(&p_table->history[index].start, NULL);
    if (sem_post(&p_table->mutex) == -1) {
        perror("sem_post");
        exit(1);
    }
}
int submit_process(char *command, char *priority) {
    printf("Entering submit_process\n");
    fflush(stdout);

    int priority_int = atoi(priority);
    printf("Priority: %d\n", priority_int);
    fflush(stdout);

    if (priority_int < 1 || priority_int > 4) {
        printf("Invalid input for submit command\n");
        fflush(stdout);
        return -1;
    }

    printf("About to acquire semaphore\n");
    fflush(stdout);

    if (sem_wait(&p_table->mutex) == -1) {
        perror("sem_wait1");
        return -1;
    }

    printf("Semaphore acquired\n");
    fflush(stdout);

    if (p_table->history_count >= 100) {
        printf("Process queue is full. Cannot submit more processes.\n");
        sem_post(&p_table->mutex);
        fflush(stdout);
        return -1;
    }

    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
        perror("pipe");
        sem_post(&p_table->mutex);
        return -1;
    }

    printf("About to fork\n");
    fflush(stdout);
    
    int status = fork();
    printf("STATUSSSS:%d\n", status);
    fflush(stdout);

    if (status < 0) {
        perror("fork() failed");
        sem_post(&p_table->mutex);
        return -1;
    } else if (status == 0) {
        // Child process
        close(pipe_fd[0]); // Close read end

        printf("Child process about to exec\n");
        //fflush(stdout);

        // Notify parent that child is ready
        char ready = 'R';
        if (write(pipe_fd[1], &ready, 1) != 1) {
            perror("write to pipe");
            exit(1);
        }
        close(pipe_fd[1]); // Close write end

        char *args[] = {command, NULL};
        execvp(command, args);
        perror("execvp");
        exit(1);
    } else {
        // Parent process
        close(pipe_fd[1]); // Close write end

        // Wait for child to signal readiness
        char ready;
        if (read(pipe_fd[0], &ready, 1) != 1) {
            perror("read from pipe");
            sem_post(&p_table->mutex);
            return -1;
        }
        close(pipe_fd[0]); // Close read end

        printf("Parent process, child PID: %d\n", status);
        // fflush(stdout);

        if (kill(status, SIGSTOP) == -1) {
            perror("kill");
            sem_post(&p_table->mutex);
            return -1;
        }

        printf("Child process stopped by parent\n");
        //fflush(stdout);

        // Record process in history
        p_table->history[p_table->history_count].pid = status;
        p_table->history[p_table->history_count].priority = priority_int;
        printf("Process added to history\n");
        sem_post(&p_table->mutex);

        printf("Semaphore released\n");
        fflush(stdout);

        return status;
    }
}
void launch(char *command_line, bool background) {
    if (strncmp(command_line, "history", 7) == 0) {
        print_history();
        return;
    }
    char* cmd_cpy = strdup(command_line);
    if (strncmp(command_line, "submit", 6) == 0) {
        char **command = break_delim(command_line, " \n");
        int command_count = 0;
        while (command[command_count] != NULL) {
            //printf("command[%d]: %s\n", command_count, command[command_count]);
            command_count++;
        }
        //printf("command[%d]: %s\n", command_count, command[command_count]);
        int submit_pid;
        if (command_count == 3) {
            submit_pid = submit_process(command[1], command[2]);
        } else {
            submit_pid = submit_process(command[1], "1");
        }
        add_to_history(cmd_cpy, submit_pid);
        return;
    }

    int status;
    status = shell_proc(command_line);
    add_to_history(cmd_cpy, status);

    //printf("Returning from launch\n");
    return;
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
        perror("sem_wait3");
        exit(1);
    }
    p_table->history[p_table->history_count].pid = -1;
    p_table->history[p_table->history_count].submit = false;
    p_table->history[p_table->history_count].wait_time = 0;
    p_table->history[p_table->history_count].execution_time = 0;
    //p_table->history[p_table->history_count].vruntime = 0;
    start_time(&p_table->history[p_table->history_count].start);

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

    sig_handler(); // Set up signal handlers

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

        if (strcmp(cmd, "exit") == 0) {
            free(cmd);
            break;
        }
        strcpy(p_table->history[p_table->history_count].command, cmd);
        update_ptable(p_table);
        bool background = false;
        launch(cmd, background);
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
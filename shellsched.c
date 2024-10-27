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

struct Process {
    int pid, priority;
    bool submit, queue, completed;
    char command[100];
    struct timeval start;
    unsigned long execution_time, wait_time, vruntime;
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
    static bool terminating = false; // Prevent multiple terminations
    if (terminating) return;
    terminating = true;

    printf("Caught signal: %d\n", signum); // Debug print
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
        exit(0); // Ensure the program exits
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

long current_time() {
    struct timeval t;
    if (gettimeofday(&t, NULL) != 0) {
        perror("Error in getting the time");
        exit(1);
    }
    long epoch_microsec = t.tv_sec * 1000000;
    return epoch_microsec + t.tv_usec;
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
            if (sem_wait(&p_table->mutex) == -1) {
                perror("sem_wait");
                exit(1);
            }
            p_table->history[p_table->history_count].pid = pid;
            p_table->history[p_table->history_count].completed = false;
            if (sem_post(&p_table->mutex) == -1) {
                perror("sem_post");
                exit(1);
            }

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
        if (strcmp(command_line[0], "history") == 0) {
            print_history();
            exit(0);
        }
        execvp(command_line[0], command_line);
        printf("Command not found: %s\n", command_line[0]);
        exit(1);
    } else {
        if (sem_wait(&p_table->mutex) == -1) {
            perror("sem_wait");
            exit(1);
        }
        p_table->history[p_table->history_count].pid = pid;
        p_table->history[p_table->history_count].completed = false;
        if (sem_post(&p_table->mutex) == -1) {
            perror("sem_post");
            exit(1);
        }

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
        printf("invalid input for submit command");
        p_table->history[p_table->history_count].completed = true;
        return -1;
    }
    p_table->history[p_table->history_count].priority = priority_int;

    char *exec_command[2] = {command, NULL};

    int status;
    status = fork();
    if (status < 0) {
        printf("fork() failed.\n");
        exit(1);
    } else if (status == 0) {
        if (execvp(exec_command[0], exec_command) == -1) {
            perror("execvp");
            printf("Not a valid/supported command.\n");
            exit(1);
        }
        exit(0);
    } else {
        if (kill(status, SIGSTOP) == -1) {
            perror("kill");
            exit(1);
        }
        return status;
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
    char *cmd_copy = strdup(cmd);
    int pid = -1;
    if (hasPipes(cmd)) {
        char **command_1 = break_delim(cmd, "|");
        char ***command_2 = pipe_manager(command_1);
        pid = pipe_execute(command_2);
    } else {
        char **command = break_delim(cmd, " \n");
        pid = launch2(command, background);
    }

    if (sem_wait(&p_table->mutex) == -1) {
        perror("sem_wait");
        exit(1);
    }
    strcpy(p_table->history[p_table->history_count].command, cmd_copy);
    p_table->history[p_table->history_count].pid = pid;
    p_table->history[p_table->history_count].completed = false;
    p_table->history_count++;
    if (sem_post(&p_table->mutex) == -1) {
        perror("sem_post");
        exit(1);
    }

    free(cmd_copy);
    return pid;
}

int launch(char *command_line, bool background) {
    int pid = fork();
    if (pid < 0) {
        printf("Fork failed.\n");
        return -1;
    } else if (pid == 0) {
        if (strncmp(command_line, "history", 7) == 0) {
            print_history();
            exit(0);
        }
        if (strncmp(command_line, "submit", 6) == 0) {
            if (sem_wait(&p_table->mutex) == -1) {
                perror("sem_wait");
                exit(1);
            }
            p_table->history[p_table->history_count].submit = true;
            p_table->history[p_table->history_count].completed = false;
            p_table->history[p_table->history_count].priority = 1;
            p_table->history[p_table->history_count].queue = false;
            start_time(&p_table->history[p_table->history_count].start);
            char **command = break_delim(command_line, " ");
            int command_count = 0;
            while (command[command_count] != NULL) {
                command_count++;
            }

            if (command_count == 3) {
                p_table->history[p_table->history_count].pid = submit_process(command[1], command[2]);
            } else {
                p_table->history[p_table->history_count].pid = submit_process(command[1], "1");
            }

            start_time(&p_table->history[p_table->history_count].start);
            if (sem_post(&p_table->mutex) == -1) {
                perror("sem_post");
                exit(1);
            }
            return 1;
        }
        if (strncmp(command_line, "exit", 4) == 0) {
            return 0;
        }

        int status;
        status = shell_proc(command_line);
        return status;
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

struct history_struct *setup() {
    shm_unlink("shm");

    shm_fd = shm_open("shm", O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open");
        exit(1);
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

    p_table->history[p_table->history_count].pid = -1;
    p_table->history[p_table->history_count].submit = false;
    p_table->history[p_table->history_count].wait_time = 0;
    p_table->history[p_table->history_count].execution_time = 0;
    p_table->history[p_table->history_count].vruntime = 0;
    start_time(&p_table->history[p_table->history_count].start);

    if (sem_post(&p_table->mutex) == -1) {
        perror("sem_post");
        exit(1);
    }
}

int main(int argc, char **argv) {
    sig_handler();
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
    printf("Forking child proc for scheduler\n");
    int stat = fork();
    if (stat < 0) {
        printf("Forking failed");
        exit(1);
    }
    if (stat == 0) {
        execlp("./scheduler", "./scheduler", NULL);
        printf("execvp failed");
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
        fgets(cmd, 100, stdin);

        if (strlen(cmd) > 0 && cmd[strlen(cmd) - 1] == '\n') {
            cmd[strlen(cmd) - 1] = '\0';
        }

        if (sem_wait(&p_table->mutex) == -1) {
            perror("sem_wait");
            exit(1);
        }

        strcpy(p_table->history[p_table->history_count].command, cmd);

        if (sem_post(&p_table->mutex) == -1) {
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
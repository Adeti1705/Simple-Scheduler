// dummy_main.h
#ifndef DUMMY_MAIN_H
#define DUMMY_MAIN_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

int dummy_main(int argc, char **argv);

int main(int argc, char **argv) {
    // Custom code to support SimpleScheduler implementation
    signal(SIGSTOP, SIG_DFL);  // Reset SIGSTOP handler to default
    signal(SIGCONT, SIG_DFL);  // Reset SIGCONT handler to default

    // You can add more initialization code here if needed

    int ret = dummy_main(argc, argv);

    // You can add cleanup code here if needed
    return ret;
}

#define main dummy_main

#endif // DUMMY_MAIN_
#include <stdio.h>
#include "dummy_main.h"

int main() {
    long long i;
    double result = 0;
    for (i = 0; i < 1000000000; i++) {
        result += i * i;
    }
    printf("Result: %f\n", result);
    return 0;
}
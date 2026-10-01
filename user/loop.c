#include <stdint.h>
#include "jupiter.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    j_print("Loop process started. PID: ");
    j_print_uint(j_getpid());
    j_print(" (use ps/kill/wait)\n");
    for (;;) j_yield();
    return 0;
}
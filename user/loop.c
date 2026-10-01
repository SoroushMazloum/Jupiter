#include <stdint.h>
#include "jupiter.h"

int main(int argc, char **argv)
{
    j_print("Loop process started. PID: ");
    j_print("(use ps/kill/wait)\n");
    (void)argc;
    (void)argv;
    for (;;) j_yield();
    return 0;
}

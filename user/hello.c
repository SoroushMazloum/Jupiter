#include <stdint.h>
#include "jupiter.h"

static void print_uint(uint32_t value)
{
    char digits[11];
    int n = 0;
    if (value == 0) { j_print("0"); return; }
    while (value) {
        digits[n++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    while (n--) j_write(&digits[n], 1);
}
static uint32_t zeros[64];
int main(int argc, char **argv)
{
    uint32_t sum = 0;
    for (int i = 0; i < 64; i++)
        sum += zeros[i];
    j_print("bss sum: ");
    print_uint(sum);
    j_print("\n");
    j_print("Hello from a C userspace program!\n");
    j_print("PID: ");
    print_uint(j_getpid());
    j_print("\nargc: ");
    print_uint((uint32_t)argc);
    j_print("\n");

    for (int i = 0; i < argc; i++) {
        j_print("argv[");
        print_uint((uint32_t)i);
        j_print("] = ");
        j_print(argv[i]);
        j_print("\n");
    }

    return 0;
}

#ifndef JUPITER_H
#define JUPITER_H
#include <stdint.h>

static inline uint32_t j_syscall(uint32_t number, uint32_t arg1, uint32_t arg2)
{
    register uint32_t eax asm("eax") = number;
    register uint32_t esi asm("esi") = arg1;
    register uint32_t ecx asm("ecx") = arg2;
    __asm__ volatile ("int $0x80"
                      : "+a"(eax)
                      : "S"(esi), "c"(ecx)
                      : "memory");
    return eax;
}

static inline void j_write(const char *text, uint32_t length)
{
    j_syscall(1, (uint32_t)text, length);
}

static inline void j_print(const char *text)
{
    uint32_t n = 0;
    while (text[n]) n++;
    j_write(text, n);
}

static inline uint32_t j_getpid(void)
{
    return j_syscall(4, 0, 0);
}

static inline void j_yield(void)
{
    j_syscall(3, 0, 0);
}

static inline void j_exit(uint32_t code)
{
    j_syscall(2, code, 0);
    for (;;) j_yield();
}

#endif

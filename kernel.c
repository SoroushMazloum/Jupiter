#include <stdint.h>

#define VGA_MEMORY ((volatile uint16_t *)0xB8000)
#define VGA_COLS   80
#define VGA_ROWS   25
#define COLOR      0x1F   /* white on blue */

static void clear_screen(void) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_MEMORY[i] = ((uint16_t)COLOR << 8) | ' ';
}

static void print_centered(const char *s, int row) {
    int len = 0;
    while (s[len]) len++;
    int col = (VGA_COLS - len) / 2;
    for (int i = 0; i < len; i++)
        VGA_MEMORY[row * VGA_COLS + col + i] = ((uint16_t)COLOR << 8) | s[i];
}

void kmain(void) {
    clear_screen();
    print_centered("JupiterOS", VGA_ROWS / 2);

    for (;;)
        __asm__ volatile ("hlt");
}
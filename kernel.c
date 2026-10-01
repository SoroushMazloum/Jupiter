#include <stdint.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((volatile uint16_t *)0xB8000)

#define IDT_ENTRIES 256
#define TIMER_VECTOR    32
#define KEYBOARD_VECTOR 33
#define PAGE_FAULT_VECTOR 14

#define PIT_FREQUENCY   1193182u
#define TIMER_HZ        100u
#define PIT_DIVISOR     (PIT_FREQUENCY / TIMER_HZ)

#define HEAP_START      0x00100000u
#define HEAP_END        0x00200000u
#define HEAP_SIZE       (HEAP_END - HEAP_START)
#define HEAP_MAGIC      0x4A484541u
#define COMMAND_MAX     70
#define KEY_LEFT        0x100
#define KEY_RIGHT       0x101
#define KEY_DELETE      0x102

#define USER_CODE_VA     0x00400000u
#define USER_STACK_VA    0x00401000u
#define USER_STACK_TOP   0x00402000u
#define USER_CS          0x1Bu
#define USER_DS          0x23u
#define TSS_SELECTOR     0x28u

static uint8_t terminal_color = 0x1F;
static int cursor_row = 0;
static int cursor_col = 0;

/*
 * VGA output is shared by the kernel shell and Ring 3 processes.  The PIT
 * can preempt a task in the middle of a multi-character print, so save the
 * interrupt state while emitting an entire logical message.
 */
static inline uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    __asm__ volatile ("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static void cursor_update(void)
{
    uint16_t position = (uint16_t)(cursor_row * VGA_WIDTH + cursor_col);

    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(position & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((position >> 8) & 0xFF));
}

static void terminal_clear(void);
static void terminal_write(const char *s);
static void terminal_putc(char c);
static void shell_prompt(void);

static void terminal_scroll(void)
{
    if (cursor_row < VGA_HEIGHT)
        return;

    for (int r = 1; r < VGA_HEIGHT; r++)
        for (int c = 0; c < VGA_WIDTH; c++)
            VGA_MEMORY[(r - 1) * VGA_WIDTH + c] =
                VGA_MEMORY[r * VGA_WIDTH + c];

    for (int c = 0; c < VGA_WIDTH; c++)
        VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + c] =
            ((uint16_t)terminal_color << 8) | ' ';

    cursor_row = VGA_HEIGHT - 1;
}

static void terminal_clear(void)
{
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEMORY[i] = ((uint16_t)terminal_color << 8) | ' ';

    cursor_row = 0;
    cursor_col = 0;
    cursor_update();
}

static void terminal_putc(char c)
{
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
        terminal_scroll();
        cursor_update();
        return;
    }

    if (c == '\r') {
        cursor_col = 0;
        cursor_update();
        return;
    }

    if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            VGA_MEMORY[cursor_row * VGA_WIDTH + cursor_col] =
                ((uint16_t)terminal_color << 8) | ' ';
        }
        cursor_update();
        return;
    }

    VGA_MEMORY[cursor_row * VGA_WIDTH + cursor_col] =
        ((uint16_t)terminal_color << 8) | (uint8_t)c;

    cursor_col++;
    if (cursor_col >= VGA_WIDTH) {
        cursor_col = 0;
        cursor_row++;
        terminal_scroll();
    }

    cursor_update();
}

static void terminal_write(const char *s)
{
    uint32_t flags = irq_save();
    while (*s)
        terminal_putc(*s++);
    irq_restore(flags);
}

static int str_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int str_starts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

static void reboot(void)
{
    uint8_t good = 0x02;

    while (good & 0x02)
        good = inb(0x64);

    outb(0x64, 0xFE);

    for (;;)
        __asm__ volatile ("hlt");
}

/* QEMU/Bochs shutdown port. Full ACPI S5 support will come later. */
static void poweroff(void)
{
    __asm__ volatile ("cli");
    outw(0x604, 0x2000);

    for (;;)
        __asm__ volatile ("hlt");
}

/* ---------------- Kernel heap ---------------- */

struct heap_block {
    uint32_t magic;
    uint32_t size;
    uint32_t free;
    struct heap_block *next;
    struct heap_block *prev;
};

#define HEAP_HEADER_SIZE ((uint32_t)sizeof(struct heap_block))

static struct heap_block *heap_first = (struct heap_block *)HEAP_START;
static uint32_t heap_used = 0;
static uint32_t heap_allocations = 0;

static void heap_init(void)
{
    heap_first->magic = HEAP_MAGIC;
    heap_first->size = HEAP_SIZE - HEAP_HEADER_SIZE;
    heap_first->free = 1;
    heap_first->next = 0;
    heap_first->prev = 0;
    heap_used = 0;
    heap_allocations = 0;
}

static void *kmalloc(uint32_t size)
{
    if (size == 0)
        return 0;

    size = (size + 7u) & ~7u;

    struct heap_block *block = heap_first;
    while (block) {
        if (block->magic == HEAP_MAGIC && block->free && block->size >= size) {
            uint32_t remaining = block->size - size;

            if (remaining >= HEAP_HEADER_SIZE + 8u) {
                struct heap_block *next =
                    (struct heap_block *)((uint8_t *)block + HEAP_HEADER_SIZE + size);

                next->magic = HEAP_MAGIC;
                next->size = remaining - HEAP_HEADER_SIZE;
                next->free = 1;
                next->next = block->next;
                next->prev = block;

                if (block->next)
                    block->next->prev = next;

                block->next = next;
                block->size = size;
            }

            block->free = 0;
            heap_used += block->size;
            heap_allocations++;
            return (void *)((uint8_t *)block + HEAP_HEADER_SIZE);
        }

        block = block->next;
    }

    return 0;
}

static void kfree(void *ptr)
{
    if (!ptr)
        return;

    struct heap_block *block =
        (struct heap_block *)((uint8_t *)ptr - HEAP_HEADER_SIZE);

    if (block->magic != HEAP_MAGIC || block->free)
        return;

    block->free = 1;
    heap_used -= block->size;
    if (heap_allocations)
        heap_allocations--;

    if (block->next && block->next->free) {
        struct heap_block *next = block->next;
        block->size += HEAP_HEADER_SIZE + next->size;
        block->next = next->next;
        if (block->next)
            block->next->prev = block;
    }

    if (block->prev && block->prev->free) {
        struct heap_block *prev = block->prev;
        prev->size += HEAP_HEADER_SIZE + block->size;
        prev->next = block->next;
        if (prev->next)
            prev->next->prev = prev;
    }
}

static uint32_t heap_free_bytes(void)
{
    return HEAP_SIZE - heap_used;
}

static void print_uint(uint32_t value)
{
    char digits[11];
    int n = 0;

    if (value == 0) {
        terminal_putc('0');
        return;
    }

    while (value && n < 10) {
        digits[n++] = (char)('0' + value % 10u);
        value /= 10u;
    }

    while (n--)
        terminal_putc(digits[n]);
}

/* ---------------- Paging ---------------- */

#define PAGE_SIZE       4096u
#define PAGE_ENTRIES    1024u

static uint32_t page_directory[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint32_t first_page_table[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static volatile uint32_t paging_enabled = 0;

#define MAX_USER_PROCS 4
static uint32_t *user_page_directories[MAX_USER_PROCS];
static uint32_t *user_page_tables[MAX_USER_PROCS];
static uint8_t *user_code_pages[MAX_USER_PROCS];
static uint8_t *user_stack_pages[MAX_USER_PROCS];

static void *page_alloc(void)
{
    uintptr_t raw = (uintptr_t)kmalloc(PAGE_SIZE * 2u);
    if (!raw) return 0;
    return (void *)((raw + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u));
}

extern const uint8_t user_program_start[];
extern const uint8_t user_program_end[];

static void paging_build_user_space(uint32_t slot)
{
    uint32_t *pd = user_page_directories[slot];
    uint32_t *pt = user_page_tables[slot];
    for (uint32_t i = 0; i < PAGE_ENTRIES; i++) {
        pd[i] = 0;
        pt[i] = 0;
    }
    pd[0] = ((uint32_t)first_page_table) | 0x003u;
    pt[0] = ((uint32_t)user_code_pages[slot]) | 0x007u;
    pt[1] = ((uint32_t)user_stack_pages[slot]) | 0x007u;
    pd[1] = ((uint32_t)pt) | 0x007u;
}

static void paging_init(void)
{
    for (uint32_t i = 0; i < PAGE_ENTRIES; i++) {
        page_directory[i] = 0;
        first_page_table[i] = (i * PAGE_SIZE) | 0x003u;
    }

    page_directory[0] = ((uint32_t)first_page_table) | 0x003u;

    for (uint32_t slot = 0; slot < MAX_USER_PROCS; slot++) {
        user_page_directories[slot] = (uint32_t *)page_alloc();
        user_page_tables[slot] = (uint32_t *)page_alloc();
        user_code_pages[slot] = (uint8_t *)page_alloc();
        user_stack_pages[slot] = (uint8_t *)page_alloc();
        if (!user_page_directories[slot] || !user_page_tables[slot] ||
            !user_code_pages[slot] || !user_stack_pages[slot]) {
            terminal_write("Paging: unable to allocate user process memory\n");
            for (;;) __asm__ volatile ("hlt");
        }
        paging_build_user_space(slot);
        for (uint32_t i = 0; i < PAGE_SIZE; i++) {
            user_code_pages[slot][i] = 0;
            user_stack_pages[slot][i] = 0;
        }
    }

    __asm__ volatile (
        "mov %0, %%cr3\n"
        "mov %%cr0, %%eax\n"
        "or $0x80000000, %%eax\n"
        "mov %%eax, %%cr0\n"
        :
        : "r"((uint32_t)page_directory)
        : "eax", "memory");

    paging_enabled = 1;
}

static void switch_address_space(uint32_t slot)
{
    if (slot >= MAX_USER_PROCS)
        return;
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint32_t)user_page_directories[slot]) : "memory");
}

static void switch_kernel_address_space(void)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint32_t)page_directory) : "memory");
}

static uint32_t read_cr3(void)
{
    uint32_t value;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(value));
    return value;
}


/* ---------------- ATA PIO + JFS1 filesystem ---------------- */

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_CNT  0x1F2
#define ATA_LBA0        0x1F3
#define ATA_LBA1        0x1F4
#define ATA_LBA2        0x1F5
#define ATA_DRIVE       0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7
#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30
#define ATA_CMD_FLUSH   0xE7

#define FS_START_LBA    65u
#define FS_SECTOR_SIZE  512u
#define FS_MAX_ENTRIES  16u
#define FS_NAME_MAX     15u
#define FS_MAGIC0       'J'
#define FS_MAGIC1       'F'
#define FS_MAGIC2       'S'
#define FS_MAGIC3       '1'

struct fs_superblock {
    char magic[4];
    uint32_t version;
    uint32_t sector_size;
    uint32_t dir_lba;
    uint32_t dir_entries;
    uint32_t data_lba;
    uint32_t total_sectors;
    uint8_t reserved[484];
} __attribute__((packed));

_Static_assert(sizeof(struct fs_superblock) == 512, "superblock must be one sector");

struct fs_dirent {
    char name[16];
    uint32_t start_lba;
    uint32_t size;
    uint32_t sectors;
    uint8_t reserved[4];
} __attribute__((packed));

static struct fs_superblock fs_super;
static struct fs_dirent fs_directory[FS_MAX_ENTRIES];
static uint8_t fs_sector_buffer[FS_SECTOR_SIZE] __attribute__((aligned(2)));
static uint8_t fs_ready = 0;

static int ata_wait(uint8_t mask, uint8_t value)
{
    for (uint32_t i = 0; i < 1000000u; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (status & 0x01)
            return 0;
        if ((status & mask) == value)
            return 1;
    }
    return 0;
}

static int ata_read_sector(uint32_t lba, void *buffer)
{
    if (!ata_wait(0x80, 0x00))
        return 0;

    outb(ATA_DRIVE, (uint8_t)(0xE0u | ((lba >> 24) & 0x0Fu)));
    outb(ATA_SECTOR_CNT, 1);
    outb(ATA_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_READ);

    if (!ata_wait(0x08, 0x08))
        return 0;

    uint16_t *dst = (uint16_t *)buffer;
    for (int i = 0; i < 256; i++)
        __asm__ volatile ("inw %1, %0" : "=a"(dst[i]) : "Nd"((uint16_t)ATA_DATA));

    return 1;
}

static int ata_write_sector(uint32_t lba, const void *buffer)
{
    if (!ata_wait(0x80, 0x00))
        return 0;

    outb(ATA_DRIVE, (uint8_t)(0xE0u | ((lba >> 24) & 0x0Fu)));
    outb(ATA_SECTOR_CNT, 1);
    outb(ATA_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_WRITE);

    if (!ata_wait(0x08, 0x08))
        return 0;

    const uint16_t *src = (const uint16_t *)buffer;
    for (int i = 0; i < 256; i++)
        __asm__ volatile ("outw %0, %1" : : "a"(src[i]), "Nd"((uint16_t)ATA_DATA));

    outb(ATA_COMMAND, ATA_CMD_FLUSH);
    return ata_wait(0x80, 0x00);
}

static int fs_load(void)
{
    if (fs_ready)
        return 1;

    if (!ata_read_sector(FS_START_LBA, &fs_super))
        return 0;

    if (fs_super.magic[0] != FS_MAGIC0 ||
        fs_super.magic[1] != FS_MAGIC1 ||
        fs_super.magic[2] != FS_MAGIC2 ||
        fs_super.magic[3] != FS_MAGIC3 ||
        fs_super.version != 1 ||
        fs_super.sector_size != FS_SECTOR_SIZE ||
        fs_super.dir_entries > FS_MAX_ENTRIES)
        return 0;

    if (!ata_read_sector(fs_super.dir_lba, fs_directory))
        return 0;

    fs_ready = 1;
    return 1;
}

static int fs_name_equal(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb)
            return 0;
    }
    return *a == '\0' && *b == '\0';
}

static struct fs_dirent *fs_find(const char *name)
{
    if (!fs_load())
        return 0;

    for (uint32_t i = 0; i < fs_super.dir_entries; i++) {
        if (fs_directory[i].name[0] && fs_name_equal(fs_directory[i].name, name))
            return &fs_directory[i];
    }
    return 0;
}


#define JEXE_MAGIC0 'J'
#define JEXE_MAGIC1 'E'
#define JEXE_MAGIC2 'X'
#define JEXE_MAGIC3 'E'

struct jexe_header {
    char magic[4];
    uint32_t version;
    uint32_t entry;
    uint32_t image_size;
} __attribute__((packed));

static int fs_load_jexe(const char *name, uint8_t *code_page, uint32_t *entry_out, uint32_t *image_size_out)
{
    struct fs_dirent *file = fs_find(name);
    if (!file || file->size < sizeof(struct jexe_header))
        return 0;
    if (file->size > PAGE_SIZE)
        return 0;

    uint32_t copied = 0;
    uint32_t remaining = file->size;
    for (uint32_t sector = 0; sector < file->sectors && remaining; sector++) {
        if (!ata_read_sector(file->start_lba + sector, fs_sector_buffer))
            return 0;

        uint32_t count = remaining < FS_SECTOR_SIZE ? remaining : FS_SECTOR_SIZE;
        for (uint32_t i = 0; i < count; i++) {
            if (copied < PAGE_SIZE)
                code_page[copied] = fs_sector_buffer[i];
            copied++;
        }
        remaining -= count;
    }

    struct jexe_header *header = (struct jexe_header *)code_page;
    if (header->magic[0] != JEXE_MAGIC0 ||
        header->magic[1] != JEXE_MAGIC1 ||
        header->magic[2] != JEXE_MAGIC2 ||
        header->magic[3] != JEXE_MAGIC3 ||
        header->version != 1 ||
        header->image_size > PAGE_SIZE - sizeof(struct jexe_header) ||
        header->entry >= header->image_size)
        return 0;

    /* Save header fields before moving the image over the header. */
    uint32_t entry = header->entry;
    uint32_t image_size = header->image_size;

    /* Move the executable image over its header. */
    for (uint32_t i = 0; i < image_size; i++)
        code_page[i] = code_page[sizeof(struct jexe_header) + i];

    *entry_out = entry;
    if (image_size_out)
        *image_size_out = image_size;
    return 1;
}

static void fs_ls(void)
{
    terminal_putc('\n');
    if (!fs_load()) {
        terminal_write("Filesystem: unable to read JFS1 filesystem\n");
        return;
    }

    terminal_write("NAME             SIZE\n");
    for (uint32_t i = 0; i < fs_super.dir_entries; i++) {
        if (!fs_directory[i].name[0])
            continue;
        terminal_write(fs_directory[i].name);
        int len = 0;
        while (fs_directory[i].name[len]) len++;
        for (; len < 17; len++) terminal_putc(' ');
        print_uint(fs_directory[i].size);
        terminal_write(" bytes\n");
    }
}

static void fs_cat(const char *name)
{
    terminal_putc('\n');
    struct fs_dirent *entry = fs_find(name);
    if (!entry) {
        if (!fs_ready)
            terminal_write("Filesystem: unable to read JFS1 filesystem\n");
        else
            terminal_write("File not found.\n");
        return;
    }

    uint32_t remaining = entry->size;
    for (uint32_t sector = 0; sector < entry->sectors && remaining; sector++) {
        if (!ata_read_sector(entry->start_lba + sector, fs_sector_buffer)) {
            terminal_write("Disk read error.\n");
            return;
        }

        uint32_t count = remaining < FS_SECTOR_SIZE ? remaining : FS_SECTOR_SIZE;
        for (uint32_t i = 0; i < count; i++)
            terminal_putc((char)fs_sector_buffer[i]);
        remaining -= count;
    }
    if (entry->size)
        terminal_putc('\n');
}

static void fs_zero_buffer(void)
{
    for (uint32_t i = 0; i < FS_SECTOR_SIZE; i++)
        fs_sector_buffer[i] = 0;
}

static int fs_sync_directory(void)
{
    return ata_write_sector(fs_super.dir_lba, fs_directory);
}

static int fs_range_overlaps(uint32_t start_a, uint32_t count_a,
                             uint32_t start_b, uint32_t count_b)
{
    if (!count_a || !count_b)
        return 0;
    return start_a < start_b + count_b && start_b < start_a + count_a;
}

static uint32_t fs_alloc_start(uint32_t sectors, int ignore_index)
{
    if (!sectors)
        return 0;

    uint32_t first = fs_super.data_lba;
    uint32_t end = FS_START_LBA + fs_super.total_sectors;

    for (uint32_t candidate = first; candidate + sectors <= end; candidate++) {
        int free = 1;
        for (uint32_t i = 0; i < fs_super.dir_entries; i++) {
            if ((int)i == ignore_index || !fs_directory[i].name[0])
                continue;
            if (fs_range_overlaps(candidate, sectors,
                                  fs_directory[i].start_lba,
                                  fs_directory[i].sectors)) {
                free = 0;
                break;
            }
        }
        if (free)
            return candidate;
    }
    return 0;
}

static int fs_valid_name(const char *name)
{
    uint32_t len = 0;
    while (name[len]) {
        if (len >= FS_NAME_MAX)
            return 0;
        if (name[len] == ' ' || name[len] == '\n' || name[len] == '\r')
            return 0;
        len++;
    }
    return len > 0;
}

static int fs_name_length(const char *name)
{
    int len = 0;
    while (name[len]) len++;
    return len;
}

static int fs_create_empty(const char *name)
{
    if (!fs_load() || !fs_valid_name(name) || fs_find(name))
        return 0;

    int slot = -1;
    for (uint32_t i = 0; i < fs_super.dir_entries; i++) {
        if (!fs_directory[i].name[0]) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0)
        return 0;

    for (uint32_t i = 0; i < sizeof(fs_directory[slot]); i++)
        ((uint8_t *)&fs_directory[slot])[i] = 0;

    int len = fs_name_length(name);
    for (int i = 0; i < len; i++)
        fs_directory[slot].name[i] = name[i];

    if (!fs_sync_directory())
        return 0;
    return 1;
}

static int fs_remove(const char *name)
{
    if (!fs_load())
        return 0;

    int slot = -1;
    for (uint32_t i = 0; i < fs_super.dir_entries; i++) {
        if (fs_directory[i].name[0] && fs_name_equal(fs_directory[i].name, name)) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0)
        return 0;

    for (uint32_t i = 0; i < sizeof(fs_directory[slot]); i++)
        ((uint8_t *)&fs_directory[slot])[i] = 0;

    return fs_sync_directory();
}

static int fs_write_text(const char *name, const char *text)
{
    if (!fs_load() || !fs_valid_name(name))
        return 0;

    int slot = -1;
    for (uint32_t i = 0; i < fs_super.dir_entries; i++) {
        if (fs_directory[i].name[0] && fs_name_equal(fs_directory[i].name, name)) {
            slot = (int)i;
            break;
        }
    }

    if (slot < 0) {
        for (uint32_t i = 0; i < fs_super.dir_entries; i++) {
            if (!fs_directory[i].name[0]) {
                slot = (int)i;
                break;
            }
        }
        if (slot < 0)
            return 0;
        for (uint32_t i = 0; i < sizeof(fs_directory[slot]); i++)
            ((uint8_t *)&fs_directory[slot])[i] = 0;
        int len = fs_name_length(name);
        for (int i = 0; i < len; i++)
            fs_directory[slot].name[i] = name[i];
    }

    uint32_t size = 0;
    while (text[size]) size++;
    uint32_t sectors = (size + FS_SECTOR_SIZE - 1u) / FS_SECTOR_SIZE;
    uint32_t start = 0;

    /* Reuse an existing extent when the new file still fits. */
    if (slot >= 0 && fs_directory[slot].name[0] &&
        sectors <= fs_directory[slot].sectors) {
        start = fs_directory[slot].start_lba;
    } else {
        start = fs_alloc_start(sectors, slot);
    }

    if (sectors && !start)
        return 0;

    for (uint32_t sector = 0; sector < sectors; sector++) {
        fs_zero_buffer();
        uint32_t offset = sector * FS_SECTOR_SIZE;
        uint32_t remaining = size - offset;
        uint32_t count = remaining < FS_SECTOR_SIZE ? remaining : FS_SECTOR_SIZE;
        for (uint32_t i = 0; i < count; i++)
            fs_sector_buffer[i] = (uint8_t)text[offset + i];
        if (!ata_write_sector(start + sector, fs_sector_buffer))
            return 0;
    }

    fs_directory[slot].start_lba = start;
    fs_directory[slot].size = size;
    fs_directory[slot].sectors = sectors;
    return fs_sync_directory();
}

/* ---------------- IDT ---------------- */

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

extern void irq0_stub(void);
extern void irq1_stub(void);
extern void default_irq_stub(void);
extern void page_fault_stub(void);
extern void syscall_stub(void);

static void idt_set_gate(int vector, uint32_t handler)
{
    idt[vector].offset_low = (uint16_t)(handler & 0xFFFF);
    idt[vector].selector = 0x08;
    idt[vector].zero = 0;
    idt[vector].flags = 0x8E; /* present, ring 0, 32-bit interrupt gate */
    idt[vector].offset_high = (uint16_t)((handler >> 16) & 0xFFFF);
}

static void idt_init(void)
{
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(i, (uint32_t)default_irq_stub);

    idt_set_gate(TIMER_VECTOR, (uint32_t)irq0_stub);
    idt_set_gate(KEYBOARD_VECTOR, (uint32_t)irq1_stub);
    idt_set_gate(PAGE_FAULT_VECTOR, (uint32_t)page_fault_stub);

    idt[0x80].offset_low = (uint16_t)((uint32_t)syscall_stub & 0xFFFF);
    idt[0x80].selector = 0x08;
    idt[0x80].zero = 0;
    idt[0x80].flags = 0xEE; /* present, DPL 3, 32-bit trap gate */
    idt[0x80].offset_high = (uint16_t)(((uint32_t)syscall_stub >> 16) & 0xFFFF);

    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint32_t)&idt[0];

    __asm__ volatile ("lidt %0" : : "m"(idtp));
}

/* ---------------- GDT / TSS ---------------- */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_ptr gdtp;
static struct tss_entry tss;

static void gdt_set_entry(int n, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt[n].base_low = (uint16_t)(base & 0xFFFF);
    gdt[n].base_mid = (uint8_t)((base >> 16) & 0xFF);
    gdt[n].base_high = (uint8_t)((base >> 24) & 0xFF);
    gdt[n].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt[n].granularity = (uint8_t)((limit >> 16) & 0x0F);
    gdt[n].granularity |= gran & 0xF0;
    gdt[n].access = access;
}

static void gdt_tss_init(void)
{
    gdt_set_entry(0, 0, 0, 0, 0);
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xCF);
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xCF);
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xCF);
    gdt_set_entry(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base = (uint32_t)&gdt[0];

    __asm__ volatile (
        "lgdt %0\n"
        "ljmp $0x08, $1f\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        : : "m"(gdtp) : "ax", "memory");

    for (uint32_t *p = (uint32_t *)&tss; p < (uint32_t *)((uint8_t *)&tss + sizeof(tss)); p++)
        *p = 0;

    tss.ss0 = 0x10;
    tss.esp0 = 0x90000;
    tss.iomap_base = sizeof(tss);

    __asm__ volatile ("mov $0x28, %%ax\n\t" "ltr %%ax" : : : "ax");
}

/* ---------------- PIC ---------------- */

static void pic_init(void)
{
    /* ICW1: begin initialization, ICW4 required. */
    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    /* ICW2: remap master to vectors 32-39, slave to 40-47. */
    outb(0x21, 0x20);
    outb(0xA1, 0x28);

    /* ICW3: master IRQ2 has the slave; slave is attached to IRQ2. */
    outb(0x21, 0x04);
    outb(0xA1, 0x02);

    /* ICW4: 8086/88 mode. */
    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    /* Unmask IRQ0 (timer) and IRQ1 (keyboard) on the master. */
    outb(0x21, 0xFC);
    outb(0xA1, 0xFF);
}


/* ---------------- Scheduler / process management ---------------- */

static volatile uint32_t timer_ticks = 0;

#define MAX_TASKS        8
#define TASK_STACK_SIZE  4096u

enum task_state {
    TASK_FREE = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_ZOMBIE
};

struct task {
    uint32_t esp;
    uint32_t id;
    uint32_t ticks;
    const char *name;
    uint8_t user;
    uint8_t state;
    uint8_t exit_code;
    int32_t parent;
    uint32_t address_space;
};

static struct task tasks[MAX_TASKS];
static uint8_t *task_stacks[MAX_TASKS];
static uint8_t *kernel_stacks[MAX_TASKS];
static volatile uint32_t current_task = 0;

static void worker_task_1(void)
{
    for (;;) { tasks[1].ticks++; __asm__ volatile ("hlt"); }
}
static void worker_task_2(void)
{
    for (;;) { tasks[2].ticks++; __asm__ volatile ("hlt"); }
}
static void worker_task_3(void)
{
    for (;;) { tasks[3].ticks++; __asm__ volatile ("hlt"); }
}

static uint32_t task_initial_stack(uint8_t *stack, void (*entry)(void))
{
    uint32_t *sp = (uint32_t *)(stack + TASK_STACK_SIZE);
    sp -= 11;
    for (int i = 0; i < 8; i++) sp[i] = 0;
    sp[8] = (uint32_t)entry;
    sp[9] = 0x08;
    sp[10] = 0x202;
    return (uint32_t)sp;
}

static uint32_t build_user_stack(uint8_t *stack, int argc, const char *const *argv)
{
    uint8_t *base = stack;
    uint32_t top = PAGE_SIZE;
    uint32_t argv_va[COMMAND_MAX / 2 + 2];
    if (argc > (int)(COMMAND_MAX / 2 + 1)) argc = COMMAND_MAX / 2 + 1;

    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = 0;
        while (argv[i][len]) len++;
        len++;
        if (len > top) return 0;
        top -= len;
        for (uint32_t j = 0; j < len; j++) base[top + j] = (uint8_t)argv[i][j];
        argv_va[i] = USER_STACK_VA + top;
    }

    top &= ~3u;
    uint32_t bytes = (uint32_t)(argc + 1) * 4u + 4u;
    if (bytes > top) return 0;
    top -= bytes;
    uint32_t *sp = (uint32_t *)(base + top);
    sp[0] = (uint32_t)argc;
    for (int i = 0; i < argc; i++) sp[1 + i] = argv_va[i];
    sp[1 + argc] = 0;
    return USER_STACK_VA + top;
}

static uint32_t user_initial_context(uint8_t *stack, uint32_t entry, uint32_t user_sp)
{
    uint32_t *sp = (uint32_t *)(stack + TASK_STACK_SIZE);
    sp -= 13;
    for (int i = 0; i < 8; i++) sp[i] = 0;
    sp[8]  = USER_CODE_VA + entry;
    sp[9]  = USER_CS;
    sp[10] = 0x202;
    sp[11] = user_sp;
    sp[12] = USER_DS;
    return (uint32_t)sp;
}

static void process_reset(int i)
{
    tasks[i].esp = 0;
    tasks[i].id = (uint32_t)i;
    tasks[i].ticks = 0;
    tasks[i].name = "free";
    tasks[i].user = 0;
    tasks[i].state = TASK_FREE;
    tasks[i].exit_code = 0;
    tasks[i].parent = -1;
    tasks[i].address_space = 0;
}

static void scheduler_init(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        process_reset(i);
        task_stacks[i] = (uint8_t *)kmalloc(TASK_STACK_SIZE);
        kernel_stacks[i] = (uint8_t *)kmalloc(TASK_STACK_SIZE);
        if (!task_stacks[i] || !kernel_stacks[i]) {
            terminal_write("Scheduler: unable to allocate stacks\n");
            for (;;) __asm__ volatile ("hlt");
        }
    }
    tasks[0].name = "shell"; tasks[0].state = TASK_RUNNING;
    tasks[1].esp = task_initial_stack(task_stacks[1], worker_task_1);
    tasks[1].name = "worker1"; tasks[1].state = TASK_READY;
    tasks[1].user = 0;
    tasks[2].esp = task_initial_stack(task_stacks[2], worker_task_2);
    tasks[2].name = "worker2"; tasks[2].state = TASK_READY;
    tasks[2].user = 0;
    tasks[3].esp = task_initial_stack(task_stacks[3], worker_task_3);
    tasks[3].name = "worker3"; tasks[3].state = TASK_READY;
    tasks[3].user = 0;
    current_task = 0;
    tss.esp0 = 0x90000;
}

static int task_is_runnable(int i)
{
    return tasks[i].state == TASK_READY || tasks[i].state == TASK_RUNNING;
}

uint32_t scheduler_tick(uint32_t saved_esp)
{
    timer_ticks++;
    tasks[current_task].esp = saved_esp;
    tasks[current_task].ticks++;
    if (tasks[current_task].state == TASK_RUNNING)
        tasks[current_task].state = TASK_READY;

    uint32_t next = (current_task + 1u) % MAX_TASKS;
    while (!task_is_runnable((int)next))
        next = (next + 1u) % MAX_TASKS;

    current_task = next;
    tasks[current_task].state = TASK_RUNNING;

    if (tasks[current_task].user) {
        tss.esp0 = (uint32_t)(kernel_stacks[current_task] + TASK_STACK_SIZE);
        switch_address_space(tasks[current_task].address_space);
    } else {
        tss.esp0 = 0x90000;
        switch_kernel_address_space();
    }

    outb(0x20, 0x20);
    return tasks[current_task].esp;
}

static int alloc_user_task(void)
{
    for (int i = 4; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_FREE)
            return i;
    return -1;
}

static void reap_task(int pid)
{
    if (pid < 4 || pid >= MAX_TASKS) return;
    if (tasks[pid].state == TASK_ZOMBIE) process_reset(pid);
}

/* ---------------- System calls ---------------- */

#define SYS_WRITE  1u
#define SYS_EXIT   2u
#define SYS_YIELD  3u
#define SYS_GETPID 4u

uint32_t syscall_handler(uint32_t *frame)
{
    uint32_t number = frame[7];
    switch (number) {
    case SYS_WRITE: {
        uint32_t ptr = frame[1];
        uint32_t len = frame[6];
        if (ptr < USER_CODE_VA || ptr >= USER_STACK_TOP) break;
        if (len > USER_STACK_TOP - ptr) len = USER_STACK_TOP - ptr;

        uint32_t flags = irq_save();
        for (uint32_t i = 0; i < len; i++)
            terminal_putc(*(volatile char *)ptr++);
        irq_restore(flags);
        break;
    }
    case SYS_EXIT: {
        tasks[current_task].exit_code = (uint8_t)frame[1];
        tasks[current_task].state = TASK_ZOMBIE;

        uint32_t flags = irq_save();
        terminal_write("[kernel] process ");
        print_uint(tasks[current_task].id);
        terminal_write(" exited\n");
        irq_restore(flags);
        break;
    }
    case SYS_YIELD:
        break;
    case SYS_GETPID:
        frame[7] = tasks[current_task].id;
        break;
    default:
        terminal_write("[kernel] unknown syscall\n");
        break;
    }
    return 0;
}

/* ---------------- PIT timer IRQ0 ---------------- */


static void pit_init(void)
{
    /* Channel 0, lobyte/hibyte, mode 3 (square wave), binary. */
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(PIT_DIVISOR & 0xFF));
    outb(0x40, (uint8_t)((PIT_DIVISOR >> 8) & 0xFF));
}

static uint32_t timer_seconds(void)
{
    return timer_ticks / TIMER_HZ;
}

static uint32_t parse_uint(const char *s, int *ok)
{
    uint32_t value = 0;

    if (!*s) {
        *ok = 0;
        return 0;
    }

    while (*s) {
        if (*s < '0' || *s > '9') {
            *ok = 0;
            return 0;
        }
        value = value * 10u + (uint32_t)(*s - '0');
        s++;
    }

    *ok = 1;
    return value;
}

static void sleep_seconds(uint32_t seconds)
{
    uint32_t target = timer_ticks + seconds * TIMER_HZ;

    while ((int32_t)(target - timer_ticks) > 0)
        __asm__ volatile ("hlt");
}

/* ---------------- Keyboard IRQ1 ---------------- */

static volatile uint16_t key_queue[128];
static volatile uint8_t key_head = 0;
static volatile uint8_t key_tail = 0;
static volatile int shift_pressed = 0;

static const char key_normal[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=',
    '\b','\t','q','w','e','r','t','y','u','i','o','p','[',']',
    '\n',0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',
};

static const char key_shifted[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+',
    '\b','\t','Q','W','E','R','T','Y','U','I','O','P','{','}',
    '\n',0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',
};

static void keyboard_queue_push(uint16_t c)
{
    uint8_t next = (uint8_t)(key_head + 1);

    if (next == key_tail)
        return; /* queue full */

    key_queue[key_head] = c;
    key_head = next;
}

void keyboard_irq_handler(void)
{
    static int extended = 0;
    uint8_t scancode = inb(0x60);

    if (scancode == 0xE0) {
        extended = 1;
    } else if (extended) {
        extended = 0;
        if (!(scancode & 0x80)) {
            if (scancode == 0x4B)
                keyboard_queue_push(KEY_LEFT);
            else if (scancode == 0x4D)
                keyboard_queue_push(KEY_RIGHT);
            else if (scancode == 0x53)
                keyboard_queue_push(KEY_DELETE);
        }
    } else if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
    } else if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = 0;
    } else if (!(scancode & 0x80) && scancode < 128) {
        char c = shift_pressed
            ? key_shifted[scancode]
            : key_normal[scancode];

        if (c)
            keyboard_queue_push((uint16_t)(uint8_t)c);
    }

    outb(0x20, 0x20);
}

static uint16_t keyboard_getkey(void)
{
    for (;;) {
        if (key_tail != key_head) {
            uint16_t key = key_queue[key_tail];
            key_tail = (uint8_t)(key_tail + 1);
            return key;
        }

        __asm__ volatile ("hlt");
    }
}

/* ---------------- Shell ---------------- */

static char command[COMMAND_MAX + 1];
static int command_len = 0;
static int command_cursor = 0;

static void shell_prompt(void)
{
    terminal_write("jupiter> ");
}

static void shell_redraw(void)
{
    uint32_t flags = irq_save();
    int i;
    int row = cursor_row;
    int prompt_col = 9;

    /*
     * Redraw the command at absolute VGA positions.  Do not use
     * terminal_putc() here because the hardware cursor may currently be
     * in the middle of the command.
     */
    for (i = 0; i < COMMAND_MAX; i++) {
        char c = (i < command_len) ? command[i] : ' ';
        VGA_MEMORY[row * VGA_WIDTH + prompt_col + i] =
            ((uint16_t)terminal_color << 8) | (uint8_t)c;
    }

    cursor_col = prompt_col + command_cursor;
    cursor_update();
    irq_restore(flags);
}

static void shell_move_left(void)
{
    if (command_cursor > 0) {
        command_cursor--;
        cursor_col--;
        cursor_update();
    }
}

static void shell_move_right(void)
{
    if (command_cursor < command_len) {
        command_cursor++;
        cursor_col++;
        cursor_update();
    }
}

static void shell_insert_char(char c)
{
    if (command_len >= COMMAND_MAX)
        return;

    for (int i = command_len; i > command_cursor; i--)
        command[i] = command[i - 1];

    command[command_cursor] = c;
    command_len++;
    command_cursor++;

    shell_redraw();
}

static void shell_backspace(void)
{
    if (command_cursor == 0)
        return;

    for (int i = command_cursor - 1; i < command_len - 1; i++)
        command[i] = command[i + 1];

    command_len--;
    command_cursor--;
    cursor_col--;
    shell_redraw();
}

static void shell_exec(const char *args)
{
    terminal_putc('\n');

    char local[COMMAND_MAX + 1];
    int n = 0;
    while (*args && n < COMMAND_MAX) local[n++] = *args++;
    local[n] = 0;

    char *argv[COMMAND_MAX / 2 + 2];
    int argc = 0;
    char *p = local;
    while (*p && argc < (int)(COMMAND_MAX / 2 + 1)) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    if (argc == 0) {
        terminal_write("Usage: exec PROGRAM [args...] [&]\n");
        return;
    }

    /* exec is foreground by default. Add '&' to explicitly run in background. */
    int background = 0;
    if (argc > 0 && str_equal(argv[argc - 1], "&")) {
        background = 1;
        argc--;
        if (argc == 0) {
            terminal_write("Usage: exec PROGRAM [args...] [&]\n");
            return;
        }
    }

    int pid = alloc_user_task();
    if (pid < 0) {
        terminal_write("No free process slots.\n");
        return;
    }

    uint32_t entry = 0, image_size = 0;
    if (!fs_load_jexe(argv[0], user_code_pages[pid - 4], &entry, &image_size)) {
        process_reset(pid);
        terminal_write("Unable to load JEXE program.\n");
        return;
    }
    for (uint32_t i = 0; i < PAGE_SIZE; i++)
        user_stack_pages[pid - 4][i] = 0;
    uint32_t user_sp = build_user_stack(user_stack_pages[pid - 4], argc, (const char *const *)argv);
    uint32_t sp = user_initial_context(task_stacks[pid], entry, user_sp);
    if (!user_sp || !sp) {
        process_reset(pid);
        terminal_write("Unable to build user stack.\n");
        return;
    }

    tasks[pid].esp = sp;
    tasks[pid].id = (uint32_t)pid;
    tasks[pid].ticks = 0;
    tasks[pid].name = "userprog";
    tasks[pid].user = 1;
    tasks[pid].state = TASK_READY;
    tasks[pid].exit_code = 0;
    tasks[pid].parent = 0;
    tasks[pid].address_space = (uint32_t)(pid - 4);

    uint32_t flags = irq_save();
    terminal_write("Started process ");
    print_uint((uint32_t)pid);
    terminal_write(".\n");
    irq_restore(flags);

    if (!background) {
        /* Keep the shell prompt hidden until the foreground process exits. */
        while (tasks[pid].state != TASK_ZOMBIE)
            __asm__ volatile ("hlt");
        reap_task(pid);
    }
}

static void shell_ps(void)
{
    terminal_putc('\n');
    terminal_write("PID  NAME      TICKS  STATE\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_FREE) continue;
        print_uint(tasks[i].id); terminal_write("   ");
        terminal_write(tasks[i].name); terminal_write("     ");
        print_uint(tasks[i].ticks); terminal_write("   ");
        if (tasks[i].state == TASK_RUNNING) terminal_write("running");
        else if (tasks[i].state == TASK_READY) terminal_write("ready");
        else terminal_write("zombie");
        terminal_putc('\n');
    }
}

static int shell_kill(uint32_t pid)
{
    if (pid < 4 || pid >= MAX_TASKS || tasks[pid].state == TASK_FREE) return 0;
    if (tasks[pid].state == TASK_ZOMBIE) return 1;
    tasks[pid].exit_code = 137;
    tasks[pid].state = TASK_ZOMBIE;
    terminal_write("Process "); print_uint(pid); terminal_write(" killed.\n");
    return 1;
}

static void shell_wait(uint32_t pid)
{
    if (pid < 4 || pid >= MAX_TASKS || tasks[pid].state == TASK_FREE) {
        terminal_write("No such process.\n"); return;
    }
    terminal_write("Waiting for process "); print_uint(pid); terminal_write("...\n");
    while (tasks[pid].state != TASK_ZOMBIE)
        __asm__ volatile ("hlt");
    terminal_write("Process "); print_uint(pid); terminal_write(" exited with code ");
    print_uint(tasks[pid].exit_code); terminal_putc('\n');
    reap_task((int)pid);
}

static void shell_delete(void)
{
    if (command_cursor >= command_len)
        return;

    for (int i = command_cursor; i < command_len - 1; i++)
        command[i] = command[i + 1];

    command_len--;
    shell_redraw();
}

static void shell_execute(void)
{
    command[command_len] = '\0';

    if (command_len == 0) {
        terminal_putc('\n');
        shell_prompt();
        return;
    }

    if (str_equal(command, "help")) {
        terminal_putc('\n');
        terminal_write("Available commands:\n");
        terminal_write("  help   - show this help\n");
        terminal_write("  clear  - clear the screen\n");
        terminal_write("  echo   - print text\n");
        terminal_write("  info   - show system information\n");
        terminal_write("  uptime - show elapsed time\n");
        terminal_write("  sleep  - sleep for N seconds\n");
        terminal_write("  meminfo - show heap memory information\n");
        terminal_write("  memtest - test kmalloc/kfree\n");
        terminal_write("  paging  - show paging status\n");
        terminal_write("  ps     - show process list\n");
        terminal_write("  kill   - terminate a process\n");
        terminal_write("  wait   - wait for a process\n");
        terminal_write("  user   - show user-mode information\n");
        terminal_write("  reboot  - reboot the machine\n");
        terminal_write("  poweroff - power off the machine\n");
        terminal_write("  ls     - list files on JFS1 filesystem\n");
        terminal_write("  cat    - display a file\n");
        terminal_write("  fsinfo - show filesystem information\n");
        terminal_write("  touch  - create an empty file\n");
        terminal_write("  write  - write text to a file\n");
        terminal_write("  rm     - remove a file\n");
        terminal_write("  exec   - run a JEXE userspace program (foreground)\n");
        terminal_write("         add & to run it in background\n");
    } else if (str_equal(command, "clear")) {
        terminal_clear();
        shell_prompt();
        return;
    } else if (str_starts(command, "echo ")) {
        terminal_putc('\n');
        terminal_write(command + 5);
    } else if (str_equal(command, "info")) {
        terminal_putc('\n');
        terminal_write("JupiterOS\n");
        terminal_write("Architecture: i386\n");
        terminal_write("Mode: 32-bit protected mode\n");
        terminal_write("Terminal: VGA text mode\n");
        terminal_write("Input: PS/2 keyboard\n");
        terminal_write("Keyboard: IRQ1\n");
        terminal_write("Timer: PIT IRQ0 (100 Hz)\n");
        terminal_write("Interrupts: IDT + PIC\n");
        terminal_write("Heap: 1 MiB kernel heap (kmalloc/kfree)\n");
        terminal_write("Paging: 32-bit identity mapping (first 4 MiB)\n");
        terminal_write("Scheduler: preemptive round-robin (8 task slots)\n");
        terminal_write("User mode: Ring 3 + int 0x80 system calls\n");
        terminal_write("Userspace: JEXE programs loaded from JFS1\n");
        terminal_write("Process management: ps/kill/wait\n");
        terminal_write("Filesystem: JFS1 writable\n");
        terminal_write("Disk: ATA PIO primary master\n");
    } else if (str_equal(command, "uptime")) {
        terminal_putc('\n');
        terminal_write("Uptime: ");
        uint32_t seconds = timer_seconds();
        char digits[11];
        int n = 0;
        if (seconds == 0) {
            terminal_putc('0');
        } else {
            while (seconds && n < 10) {
                digits[n++] = (char)('0' + seconds % 10u);
                seconds /= 10u;
            }
            while (n--)
                terminal_putc(digits[n]);
        }
        terminal_write(" seconds\n");
    } else if (str_starts(command, "sleep ")) {
        int ok = 0;
        uint32_t seconds = parse_uint(command + 6, &ok);
        terminal_putc('\n');
        if (!ok || seconds > 3600u) {
            terminal_write("Usage: sleep N  (N = 0..3600)\n");
        } else {
            terminal_write("Sleeping...\n");
            sleep_seconds(seconds);
            terminal_write("Done.\n");
        }
    } else if (str_equal(command, "meminfo")) {
        terminal_putc('\n');
        terminal_write("Heap start: 0x00100000\n");
        terminal_write("Heap size:  1048576 bytes\n");
        terminal_write("Used:       ");
        print_uint(heap_used);
        terminal_write(" bytes\n");
        terminal_write("Free:       ");
        print_uint(heap_free_bytes());
        terminal_write(" bytes\n");
        terminal_write("Paging:     ");
        terminal_write(paging_enabled ? "enabled\n" : "disabled\n");
        terminal_write("Allocations:");
        print_uint(heap_allocations);
        terminal_putc('\n');
    } else if (str_equal(command, "paging")) {
        terminal_putc('\n');
        terminal_write("Paging: ");
        terminal_write(paging_enabled ? "enabled\n" : "disabled\n");
        terminal_write("Page size: 4096 bytes\n");
        terminal_write("Mapped: first 4 MiB identity-mapped\n");
        terminal_write("CR3: 0x");
        uint32_t cr3 = read_cr3();
        char hex[8];
        for (int i = 7; i >= 0; i--) {
            uint8_t nibble = (uint8_t)(cr3 & 0xFu);
            hex[i] = (nibble < 10) ? (char)('0' + nibble) : (char)('A' + nibble - 10);
            cr3 >>= 4;
        }
        for (int i = 0; i < 8; i++) terminal_putc(hex[i]);
        terminal_putc('\n');
    } else if (str_equal(command, "memtest")) {
        terminal_putc('\n');
        terminal_write("Allocating 256 bytes... ");
        uint8_t *test = (uint8_t *)kmalloc(256);
        if (!test) {
            terminal_write("FAILED (out of memory)\n");
        } else {
            for (int i = 0; i < 256; i++)
                test[i] = (uint8_t)i;

            int valid = 1;
            for (int i = 0; i < 256; i++) {
                if (test[i] != (uint8_t)i) {
                    valid = 0;
                    break;
                }
            }

            kfree(test);
            terminal_write(valid ? "OK\n" : "FAILED\n");
            terminal_write("Memory block freed.\n");
        }
    } else if (str_equal(command, "ps")) {
        shell_ps();
    } else if (str_equal(command, "user")) {
        terminal_putc('\n');
        terminal_write("User mode: enabled\n");
        terminal_write("Ring 3 code: 0x00400000\n");
        terminal_write("Ring 3 stack: 0x00402000\n");
        terminal_write("Syscall ABI: int 0x80\n");
        terminal_write("Program format: JEXE v1\n");
    } else if (str_starts(command, "kill ")) {
        int ok = 0; uint32_t pid = parse_uint(command + 5, &ok);
        terminal_putc('\n');
        if (!ok || !shell_kill(pid)) terminal_write("Usage: kill PID\n");
    } else if (str_starts(command, "wait ")) {
        int ok = 0; uint32_t pid = parse_uint(command + 5, &ok);
        terminal_putc('\n');
        if (!ok) terminal_write("Usage: wait PID\n"); else shell_wait(pid);
    } else if (str_starts(command, "exec ")) {
        shell_exec(command + 5);
        /* shell_exec() already positioned us after the command/output. */
        shell_prompt();
        return;
    } else if (str_equal(command, "ls")) {
        fs_ls();
    } else if (str_starts(command, "cat ")) {
        fs_cat(command + 4);
    } else if (str_starts(command, "touch ")) {
        terminal_putc('\n');
        if (fs_create_empty(command + 6))
            terminal_write("File created.\n");
        else
            terminal_write("Unable to create file.\n");
    } else if (str_starts(command, "write ")) {
        terminal_putc('\n');
        const char *args = command + 6;
        const char *space = 0;
        for (const char *p = args; *p; p++) {
            if (*p == ' ') {
                space = p;
                break;
            }
        }
        if (!space || space == args || !space[1]) {
            terminal_write("Usage: write FILE TEXT\n");
        } else {
            int len = (int)(space - args);
            if (len > (int)FS_NAME_MAX) {
                terminal_write("Filename too long.\n");
            } else {
                char name[FS_NAME_MAX + 1];
                for (int i = 0; i < len; i++) name[i] = args[i];
                name[len] = '\0';
                if (fs_write_text(name, space + 1))
                    terminal_write("File written.\n");
                else
                    terminal_write("Unable to write file.\n");
            }
        }
    } else if (str_starts(command, "rm ")) {
        terminal_putc('\n');
        if (fs_remove(command + 3))
            terminal_write("File removed.\n");
        else
            terminal_write("File not found.\n");
    } else if (str_equal(command, "fsinfo")) {
        terminal_putc('\n');
        if (!fs_load()) {
            terminal_write("Filesystem: unavailable\n");
        } else {
            terminal_write("Filesystem: JFS1\n");
            terminal_write("Version:   1\n");
            terminal_write("Start LBA: ");
            print_uint(FS_START_LBA);
            terminal_putc('\n');
            terminal_write("Directory: ");
            print_uint(fs_super.dir_lba);
            terminal_putc('\n');
            terminal_write("Entries:   ");
            print_uint(fs_super.dir_entries);
            terminal_putc('\n');
            terminal_write("Data LBA:  ");
            print_uint(fs_super.data_lba);
            terminal_putc('\n');
            terminal_write("Sectors:   ");
            print_uint(fs_super.total_sectors);
            terminal_putc('\n');
        }
    } else if (str_equal(command, "poweroff")) {
        terminal_putc('\n');
        terminal_write("Powering off...\n");
        poweroff();
    } else if (str_equal(command, "reboot")) {
        terminal_putc('\n');
        terminal_write("Rebooting...\n");
        reboot();
    } else {
        terminal_putc('\n');
        terminal_write("Unknown command. Type 'help'.");
    }

    terminal_putc('\n');
    shell_prompt();
}

void kmain(void)
{
    terminal_clear();
    heap_init();
    gdt_tss_init();
    paging_init();

    terminal_write("JupiterOS\n");
    terminal_write("========================================\n");
    terminal_write("Welcome to JupiterOS!\n");
    terminal_write("Keyboard interrupts are enabled.\n");
    terminal_write("Type 'help' for available commands.\n\n");

    /*
     * Build the interrupt system before enabling interrupts.
     */
    idt_init();
    pic_init();
    pit_init();
    scheduler_init();

    terminal_write("Filesystem: JFS1\n");

    shell_prompt();

    __asm__ volatile ("sti");

    for (;;) {
        uint16_t key = keyboard_getkey();

        if (key == KEY_LEFT) {
            shell_move_left();
        } else if (key == KEY_RIGHT) {
            shell_move_right();
        } else if (key == KEY_DELETE) {
            shell_delete();
        } else if (key == '\b') {
            shell_backspace();
        } else if (key == '\n') {
            shell_execute();
            command_len = 0;
            command_cursor = 0;
        } else if (key < 128 && key >= 32) {
            shell_insert_char((char)key);
        }
    }
}

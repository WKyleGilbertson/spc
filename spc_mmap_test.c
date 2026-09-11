#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

#define SPC_MAP_SIZE 0x1000

// Channel 0 Offsets (0x41600000)
#define REG_CONFIG      0x000 // [1:0] = nms, [7:2] = spacing
#define REG_CARR_FREQ   0x004 // 32-bit Carrier NCO
#define REG_CODE_FREQ   0x008 // 32-bit Code NCO
#define REG_SEEDS       0x00C // Phase & LFSR seeds

int main()
{
    int fd = open("/dev/spc", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("Failed to open /dev/spc");
        return 1;
    }

    volatile uint8_t *base = (volatile uint8_t *)mmap(
        NULL, SPC_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (base == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return 1;
    }

    printf("=== Testing Direct APB mmap Read/Write ===\n");

    // 1. Test Carrier NCO register at 0x004
    volatile uint32_t *carr_reg = (volatile uint32_t *)(base + REG_CARR_FREQ);
    uint32_t orig_carr = *carr_reg;
    uint32_t test_carr = 0x40010000; // Offset test frequency

    *carr_reg = test_carr;
    uint32_t read_carr = *carr_reg;
    printf("Carrier NCO (0x04): Wrote 0x%08X | Read 0x%08X -> %s\n",
           test_carr, read_carr, (test_carr == read_carr) ? "PASS" : "FAIL");

    // Restore original carrier value
    *carr_reg = orig_carr;

    // 2. Test Code NCO register at 0x008
    volatile uint32_t *code_reg = (volatile uint32_t *)(base + REG_CODE_FREQ);
    uint32_t orig_code = *code_reg;
    uint32_t test_code = 0x10000800;

    *code_reg = test_code;
    uint32_t read_code = *code_reg;
    printf("Code NCO    (0x08): Wrote 0x%08X | Read 0x%08X -> %s\n",
           test_code, read_code, (test_code == read_code) ? "PASS" : "FAIL");

    // Restore original code value
    *code_reg = orig_code;

    munmap((void *)base, SPC_MAP_SIZE);
    close(fd);
    return 0;
}
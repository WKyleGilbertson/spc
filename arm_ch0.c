#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

#define SPC_MAP_SIZE 0x1000

// Channel 0 Offsets (0x00 to 0x7F)
#define REG_CONFIG      0x000 // [1:0] = nms, [7:2] = spacing
#define REG_CARR_FREQ   0x004 // 32-bit carrier freq
#define REG_CODE_FREQ   0x008 // 32-bit code freq
#define REG_SEEDS       0x00C // [9:0] = phase, [19:10] = g1, [29:20] = g2

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

    printf("Arming Channel 0 with correct spc_ctrlmux mapping...\n");

    // 1. Config: nms = 0 (1ms), spacing = 8 (0.5 chip -> 8 << 2 = 0x20)
    *(volatile uint32_t *)(base + REG_CONFIG) = (8 << 2) | 0;

    // 2. Carrier: 4.092 MHz center IF
    *(volatile uint32_t *)(base + REG_CARR_FREQ) = 0x40000000;

    // 3. Code: 1.023 MHz chipping rate
    *(volatile uint32_t *)(base + REG_CODE_FREQ) = 0x10000000;

    // 4. Seeds: PRN 7 (G1 = 0x3FF, G2 = 0x288, Code Phase = 0)
    uint32_t code_phase = 0;
    uint32_t g1 = 0x3FF;
    uint32_t g2 = 0x288;
    *(volatile uint32_t *)(base + REG_SEEDS) = (code_phase & 0x3FF) | 
                                               ((g1 & 0x3FF) << 10) | 
                                               ((g2 & 0x3FF) << 20);

    // Readback verification
    printf("Readback Config     (0x00): 0x%08X\n", *(volatile uint32_t *)(base + REG_CONFIG));
    printf("Readback Carr Freq  (0x04): 0x%08X\n", *(volatile uint32_t *)(base + REG_CARR_FREQ));
    printf("Readback Code Freq  (0x08): 0x%08X\n", *(volatile uint32_t *)(base + REG_CODE_FREQ));
    printf("Readback Seeds      (0x0C): 0x%08X\n", *(volatile uint32_t *)(base + REG_SEEDS));

    munmap((void *)base, SPC_MAP_SIZE);
    close(fd);
    return 0;
}
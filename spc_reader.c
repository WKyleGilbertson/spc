#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include "spc.h"

int main()
{
    int fd = open("/dev/spc", O_RDONLY);
    if (fd < 0) {
        perror("Failed to open /dev/spc");
        return 1;
    }

    printf("Connected to /dev/spc. Streaming 1ms epoch frames...\n");
    printf("Epoch Seq  | Ch0 IP     | Ch0 QP     | Ch0 IE     | Ch0 IL     | Epoch Idx\n");
    printf("-----------+------------+------------+------------+------------+----------\n");

    struct spc_epoch_data frame;
    uint32_t last_seq = 0;
    uint32_t frame_count = 0;
    time_t last_time = time(NULL);

    while (read(fd, &frame, sizeof(frame)) == sizeof(frame)) {
        frame_count++;

        // Detect dropped frames in kernel kfifo
        if (last_seq != 0 && frame.system_epoch != last_seq + 1) {
            printf("[!] DROP DETECTED: Jumped from %u to %u (%u lost)\n",
                   last_seq, frame.system_epoch, frame.system_epoch - last_seq - 1);
        }
        last_seq = frame.system_epoch;

        // Print telemetry snapshot every 200 ms
        if (frame.system_epoch % 200 == 0) {
            printf("%10u | %10d | %10d | %10d | %10d | %8u\n",
                   frame.system_epoch,
                   frame.ch[0].ip,
                   frame.ch[0].qp,
                   frame.ch[0].ie,
                   frame.ch[0].il,
                   frame.ch[0].epoch_idx);
        }

        // Print epoch delivery rate once per second
        time_t now = time(NULL);
        if (now != last_time) {
            // Uncomment to monitor exact ingest frequency:
            // printf("-> Ingest Rate: %u frames/sec\n", frame_count);
            frame_count = 0;
            last_time = now;
        }
    }

    close(fd);
    return 0;
}
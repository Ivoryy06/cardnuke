#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

static double get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void drop_caches(void) {
    sync();
    FILE *f = fopen("/proc/sys/vm/drop_caches", "w");
    if (f) {
        fprintf(f, "3");
        fclose(f);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <device_mount_path> <write|read|both>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    const char *mode = argv[2];
    const size_t SIZE = 256 * 1024 * 1024;
    const size_t CHUNK = 8 * 1024 * 1024;

    char *buf = aligned_alloc(512, CHUNK);
    if (!buf) {
        perror("aligned_alloc");
        return 1;
    }

    for (size_t i = 0; i < CHUNK; i++) {
        buf[i] = (i * 31) & 0xFF;
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/.speed_test", path);

    double write_speed = 0, read_speed = 0;
    int direct_ok = 0;

    if (!strcmp(mode, "write") || !strcmp(mode, "both")) {
        unlink(filepath);
        int fd = open(filepath, O_RDWR | O_CREAT | O_DIRECT, 0644);
        if (fd < 0) {
            fd = open(filepath, O_RDWR | O_CREAT, 0644);
        } else {
            direct_ok = 1;
        }
        if (fd < 0) {
            perror("open write");
            free(buf);
            return 1;
        }

        double t0 = get_time();
        size_t written = 0;
        while (written < SIZE) {
            ssize_t w = write(fd, buf, CHUNK);
            if (w <= 0) {
                perror("write");
                close(fd);
                free(buf);
                return 1;
            }
            written += w;
        }
        fsync(fd);
        double t1 = get_time();
        write_speed = SIZE / (t1 - t0) / 1024 / 1024;
        close(fd);
        fprintf(stderr, "Write: Direct I/O %s\n", direct_ok ? "enabled" : "disabled (fallback)");
    }

    if (!strcmp(mode, "read") || !strcmp(mode, "both")) {
        drop_caches();
        usleep(50000);

        int fd = open(filepath, O_RDONLY | O_DIRECT);
        if (fd < 0) {
            fd = open(filepath, O_RDONLY);
        }
        if (fd < 0) {
            perror("open read");
            free(buf);
            return 1;
        }

        lseek(fd, 0, SEEK_SET);
        double t0 = get_time();
        size_t total = 0;
        while (total < SIZE) {
            ssize_t r = read(fd, buf, CHUNK);
            if (r <= 0) break;
            total += r;
        }
        double t1 = get_time();
        read_speed = SIZE / (t1 - t0) / 1024 / 1024;
        close(fd);
        unlink(filepath);
    }

    if (!strcmp(mode, "both")) {
        printf("%.1f %.1f\n", write_speed, read_speed);
    } else if (!strcmp(mode, "write")) {
        printf("%.1f\n", write_speed);
    } else if (!strcmp(mode, "read")) {
        printf("%.1f\n", read_speed);
    }

    free(buf);
    return 0;
}
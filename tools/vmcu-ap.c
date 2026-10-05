/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "rpc.h"

static volatile sig_atomic_t stopping;

static void stop(int signo)
{
    (void)signo;
    stopping = 1;
}

static uint64_t milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool locate_uart(char *path, size_t size)
{
    glob_t matches = { 0 };
    bool found = false;
    if (glob("/sys/class/tty/ttyS*/device", 0, NULL, &matches)) {
        return false;
    }
    for (size_t i = 0; i < matches.gl_pathc; i++) {
        char resolved[PATH_MAX];
        const char *name = matches.gl_pathv[i] + strlen("/sys/class/tty/");
        const char *end = strchr(name, '/');
        if (realpath(matches.gl_pathv[i], resolved) &&
            strstr(resolved, "/301c0000.serial/") && end) {
            snprintf(path, size, "/dev/%.*s", (int)(end - name), name);
            found = true;
            break;
        }
        /* Some kernels expose device as the platform device itself. */
        if (realpath(matches.gl_pathv[i], resolved) && end) {
            const char *base = strrchr(resolved, '/');
            if (base && !strcmp(base + 1, "301c0000.serial")) {
                snprintf(path, size, "/dev/%.*s", (int)(end - name), name);
                found = true;
                break;
            }
        }
    }
    globfree(&matches);
    return found;
}

struct output {
    uint8_t bytes[1024];
    size_t start, used;
};

static bool enqueue(struct output *output, const struct vmcu_frame *frame)
{
    if (output->used + VMCU_FRAME_SIZE > sizeof(output->bytes)) {
        return false;
    }
    if (output->start + output->used + VMCU_FRAME_SIZE > sizeof(output->bytes)) {
        memmove(output->bytes, output->bytes + output->start, output->used);
        output->start = 0;
    }
    vmcu_encode(output->bytes + output->start + output->used, frame);
    output->used += VMCU_FRAME_SIZE;
    return true;
}

/* No shell execution and no host-side process restart. The calling vMCU must
 * arm SI CL0 first; Linux's PSCI shutdown is then intercepted by SI policy. */
static int power_off(int fd)
{
    uint64_t deadline = milliseconds() + 2000;
    for (;;) {
        int pending, line_status;
        if (ioctl(fd, TIOCOUTQ, &pending) < 0)
            return -1;
        if (!pending && ioctl(fd, TIOCSERGETLSR, &line_status) == 0 &&
            (line_status & TIOCSER_TEMT))
            break;
        if (milliseconds() >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct timespec pause = { .tv_nsec = 1000000 };
        nanosleep(&pause, NULL);
    }
    puts("VMCU_AP_SHUTDOWN result=UART_DRAINED action=kernel-poweroff");
    sync();
    return reboot(RB_POWER_OFF);
}

static int run_link(int fd, uint32_t epoch, const char *device)
{
    struct vmcu_rpc_peer peer;
    struct vmcu_stream stream = { 0 };
    struct output output = { 0 };
    uint64_t hello_at = 0;
    bool acknowledged = false;

    vmcu_rpc_init(&peer, epoch);
    while (!stopping) {
        uint64_t now = milliseconds();
        struct vmcu_frame frame = {
            .type = VMCU_HELLO, .sender = VMCU_AP,
            .epoch = epoch, .sequence = 1
        };
        if (now >= hello_at) {
            if (!enqueue(&output, &frame)) {
                return -1;
            }
            /* Re-HELLO discovers a restarted management client. Safety
             * monitoring and reporting belong exclusively to SI CL0. */
            hello_at = now + (acknowledged ? 1000 : VMCU_PERIOD_MS);
        }
        struct pollfd descriptor = {
            .fd = fd, .events = POLLIN | (output.used ? POLLOUT : 0)
        };
        int rc = poll(&descriptor, 1, 20);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            return -1;
        }
        if (descriptor.revents & POLLOUT) {
            ssize_t written = write(fd, output.bytes + output.start, output.used);
            if (written > 0) {
                output.start += written;
                output.used -= written;
                if (!output.used) {
                    output.start = 0;
                }
            } else if (written < 0 && errno != EAGAIN && errno != EINTR) {
                return -1;
            }
        }
        if (peer.poweroff_requested && !output.used) {
            if (power_off(fd) < 0) {
                perror("VMCU_AP_SHUTDOWN result=FAIL");
                return -1;
            }
        }
        if (descriptor.revents & POLLIN) {
            uint8_t bytes[256];
            ssize_t count = read(fd, bytes, sizeof(bytes));
            if (count < 0 && errno != EAGAIN && errno != EINTR) {
                return -1;
            }
            for (ssize_t i = 0; i < count; i++) {
                if (!vmcu_feed(&stream, bytes[i], &frame)) {
                    continue;
                }
                if (frame.type == VMCU_HELLO_ACK && frame.sender == VMCU_MCU &&
                    frame.epoch == epoch) {
                    if (!acknowledged) {
                        printf("VMCU_AP_LINK epoch=%u device=%s\n", epoch, device);
                    }
                    acknowledged = true;
                } else if (acknowledged) {
                    struct vmcu_frame reply;
                    if (vmcu_rpc_receive(&peer, &frame, &reply) &&
                        !enqueue(&output, &reply)) {
                        return -1;
                    }
                }
            }
        }
    }
    return 0;
}

static bool parse_epoch(const char *text, uint32_t *epoch)
{
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !*text || *end || !value || value > UINT32_MAX) {
        return false;
    }
    *epoch = value;
    return true;
}

int main(int argc, char **argv)
{
    const char *device = NULL;
    const char *lock_path = "/run/vmcu-ap.lock";
    const char *pid_path = "/run/vmcu-ap.pid";
    uint32_t epoch = 0;
    char detected[128];
    int result = 1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            puts("vmcu-ap [--device PATH] [--epoch N] [--lock-file PATH] [--pid-file PATH]");
            return 0;
        }
        if (i + 1 == argc) {
            fprintf(stderr, "missing option value\n");
            return 2;
        }
        if (!strcmp(argv[i], "--device")) {
            device = argv[++i];
        } else if (!strcmp(argv[i], "--lock-file")) {
            lock_path = argv[++i];
        } else if (!strcmp(argv[i], "--pid-file")) {
            pid_path = argv[++i];
        } else if (!strcmp(argv[i], "--epoch")) {
            if (!parse_epoch(argv[++i], &epoch)) {
                fprintf(stderr, "invalid epoch\n");
                return 2;
            }
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    signal(SIGHUP, stop);
    int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "VMCU_AP_START result=FAIL reason=lock\n");
        if (lock_fd >= 0) {
            close(lock_fd);
        }
        return 1;
    }
    if (!epoch) {
        char text[32] = { 0 };
        uint32_t previous = 0;
        ssize_t count = read(lock_fd, text, sizeof(text) - 1);
        if (count > 0) {
            (void)parse_epoch(text, &previous);
        }
        epoch = (uint32_t)time(NULL);
        if (!vmcu_newer(epoch, previous)) {
            epoch = previous + 1;
        }
        if (!epoch) {
            epoch = 1;
        }
    }
    if (ftruncate(lock_fd, 0) || lseek(lock_fd, 0, SEEK_SET) < 0 ||
        dprintf(lock_fd, "%u", epoch) < 0) {
        perror("epoch storage");
        goto finish;
    }
    FILE *pid_file = fopen(pid_path, "w");
    if (!pid_file) {
        perror("pid file");
        goto finish;
    }
    fprintf(pid_file, "%ld\n", (long)getpid());
    fclose(pid_file);
    if (!device) {
        uint64_t deadline = milliseconds() + 30000;
        while (!stopping && milliseconds() < deadline &&
               !locate_uart(detected, sizeof(detected))) {
            poll(NULL, 0, 250);
        }
        if (stopping || !locate_uart(detected, sizeof(detected))) {
            puts("VMCU_AP_START result=SKIP reason=uart-unavailable");
            result = 0;
            goto remove_pid;
        }
        device = detected;
    }
    /* Reopen transiently disconnected UARTs, but never spin indefinitely on
     * device/open failures. A present UART may wait quietly for HELLO_ACK.
     */
    for (unsigned attempt = 0; !stopping && attempt < 30; attempt++) {
        struct termios previous, raw;
        int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            poll(NULL, 0, 1000);
            continue;
        }
        if (tcgetattr(fd, &previous)) {
            close(fd);
            break;
        }
        raw = previous;
        cfmakeraw(&raw);
        cfsetispeed(&raw, B115200);
        cfsetospeed(&raw, B115200);
        raw.c_cflag |= CLOCAL | CREAD;
        raw.c_cflag &= ~CRTSCTS;
        if (tcsetattr(fd, TCSANOW, &raw)) {
            close(fd);
            break;
        }
        tcflush(fd, TCIOFLUSH);
        result = run_link(fd, epoch, device) ? 1 : 0;
        tcsetattr(fd, TCSANOW, &previous);
        close(fd);
        if (!result || stopping) {
            break;
        }
        puts("VMCU_AP_LINK result=RECONNECT");
        /* A reopened session must not reuse HEALTH sequence numbers. */
        if (!++epoch) {
            epoch = 1;
        }
        if (ftruncate(lock_fd, 0) || lseek(lock_fd, 0, SEEK_SET) < 0 ||
            dprintf(lock_fd, "%u", epoch) < 0) {
            break;
        }
        poll(NULL, 0, 1000);
    }
remove_pid:
    unlink(pid_path);
finish:
    close(lock_fd);
    return stopping ? 0 : result;
}

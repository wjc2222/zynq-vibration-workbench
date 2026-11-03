/* SPDX-License-Identifier: MIT
 * One bounded, explicitly virtual Type-B touchscreen tap for board-side QA.
 * A successful run proves injection only, not Qt handling or physical USB/HDMI.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#if !defined(UI_DEV_SETUP) || !defined(UI_ABS_SETUP) || \
    !defined(UI_GET_SYSNAME) || !defined(UI_GET_VERSION)
#error "Linux uinput v5 UAPI headers are required"
#endif

#define DEVICE_NAME "AX7Z020 virtual touch validation"
#define WIDTH 1280U
#define HEIGHT 720U
#define DISCOVERY_MS 3000U
#define PRESS_MS 100U
#define MAX_HOLD_SECONDS 8U
#define WATCHDOG_SECONDS 14U
#define POINTER_ARG(p) ((unsigned long)(uintptr_t)(p))

static volatile sig_atomic_t stop_signal;
static int64_t deadline_ms;

static void stop_handler(int number)
{
    stop_signal = number;
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s --tap X Y [--hold-seconds N]\n"
            "       %s --help\n"
            "X: 0..1279, Y: 0..719; N: 0..8 (default 1).\n"
            "Wait 3 seconds for discovery, tap for 100 ms, then retain the\n"
            "virtual device for N seconds before destroying it. A 14-second\n"
            "watchdog bounds the whole run. SIGINT/SIGTERM also clean up.\n"
            "Run only on the intended board with an input consumer ready.\n"
            "This validates virtual input injection; inspect Qt/framebuffer\n"
            "separately. It does not validate a physical screen or USB touch.\n",
            program, program);
}

static int parse_unsigned(const char *text, unsigned int maximum,
                          unsigned int *result)
{
    unsigned int value = 0U;
    const unsigned char *cursor = (const unsigned char *)text;

    if (*cursor == '\0')
        return -1;
    while (*cursor != '\0') {
        unsigned int digit;

        if (*cursor < '0' || *cursor > '9')
            return -1;
        digit = (unsigned int)(*cursor - '0');
        if (digit > maximum || value > (maximum - digit) / 10U)
            return -1;
        value = value * 10U + digit;
        ++cursor;
    }
    *result = value;
    return 0;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int check_deadline(void)
{
    int64_t now;

    if (stop_signal != 0) {
        errno = EINTR;
        return -1;
    }
    now = monotonic_ms();
    if (now < 0)
        return -1;
    if (now >= deadline_ms) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static int delay_ms(unsigned int duration_ms)
{
    int64_t end = monotonic_ms();

    if (end < 0)
        return -1;
    end += duration_ms;
    for (;;) {
        struct timespec delay;
        int64_t now;
        int64_t remaining;

        if (check_deadline() < 0)
            return -1;
        now = monotonic_ms();
        if (now < 0)
            return -1;
        remaining = end - now;
        if (remaining <= 0)
            return 0;
        if (remaining > 100)
            remaining = 100;
        delay.tv_sec = 0;
        delay.tv_nsec = (long)remaining * 1000000L;
        if (nanosleep(&delay, NULL) < 0 && errno != EINTR)
            return -1;
    }
}

static int ioctl_retry(int fd, unsigned long request, unsigned long argument)
{
    int result;

    do {
        if (check_deadline() < 0)
            return -1;
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR && stop_signal == 0);
    return result;
}

static int setup_axis(int fd, unsigned int code, int minimum, int maximum,
                      int initial)
{
    struct uinput_abs_setup axis;

    memset(&axis, 0, sizeof(axis));
    axis.code = (__u16)code;
    axis.absinfo.minimum = minimum;
    axis.absinfo.maximum = maximum;
    axis.absinfo.value = initial;
    if (ioctl_retry(fd, UI_SET_ABSBIT, code) < 0)
        return -1;
    return ioctl_retry(fd, UI_ABS_SETUP, POINTER_ARG(&axis));
}

static int create_touch(int fd)
{
    struct uinput_setup setup;
    unsigned int protocol = 0;

    if (ioctl_retry(fd, UI_GET_VERSION, POINTER_ARG(&protocol)) < 0)
        return -1;
    if (protocol < 5U) {
        fprintf(stderr, "uinput protocol %u is too old; version 5 is required\n",
                protocol);
        errno = ENOTSUP;
        return -1;
    }
    printf("uinput_protocol=%u\n", protocol);
    if (ioctl_retry(fd, UI_SET_EVBIT, EV_SYN) < 0 ||
        ioctl_retry(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl_retry(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl_retry(fd, UI_SET_KEYBIT, BTN_TOUCH) < 0 ||
        ioctl_retry(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0 ||
        setup_axis(fd, ABS_X, 0, (int)WIDTH - 1, 0) < 0 ||
        setup_axis(fd, ABS_Y, 0, (int)HEIGHT - 1, 0) < 0 ||
        setup_axis(fd, ABS_MT_SLOT, 0, 0, 0) < 0 ||
        setup_axis(fd, ABS_MT_TRACKING_ID, 0, 65535, -1) < 0 ||
        setup_axis(fd, ABS_MT_POSITION_X, 0, (int)WIDTH - 1, 0) < 0 ||
        setup_axis(fd, ABS_MT_POSITION_Y, 0, (int)HEIGHT - 1, 0) < 0)
        return -1;
    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, sizeof(setup.name), "%s", DEVICE_NAME);
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.version = 1;
    if (ioctl_retry(fd, UI_DEV_SETUP, POINTER_ARG(&setup)) < 0)
        return -1;
    return ioctl_retry(fd, UI_DEV_CREATE, 0);
}

static int write_events(int fd, const struct input_event *events, size_t count)
{
    const unsigned char *cursor = (const unsigned char *)events;
    size_t remaining = count * sizeof(*events);

    while (remaining != 0U) {
        ssize_t written;

        if (check_deadline() < 0)
            return -1;
        written = write(fd, cursor, remaining);
        if (written > 0) {
            /* Never re-submit a truncated event as a fresh uinput record. */
            if ((size_t)written % sizeof(*events) != 0U) {
                errno = EIO;
                return -1;
            }
            cursor += (size_t)written;
            remaining -= (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd waiter = { .fd = fd, .events = POLLOUT, .revents = 0 };
            int ready = poll(&waiter, 1, 100);

            if (ready < 0 && errno != EINTR)
                return -1;
            if (ready > 0 && (waiter.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                errno = EIO;
                return -1;
            }
        } else {
            if (written == 0)
                errno = EIO;
            return -1;
        }
    }
    return 0;
}

static void event_set(struct input_event *event, unsigned int type,
                      unsigned int code, int value)
{
    memset(event, 0, sizeof(*event));
    event->type = (__u16)type;
    event->code = (__u16)code;
    event->value = value;
}

static int tap_down(int fd, unsigned int x, unsigned int y)
{
    struct input_event events[8];

    /* BTN_TOUCH first also preserves the kernel's legacy mousedev contract. */
    event_set(&events[0], EV_KEY, BTN_TOUCH, 1);
    event_set(&events[1], EV_ABS, ABS_MT_SLOT, 0);
    event_set(&events[2], EV_ABS, ABS_MT_TRACKING_ID, 1);
    event_set(&events[3], EV_ABS, ABS_MT_POSITION_X, (int)x);
    event_set(&events[4], EV_ABS, ABS_MT_POSITION_Y, (int)y);
    event_set(&events[5], EV_ABS, ABS_X, (int)x);
    event_set(&events[6], EV_ABS, ABS_Y, (int)y);
    event_set(&events[7], EV_SYN, SYN_REPORT, 0);
    return write_events(fd, events, sizeof(events) / sizeof(events[0]));
}

static int tap_up(int fd)
{
    struct input_event events[4];

    event_set(&events[0], EV_KEY, BTN_TOUCH, 0);
    event_set(&events[1], EV_ABS, ABS_MT_SLOT, 0);
    event_set(&events[2], EV_ABS, ABS_MT_TRACKING_ID, -1);
    event_set(&events[3], EV_SYN, SYN_REPORT, 0);
    return write_events(fd, events, sizeof(events) / sizeof(events[0]));
}

int main(int argc, char **argv)
{
    unsigned int x = 0;
    unsigned int y = 0;
    unsigned int hold_seconds = 1;
    int have_tap = 0;
    int have_hold = 0;
    int fd = -1;
    int created = 0;
    int result = EXIT_FAILURE;
    int saved_errno;
    const char *device_path = "/dev/uinput";
    char sysname[128] = { 0 };
    struct sigaction action;
    int index;
    int64_t started;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--tap") == 0 && !have_tap &&
            index + 2 < argc) {
            if (parse_unsigned(argv[index + 1], WIDTH - 1U, &x) < 0 ||
                parse_unsigned(argv[index + 2], HEIGHT - 1U, &y) < 0)
                goto bad_arguments;
            have_tap = 1;
            index += 2;
        } else if (strcmp(argv[index], "--hold-seconds") == 0 && !have_hold &&
                   index + 1 < argc) {
            if (parse_unsigned(argv[index + 1], MAX_HOLD_SECONDS,
                               &hold_seconds) < 0)
                goto bad_arguments;
            have_hold = 1;
            ++index;
        } else {
            goto bad_arguments;
        }
    }
    if (!have_tap)
        goto bad_arguments;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("scope=VIRTUAL_INPUT_ONLY; verify Qt/framebuffer separately; "
           "physical screen/USB touch NOT validated\n");
    started = monotonic_ms();
    if (started < 0) {
        perror("CLOCK_MONOTONIC");
        return EXIT_FAILURE;
    }
    deadline_ms = started + (int64_t)WATCHDOG_SECONDS * 1000;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGALRM, &action, NULL) < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    alarm(WATCHDOG_SECONDS);
    do {
        fd = open(device_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR && stop_signal == 0);
    if (fd < 0 && errno == ENOENT && stop_signal == 0) {
        device_path = "/dev/input/uinput";
        do {
            fd = open(device_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        } while (fd < 0 && errno == EINTR && stop_signal == 0);
    }
    if (fd < 0) {
        fprintf(stderr, "open %s: %s (requires CONFIG_INPUT_UINPUT and access)\n",
                device_path, strerror(errno));
        goto cleanup;
    }
    if (create_touch(fd) < 0) {
        perror("configure/create virtual touchscreen");
        goto cleanup;
    }
    created = 1;
    if (ioctl_retry(fd, UI_GET_SYSNAME(sizeof(sysname)),
                    POINTER_ARG(sysname)) < 0) {
        perror("UI_GET_SYSNAME");
        goto cleanup;
    }
    sysname[sizeof(sysname) - 1U] = '\0';
    printf("created name=\"%s\" bus=BUS_VIRTUAL sysname=%s "
           "sysfs=/sys/devices/virtual/input/%s node=%s\n",
           DEVICE_NAME, sysname, sysname, device_path);
    printf("tap=(%u,%u) logical_size=%ux%u single_type_b_slot=0 "
           "discovery_ms=%u press_ms=%u post_tap_hold_seconds=%u\n",
           x, y, WIDTH, HEIGHT, DISCOVERY_MS, PRESS_MS, hold_seconds);
    if (delay_ms(DISCOVERY_MS) < 0 || tap_down(fd, x, y) < 0 ||
        delay_ms(PRESS_MS) < 0 || tap_up(fd) < 0) {
        perror("virtual tap");
        goto cleanup;
    }
    printf("tap_sent=(%u,%u) down_and_release_complete; "
           "consumer acceptance still requires separate evidence\n", x, y);
    if (delay_ms(hold_seconds * 1000U) < 0) {
        perror("post-tap hold");
        goto cleanup;
    }
    result = EXIT_SUCCESS;

cleanup:
    saved_errno = errno;
    if (created) {
        int destroy_result;
        unsigned int attempts = 0;

        /* close() also removes a uinput device, including after failed setup. */
        do {
            destroy_result = ioctl(fd, UI_DEV_DESTROY);
            ++attempts;
        } while (destroy_result < 0 && errno == EINTR && attempts < 3U);
        if (destroy_result < 0) {
            perror("UI_DEV_DESTROY (close will also remove the device)");
            result = EXIT_FAILURE;
        } else {
            printf("destroyed sysname=%s\n", sysname);
        }
    }
    if (fd >= 0 && close(fd) < 0) {
        /* Do not retry close(): Linux releases the fd even on EINTR. */
        perror("close uinput");
        result = EXIT_FAILURE;
    }
    alarm(0);
    if (stop_signal != 0) {
        fprintf(stderr, "interrupted signal=%d; device/fd cleanup attempted\n",
                (int)stop_signal);
        return 128 + (int)stop_signal;
    }
    errno = saved_errno;
    if (result == EXIT_SUCCESS) {
        int64_t finished = monotonic_ms();

        if (finished < 0) {
            perror("final CLOCK_MONOTONIC");
            return EXIT_FAILURE;
        }
        printf("complete virtual_tap=(%u,%u) elapsed_ms=%" PRId64
               "; physical screen/USB touch NOT validated\n",
               x, y, finished - started);
    }
    return result;

bad_arguments:
    fprintf(stderr, "Invalid or duplicate arguments; no input device was opened.\n");
    usage(stderr, argv[0]);
    return 2;
}

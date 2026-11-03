// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void die(const char *name) { perror(name); exit(1); }
int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/fb0";
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    uint32_t *map, crtc = 0;
    size_t frame_bytes;
    int fd, page, x, y;
    static const uint32_t colors[] = {
        0x00ff0000, 0x0000ff00, 0x000000ff, 0x00ffffff,
        0x00ffff00, 0x0000ffff, 0x00ff00ff, 0x00000000
    };
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) die(path);
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix)) die("FBIOGET_FSCREENINFO");
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var)) die("FBIOGET_VSCREENINFO");
    if (var.xres != 1280 || var.yres != 720 || var.bits_per_pixel != 32 ||
        var.yres_virtual != 1440 || fix.line_length != 5120 ||
        var.red.offset != 16 || var.green.offset != 8 || var.blue.offset != 0 ||
        var.red.length != 8 || var.green.length != 8 || var.blue.length != 8) {
        fprintf(stderr, "Unexpected framebuffer geometry/format; refusing to write\n");
        close(fd); return 2;
    }
    frame_bytes = (size_t)fix.line_length * var.yres;
    if (fix.smem_len < 2 * frame_bytes) {
        fprintf(stderr, "Framebuffer has insufficient backing memory\n"); close(fd); return 2;
    }
    map = mmap(NULL, 2 * frame_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) die("mmap WC framebuffer");
    for (page = 0; page < 2; page++)
        for (y = 0; y < 720; y++)
            for (x = 0; x < 1280; x++)
                map[(page * 720 + y) * 1280 + x] = colors[(x / 160 + page) % 8];
    for (page = 0; page < 2; page++)
        for (x = 0; x < 8; x++)
            if (map[page * 1280 * 720 + 80 + x * 160] != colors[(x + page) % 8]) {
                fprintf(stderr, "CPU mmap color readback failed\n"); return 3;
            }
    for (page = 0; page < 4; page++) {
        var.yoffset = (page % 2) * 720;
        var.activate = FB_ACTIVATE_NOW;
        if (ioctl(fd, FBIOPAN_DISPLAY, &var)) die("FBIOPAN_DISPLAY park/current acknowledgement");
        if (ioctl(fd, FBIO_WAITFORVSYNC, &crtc)) die("FBIO_WAITFORVSYNC real output counter");
        printf("CPU mmap readback and page %d park/vsync request passed\n", page % 2);
        sleep(1);
    }
    puts("PASS: CPU memory, fbdev park acknowledgement and internal VSYNC activity. Inspect monitor colors separately; this is not optical acceptance.");
    munmap(map, 2 * frame_bytes); close(fd);
    return 0;
}

// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fb.h>
#include <linux/magic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <unistd.h>

#define PAGE_BYTES (1280u * 720u * 4u)
static void die(const char *reason) { perror(reason); exit(1); }
static void require(int ok, const char *reason)
{ if (!ok) { fprintf(stderr, "front-sync-dump: %s\n", reason); exit(2); } }
static int mode_ok(const struct fb_fix_screeninfo *f, const struct fb_var_screeninfo *v)
{
    const char id[16] = "AX7Z020-720p";
    return !memcmp(f->id, id, 16) && f->type == FB_TYPE_PACKED_PIXELS && f->visual == FB_VISUAL_TRUECOLOR &&
        f->line_length == 5120 && f->smem_len == PAGE_BYTES * 2 && f->ypanstep == 720 &&
        v->xres == 1280 && v->yres == 720 && v->xres_virtual == 1280 && v->yres_virtual == 1440 &&
        v->bits_per_pixel == 32 && !v->xoffset && (v->yoffset == 0 || v->yoffset == 720) &&
        v->red.offset == 16 && v->green.offset == 8 && v->blue.offset == 0 &&
        v->red.length == 8 && v->green.length == 8 && v->blue.length == 8 &&
        !v->red.msb_right && !v->green.msb_right && !v->blue.msb_right && !v->transp.length &&
        !v->rotate && !v->grayscale && !v->nonstd && v->vmode == FB_VMODE_NONINTERLACED;
}
static void owner_stopped_with_fb(pid_t pid, dev_t framebuffer)
{
    char path[PATH_MAX], line[512]; int stopped = 0, found = 0;
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    FILE *file = fopen(path, "r"); if (!file) die("owner status");
    while (fgets(line, sizeof(line), file))
        if (!strncmp(line, "State:", 6)) { char state = 0; stopped = sscanf(line + 6, " %c", &state) == 1 && (state == 'T' || state == 't'); }
    fclose(file); require(stopped, "owner must already be stopped by the root controller");
    snprintf(path, sizeof(path), "/proc/%ld/fd", (long)pid);
    DIR *directory = opendir(path); if (!directory) die("owner fd directory");
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] == '.') continue;
        char fdpath[PATH_MAX]; struct stat st;
        int n = snprintf(fdpath, sizeof(fdpath), "%s/%s", path, entry->d_name);
        if (n > 0 && (size_t)n < sizeof(fdpath) && !stat(fdpath, &st) && S_ISCHR(st.st_mode) && st.st_rdev == framebuffer) found = 1;
    }
    closedir(directory); require(found, "stopped owner does not hold this framebuffer");
}
static void identity(dev_t device)
{
    char path[PATH_MAX], link[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/driver", major(device), minor(device));
    ssize_t n = readlink(path, link, sizeof(link) - 1); if (n < 0) die("framebuffer driver identity");
    link[n] = 0; const char *name = strrchr(link, '/');
    require(name && !strcmp(name + 1, "codex-ax7z020-fb"), "wrong framebuffer driver");
}
static void safe_output(const char *path, char *part)
{
    size_t n = strlen(path); require(n > 5 && n + 6 < PATH_MAX && !strncmp(path, "/tmp/", 5), "output must be an absolute new /tmp path");
    for (size_t i = 0; i < n; ++i)
        require((path[i] >= 'a' && path[i] <= 'z') || (path[i] >= 'A' && path[i] <= 'Z') ||
            (path[i] >= '0' && path[i] <= '9') || path[i] == '/' || path[i] == '_' || path[i] == '-' || path[i] == '.', "output path must be simple ASCII");
    require(!strstr(path, "/../") && !strstr(path, "/./") && !strstr(path, "//"), "noncanonical output path");
    char parent[PATH_MAX], resolved[PATH_MAX]; strcpy(parent, path); char *slash = strrchr(parent, '/'); require(slash && slash[1], "output filename missing"); *slash = 0;
    require(realpath(parent, resolved) && !strcmp(parent, resolved), "output parent must be an existing canonical directory");
    struct stat st; struct statfs fs;
    require(!lstat(parent, &st) && S_ISDIR(st.st_mode) && st.st_uid == 0 && (st.st_mode & 0777) == 0700, "output parent must be root-owned mode0700");
    require(!statfs(parent, &fs) && (unsigned long)fs.f_type == TMPFS_MAGIC, "output parent must be RAM tmpfs");
    require(lstat(path, &st) && errno == ENOENT, "output already exists or cannot be inspected");
    snprintf(part, PATH_MAX, "%s.part", path);
}
static void confirm(int fd, struct fb_var_screeninfo *value, uint32_t expected)
{
    struct fb_var_screeninfo request = *value;
    require(request.yoffset == expected && request.xoffset == 0, "front yoffset changed");
    request.activate = FB_ACTIVATE_VBL;
    if (ioctl(fd, FBIOPAN_DISPLAY, &request)) die("same-page PAN CURRENT acknowledgement");
    if (ioctl(fd, FBIOGET_VSCREENINFO, value)) die("FBIOGET after CURRENT acknowledgement");
    require(value->yoffset == expected && value->xoffset == 0, "front changed after synchronization");
}
int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "Usage: front-sync-dump STOPPED_OWNER_PID /tmp/private0700/NEW.bgrx\nThis synchronizes the same park page through the driver before/after a read; it is not completely read-only.\n"); return 2; }
    char *end = NULL; errno = 0; long raw_pid = strtol(argv[1], &end, 10);
    require(!errno && end && !*end && raw_pid > 1 && raw_pid <= INT_MAX && raw_pid != getpid(), "invalid stopped owner PID");
    char part[PATH_MAX]; safe_output(argv[2], part);
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC | O_NOFOLLOW); if (fd < 0) die("open fb0");
    struct stat dev; if (fstat(fd, &dev)) die("fstat fb0"); require(S_ISCHR(dev.st_mode), "fb0 is not a character device"); identity(dev.st_rdev);
    owner_stopped_with_fb((pid_t)raw_pid, dev.st_rdev);
    struct fb_fix_screeninfo fixed; struct fb_var_screeninfo before, after;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fixed) || ioctl(fd, FBIOGET_VSCREENINFO, &before)) die("framebuffer mode query");
    require(mode_ok(&fixed, &before), "unexpected fixed framebuffer layout"); uint32_t offset = before.yoffset;
    confirm(fd, &before, offset); require(mode_ok(&fixed, &before), "layout changed during initial synchronization");
    const unsigned char *map = mmap(NULL, PAGE_BYTES * 2, PROT_READ, MAP_SHARED, fd, 0); if (map == MAP_FAILED) die("mmap read-only framebuffer pages");
    unsigned char *snapshot = malloc(PAGE_BYTES), *repeat = malloc(PAGE_BYTES); require(snapshot && repeat, "snapshot allocation failed");
    memcpy(snapshot, map + (offset / 720) * PAGE_BYTES, PAGE_BYTES);
    memcpy(repeat, map + (offset / 720) * PAGE_BYTES, PAGE_BYTES);
    require(!memcmp(snapshot, repeat, PAGE_BYTES), "front pixels changed between double read");
    if (ioctl(fd, FBIOGET_VSCREENINFO, &after)) die("final framebuffer query");
    confirm(fd, &after, offset); require(mode_ok(&fixed, &after), "layout changed during final synchronization");
    owner_stopped_with_fb((pid_t)raw_pid, dev.st_rdev);
    int output = open(part, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600); if (output < 0) die("new partial output");
    size_t written = 0;
    while (written < PAGE_BYTES) { ssize_t n = write(output, snapshot + written, PAGE_BYTES - written); if (n < 0 && errno == EINTR) continue; if (n <= 0) die("snapshot write"); written += (size_t)n; }
    if (fsync(output) || close(output)) die("snapshot close");
    if (renameat2(AT_FDCWD, part, AT_FDCWD, argv[2], RENAME_NOREPLACE)) die("publish new synchronized snapshot");
    printf("{\"schema\":\"ax7z020.front-sync-readback.v1\",\"operation\":\"same_page_CURRENT_acknowledged_readback\",\"completely_read_only\":false,\"synchronizing_pan_ioctls\":2,\"owner_pid\":%ld,\"owner_stopped\":true,\"owner_fb_open_verified\":true,\"current_ack_before_after\":true,\"before_yoffset\":%u,\"after_yoffset\":%u,\"page\":%u,\"width\":1280,\"height\":720,\"stride\":5120,\"bytes\":%u,\"double_read_equal\":true,\"framebuffer_writes\":false,\"output\":\"%s\"}\n", raw_pid, offset, after.yoffset, offset / 720, PAGE_BYTES, argv[2]);
    munmap((void *)map, PAGE_BYTES * 2); close(fd); free(snapshot); free(repeat); return 0;
}

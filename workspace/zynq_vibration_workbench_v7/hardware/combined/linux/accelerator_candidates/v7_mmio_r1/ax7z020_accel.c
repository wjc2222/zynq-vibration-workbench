// SPDX-License-Identifier: GPL-2.0-only
/* V7 credit128/LO32 candidate; protected V6 driver remains a separate file.
 * This driver never maps or writes the
 * capture/display resources. Input/output are copied through bounded buffers.
 */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include "ax7z020_accel.h"
#include "ax7z020_accel_regs.h"
#include "v7_credit_policy.h"

static bool credit_feed = true;
static bool sample_lo32 = true;
static unsigned int feed_batch_frames = V7_ACCEL_MAX_BATCH;
module_param(credit_feed, bool, 0444);
MODULE_PARM_DESC(credit_feed, "Use conservative RAW_COUNT FIFO credits (analysis only)");
module_param(sample_lo32, bool, 0444);
MODULE_PARM_DESC(sample_lo32, "Read sign-extended Q7 samples through LO32 only");
module_param(feed_batch_frames, uint, 0444);
MODULE_PARM_DESC(feed_batch_frames, "Credit batch bound 1..128; default 128");

struct ax7acc {
    struct miscdevice misc;
    struct kref ref;
    struct mutex lock;
    void __iomem *regs;
    bool opened, removed, result_valid;
    size_t input_bytes;
    u8 *input;
    struct ax7z020_accel_result *output;
    struct ax7z020_accel_caps caps;
    unsigned long result_deadline;
    int result_error;
};

static void acc_release(struct kref *ref)
{
    struct ax7acc *a = container_of(ref, struct ax7acc, ref);
    vfree(a->input);
    vfree(a->output);
    kfree(a);
}

static int acc_check(struct ax7acc *a, unsigned long deadline)
{
    if (signal_pending(current)) return -ERESTARTSYS;
    if (time_after_eq(jiffies, deadline)) return -ETIMEDOUT;
    if (readl(a->regs + ACC_STATUS) & ACC_STATUS_ERROR) return -EIO;
    return 0;
}

static bool acc_identity_matches(struct ax7acc *a)
{
    return readl(a->regs + ACC_ID) == AX7Z020_ACCEL_HW_ID &&
           readl(a->regs + ACC_REVISION) == ACC_EXPECTED_REVISION;
}

static int acc_wait(struct ax7acc *a, u32 mask, unsigned long deadline)
{
    unsigned spin = 0;
    int ret;
    while (!(readl(a->regs + ACC_STATUS) & mask)) {
        ret = acc_check(a, deadline);
        if (ret) return ret;
        if ((++spin & 255) == 0) {
            usleep_range(10, 30);
            cond_resched();
        } else cpu_relax();
    }
    return acc_check(a, deadline);
}

static int acc_validate_plan(const struct ax7z020_accel_plan *p,
                             u32 *decimation, u32 *taps)
{
    unsigned i, j;
    if (p->abi_version != AX7Z020_ACCEL_ABI ||
        p->flags & ~AX7Z020_ACCEL_PLAN_BANDS_VALID ||
        p->timeout_ms < 10 || p->timeout_ms > 30000 ||
        p->reserved[0] || p->reserved[1] || p->reserved[2]) return -EINVAL;
    switch (p->sample_rate_hz) {
    case 50000: *decimation = 10; *taps = ACC_TAPS_50000; break;
    case 25000: *decimation = 5; *taps = ACC_TAPS_25000; break;
    case 10000: *decimation = 2; *taps = ACC_TAPS_10000; break;
    case 5000: *decimation = 1; *taps = ACC_TAPS_5000; break;
    default: return -EINVAL;
    }
    if (p->raw_frames != *taps + (AX7Z020_ACCEL_POINTS - 1) * *decimation ||
        p->raw_frames > AX7Z020_ACCEL_MAX_RAW_FRAMES) return -EINVAL;
    if (!(p->flags & AX7Z020_ACCEL_PLAN_BANDS_VALID)) return 0;
    for (i = 0; i < AX7Z020_ACCEL_ORDERS; ++i) {
        if (p->bands[i].first < 4 || p->bands[i].last > 819 ||
            p->bands[i].first > p->bands[i].last) return -EINVAL;
        for (j = 0; j < i; ++j)
            if (p->bands[i].first <= p->bands[j].last &&
                p->bands[j].first <= p->bands[i].last) return -EINVAL;
    }
    return 0;
}

/* Implemented against the common V6 register contract. */
static bool acc_select_result(struct ax7acc *a, u32 index)
{
    unsigned spin = 0;
    if (a->result_error) return false;
    writel(index, a->regs + ACC_RESULT_INDEX);
    /* Selected-index division and the synchronous BRAM have separate stages.
     * The completed tag is the wire-protocol acknowledgement that this exact
     * selected payload is stable, independent of PS/interconnect turnaround. */
    while (readl(a->regs + ACC_RESULT_READY_INDEX) != index) {
        a->result_error = acc_check(a, a->result_deadline);
        if (a->result_error) return false;
        if ((++spin & 255) == 0) cond_resched();
        cpu_relax();
    }
    return true;
}

static u64 acc_result(struct ax7acc *a, u32 index)
{
    u32 lo, hi;
    if (!acc_select_result(a, index)) return 0;
    lo = readl(a->regs + ACC_RESULT_LO);
    hi = readl(a->regs + ACC_RESULT_HI);
    return ((u64)hi << 32) | lo;
}

static s32 acc_sample32(struct ax7acc *a, u32 index)
{
    s32 value;
    if (!acc_select_result(a, index)) return 0;
    /* RTL sign-extends signed24 through bit31. Never mask off a malformed
     * high byte: range-check it, so a noncanonical LO32 becomes EPROTO. */
    if (v7_accel_sample32(readl(a->regs + ACC_RESULT_LO), &value)) {
        a->result_error = -EPROTO;
        return 0;
    }
    return value;
}

static int acc_wait_credit(struct ax7acc *a, u32 issued, u32 remaining,
                           u32 *previous_consumed, u32 *credit,
                           unsigned long deadline)
{
    u32 wanted = min_t(u32, remaining, feed_batch_frames);
    for (;;) {
        u32 consumed, available;
        int ret = acc_check(a, deadline);
        if (ret) return ret;
        /* Keep ordered readl/writel; no relaxed MMIO and no speculative
         * extra commits. Pending includes any PS/interconnect queued writes. */
        consumed = readl(a->regs + ACC_RAW_COUNT);
        if (v7_accel_credit(issued, consumed, *previous_consumed,
                            remaining, feed_batch_frames, &available))
            return -EPROTO;
        *previous_consumed = consumed;
        if (available >= wanted) {
            *credit = wanted;
            return 0;
        }
        /* FIFO still retains >=511-wanted frames when waiting for this
         * watermark. Yield CPU rather than polling every raw frame. */
        usleep_range(10, 30);
        cond_resched();
    }
}

static int acc_transaction(struct ax7acc *a, const struct ax7z020_accel_plan *p,
                           u32 decimation, u32 taps)
{
    unsigned long deadline = jiffies + msecs_to_jiffies(p->timeout_ms);
    u32 profile = p->sample_rate_hz == 50000 ? 0 :
                  p->sample_rate_hz == 25000 ? 1 :
                  p->sample_rate_hz == 10000 ? 2 : 3;
    unsigned n, ch, b;
    u32 credit = 0, previous_consumed = 0;
    int ret;
    struct ax7z020_accel_result *r = a->output;
    memset(r, 0, sizeof(*r));
    a->result_deadline = deadline;
    a->result_error = 0;
    if (!acc_identity_matches(a)) return -ENODEV;
    writel(ACC_CONTROL_RESET, a->regs + ACC_CONTROL);
    ret = acc_wait(a, ACC_STATUS_IDLE, deadline);
    if (ret) return ret;
    if (!acc_identity_matches(a)) return -ENODEV;
    writel(profile, a->regs + ACC_PROFILE);
    for (b = 0; b < AX7Z020_ACCEL_ORDERS; ++b)
        writel((p->flags & AX7Z020_ACCEL_PLAN_BANDS_VALID) ?
               p->bands[b].first | (p->bands[b].last << 16) : 0x00001fffu,
               a->regs + ACC_BANDS + 4 * b);
    writel(ACC_CONTROL_START, a->regs + ACC_CONTROL);
    if (readl(a->regs + ACC_RAW_REQUIRED) != p->raw_frames) return -EPROTO;
    if (credit_feed && readl(a->regs + ACC_RAW_COUNT) != 0) return -EPROTO;
    for (n = 0; n < p->raw_frames; ++n) {
        const u8 *frame = a->input + n * 6;
        u32 ch12 = frame[0] | (frame[1] << 8) | (frame[2] << 16) | ((u32)frame[3] << 24);
        u32 ch3 = frame[4] | (frame[5] << 8);
        if (credit_feed) {
            if (!credit) {
                ret = acc_wait_credit(a, n, p->raw_frames - n,
                                      &previous_consumed, &credit, deadline);
                if (ret) return ret;
            }
        } else {
            ret = acc_wait(a, ACC_STATUS_INPUT_READY, deadline);
            if (ret) return ret;
        }
        writel(ch12, a->regs + ACC_INPUT_CH12);
        writel(ch3, a->regs + ACC_INPUT_CH3_COMMIT);
        if (credit_feed) {
            --credit;
            if (!credit) cond_resched();
        } else if (!(n & 255)) cond_resched();
    }
    ret = acc_wait(a, ACC_STATUS_DONE, deadline);
    if (ret) return ret;
    if (readl(a->regs + ACC_RAW_COUNT) != p->raw_frames ||
        readl(a->regs + ACC_LOW_COUNT) != AX7Z020_ACCEL_POINTS) return -EPROTO;
    r->abi_version = AX7Z020_ACCEL_ABI;
    r->hardware_id = a->caps.hardware_id;
    r->revision = a->caps.revision;
    r->sample_rate_hz = p->sample_rate_hz;
    r->points = AX7Z020_ACCEL_POINTS;
    r->fir_taps = taps;
    r->decimation = decimation;
    r->flags = p->flags;
    r->error_code = readl(a->regs + ACC_ERRORS);
    r->cycles = readl(a->regs + ACC_CYCLES_LO);
    r->cycles |= (u64)readl(a->regs + ACC_CYCLES_HI) << 32;
    if (r->error_code) return -EIO;
    for (ch = 0; ch < 3; ++ch) {
        struct ax7z020_accel_channel *c = &r->channel[ch];
        u32 index = 0x10000 + ch * 0x100;
        c->minimum_q7 = (s32)acc_result(a, index);
        c->maximum_q7 = (s32)acc_result(a, index + 1);
        c->sum_q7 = (s64)acc_result(a, index + 2);
        c->sumsq_q14 = acc_result(a, index + 3);
        c->raw_endpoint_count = acc_result(a, index + 4);
        c->fft_exponent = acc_result(a, index + 5);
        c->fft_pre_shift = acc_result(a, index + 6);
        c->saturation_count = acc_result(a, index + 7);
        if (a->result_error) return a->result_error;
        r->saturation_count += min_t(u64, c->saturation_count, U32_MAX);
        for (n = 0; n < AX7Z020_ACCEL_POINTS; ++n) {
            c->lowband_q7[n] = sample_lo32 ? acc_sample32(a, n * 3 + ch) :
                                           (s32)acc_result(a, n * 3 + ch);
            if (!(n & 255)) {
                if (a->result_error) return a->result_error;
                ret = acc_check(a, deadline);
                if (ret) return ret;
                cond_resched();
            }
        }
        for (n = 0; n < AX7Z020_ACCEL_BINS; ++n)
            c->fft_magnitude_squared[n] = acc_result(a, 0x20000 + ch * 8192 + n);
        if (a->result_error) return a->result_error;
        for (b = 0; b < 18; ++b)
            c->band_magnitude_squared[b] = acc_result(a, 0x30000 + ch * 32 + b);
        if (a->result_error) return a->result_error;
    }
    writel(ACC_CONTROL_ACK, a->regs + ACC_CONTROL);
    return acc_check(a, deadline);
}

static int acc_open(struct inode *inode, struct file *f)
{
    struct ax7acc *a = container_of(f->private_data, struct ax7acc, misc);
    int ret = 0;
    mutex_lock(&a->lock);
    if (a->removed) ret = -ENODEV;
    else if (a->opened) ret = -EBUSY;
    else {
        a->opened = true;
        a->input_bytes = 0;
        a->result_valid = false;
        kref_get(&a->ref);
        f->private_data = a;
    }
    mutex_unlock(&a->lock);
    return ret;
}

static int acc_close(struct inode *inode, struct file *f)
{
    struct ax7acc *a = f->private_data;
    mutex_lock(&a->lock);
    if (!a->removed && acc_identity_matches(a)) writel(ACC_CONTROL_RESET, a->regs + ACC_CONTROL);
    a->opened = false;
    a->input_bytes = 0;
    a->result_valid = false;
    mutex_unlock(&a->lock);
    kref_put(&a->ref, acc_release);
    return 0;
}

static ssize_t acc_write(struct file *f, const char __user *buf, size_t n, loff_t *pos)
{
    struct ax7acc *a = f->private_data;
    ssize_t ret;
    if (!n) return 0;
    mutex_lock(&a->lock);
    if (a->removed) ret = -ENODEV;
    else if (n % 6 || n > AX7Z020_ACCEL_MAX_RAW_FRAMES * 6u - a->input_bytes)
        ret = -EINVAL;
    else if (copy_from_user(a->input + a->input_bytes, buf, n)) ret = -EFAULT;
    else {
        a->input_bytes += n;
        a->result_valid = false;
        f->f_pos = 0;
        ret = n;
    }
    mutex_unlock(&a->lock);
    return ret;
}

static ssize_t acc_read(struct file *f, char __user *buf, size_t n, loff_t *pos)
{
    struct ax7acc *a = f->private_data;
    ssize_t ret;
    mutex_lock(&a->lock);
    if (a->removed) ret = -ENODEV;
    else if (!a->result_valid) ret = -ENODATA;
    else ret = simple_read_from_buffer(buf, n, pos, a->output, sizeof(*a->output));
    mutex_unlock(&a->lock);
    return ret;
}

static long acc_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct ax7acc *a = f->private_data;
    struct ax7z020_accel_plan plan;
    struct ax7z020_accel_status status;
    u32 decimation, taps;
    long ret;
    if (mutex_lock_interruptible(&a->lock)) return -ERESTARTSYS;
    if (a->removed) { ret = -ENODEV; goto out; }
    switch (cmd) {
    case AX7Z020_ACCEL_GET_STATUS:
        memset(&status, 0, sizeof(status));
        status.abi_version = AX7Z020_ACCEL_ABI;
        status.hardware_id = readl(a->regs + ACC_ID);
        status.revision = readl(a->regs + ACC_REVISION);
        status.status = readl(a->regs + ACC_STATUS);
        status.errors = readl(a->regs + ACC_ERRORS);
        status.raw_frames = readl(a->regs + ACC_RAW_COUNT);
        status.lowband_frames = readl(a->regs + ACC_LOW_COUNT);
        status.raw_required = readl(a->regs + ACC_RAW_REQUIRED);
        ret = copy_to_user((void __user *)arg, &status, sizeof(status)) ? -EFAULT : 0;
        break;
    case AX7Z020_ACCEL_GET_CAPS:
        ret = !acc_identity_matches(a) ? -ENODEV :
              copy_to_user((void __user *)arg, &a->caps, sizeof(a->caps)) ? -EFAULT : 0;
        break;
    case AX7Z020_ACCEL_RESET:
        if (!acc_identity_matches(a)) { ret = -ENODEV; break; }
        writel(ACC_CONTROL_RESET, a->regs + ACC_CONTROL);
        a->input_bytes = 0;
        a->result_valid = false;
        f->f_pos = 0;
        ret = 0;
        break;
    case AX7Z020_ACCEL_RUN:
        a->result_valid = false;
        f->f_pos = 0;
        if (copy_from_user(&plan, (void __user *)arg, sizeof(plan))) { ret = -EFAULT; break; }
        ret = acc_validate_plan(&plan, &decimation, &taps);
        if (ret) break;
        if (a->input_bytes != plan.raw_frames * 6u) { ret = -EMSGSIZE; break; }
        ret = acc_transaction(a, &plan, decimation, taps);
        a->input_bytes = 0;
        if (ret) {
            dev_warn_ratelimited(a->misc.parent,
                "analysis transaction failed %ld: status=%08x errors=%08x raw=%u low=%u\n",
                ret, readl(a->regs + ACC_STATUS), readl(a->regs + ACC_ERRORS),
                readl(a->regs + ACC_RAW_COUNT), readl(a->regs + ACC_LOW_COUNT));
            if (acc_identity_matches(a)) writel(ACC_CONTROL_RESET, a->regs + ACC_CONTROL);
        }
        else a->result_valid = true;
        break;
    default: ret = -ENOTTY;
    }
out:
    mutex_unlock(&a->lock);
    return ret;
}

static const struct file_operations acc_fops = {
    .owner = THIS_MODULE, .open = acc_open, .release = acc_close,
    .read = acc_read, .write = acc_write, .unlocked_ioctl = acc_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = acc_ioctl,
#endif
    .llseek = no_llseek,
};

static int acc_probe(struct platform_device *pdev)
{
    struct ax7acc *a;
    struct resource *resource;
    int ret;
    if (!feed_batch_frames || feed_batch_frames > V7_ACCEL_MAX_BATCH) return -EINVAL;
    resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!resource || resource_size(resource) != 0x10000) return -EINVAL;
    a = kzalloc(sizeof(*a), GFP_KERNEL);
    if (!a) return -ENOMEM;
    kref_init(&a->ref);
    mutex_init(&a->lock);
    a->regs = devm_ioremap_resource(&pdev->dev, resource);
    if (IS_ERR(a->regs)) { ret = PTR_ERR(a->regs); goto fail; }
    a->caps.hardware_id = readl(a->regs + ACC_ID);
    a->caps.revision = readl(a->regs + ACC_REVISION);
    if (a->caps.hardware_id != AX7Z020_ACCEL_HW_ID ||
        a->caps.revision != ACC_EXPECTED_REVISION) { ret = -ENODEV; goto fail; }
    a->input = vzalloc(AX7Z020_ACCEL_MAX_RAW_FRAMES * 6u);
    a->output = vzalloc(sizeof(*a->output));
    if (!a->input || !a->output) { ret = -ENOMEM; goto fail; }
    a->caps.abi_version = AX7Z020_ACCEL_ABI;
    a->caps.clock_hz = 100000000;
    of_property_read_u32(pdev->dev.of_node, "clock-frequency", &a->caps.clock_hz);
    a->caps.points = AX7Z020_ACCEL_POINTS;
    a->caps.bins = AX7Z020_ACCEL_BINS;
    a->caps.rate_mask = 15;
    a->caps.max_raw_frames = AX7Z020_ACCEL_MAX_RAW_FRAMES;
    a->misc.minor = MISC_DYNAMIC_MINOR;
    a->misc.name = "vibration-accel";
    a->misc.fops = &acc_fops;
    a->misc.parent = &pdev->dev;
    a->misc.mode = 0600;
    if (acc_identity_matches(a)) writel(ACC_CONTROL_RESET, a->regs + ACC_CONTROL);
    platform_set_drvdata(pdev, a);
    ret = misc_register(&a->misc);
    if (!ret) return 0;
fail:
    kref_put(&a->ref, acc_release);
    return ret;
}

static int acc_remove(struct platform_device *pdev)
{
    struct ax7acc *a = platform_get_drvdata(pdev);
    misc_deregister(&a->misc);
    mutex_lock(&a->lock);
    a->removed = true;
    if (acc_identity_matches(a)) writel(ACC_CONTROL_RESET, a->regs + ACC_CONTROL);
    mutex_unlock(&a->lock);
    kref_put(&a->ref, acc_release);
    return 0;
}
static const struct of_device_id acc_of_match[] = {
    { .compatible = "codex,ax7z020-analysis-v6" }, {}
};
MODULE_DEVICE_TABLE(of, acc_of_match);
static struct platform_driver acc_driver = {
    .probe = acc_probe, .remove = acc_remove,
    .driver = { .name = "ax7z020-analysis-v6", .of_match_table = acc_of_match },
};
module_platform_driver(acc_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AX7Z020 V7 independent credit128/LO32 analysis candidate");
MODULE_VERSION("v7-credit128-lo32-r1");

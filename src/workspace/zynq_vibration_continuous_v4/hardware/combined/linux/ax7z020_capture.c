// SPDX-License-Identifier: GPL-2.0-only
/* Finite, exclusive AX7Z020 CAP2 + AXI DMA simple S2MM capture.
 * This driver owns both MMIO windows and their single S2MM interrupt.
 * Userspace receives copies; physical addresses and mmap are never exposed.
 */
#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include "ax7z020_capture.h"

#define CAP_ID 0x00
#define CAP_COMMAND 0x04
#define CAP_STATUS 0x08
#define CAP_TARGET 0x0c
#define CAP_RATE 0x10
#define CAP_CAPTURED 0x14
#define CAP_SENT 0x18
#define CAP_DROPPED 0x1c
#define CAP_CLOCK 0x2c
#define CAP_ADC_FRAMES 0x38
#define CAP_RECORD_BYTES 0x3c
#define CAP_FILTER 0x40
#define CAP_TIMEOUT 0x44
#define CAP_REVISION 0x50
#define CAP_RATE_SELECT 0x54
#define CAP_SOURCE 0x58
#define CAP_REJECT 0x5c
#define CAP_RANGE 0x60
#define CAP_CHANNEL_MAP 0x64
#define CAP_FEATURES 0x68
#define CAP_START BIT(0)
#define CAP_ADC_RESET BIT(1)
#define CAP_ABORT BIT(2)
#define CAP_CLEAR_REJECT BIT(4)
#define CAP_ACTIVE BIT(0)
#define CAP_COMPLETE (BIT(1) | BIT(2))
#define CAP_IS_TEST BIT(6)
#define CAP_ERRORS GENMASK(5, 3)
#define CAP_ADC_ERRORS GENMASK(31, 29)
#define DMA_CONTROL 0x30
#define DMA_STATUS 0x34
#define DMA_DESTINATION 0x48
#define DMA_DESTINATION_HI 0x4c
#define DMA_LENGTH 0x58
#define DMA_RUN BIT(0)
#define DMA_RESET BIT(2)
#define DMA_HALTED BIT(0)
#define DMA_IDLE BIT(1)
#define DMA_ERRORS 0x00000770u
#define DMA_IOC BIT(12)
#define DMA_ERR_IRQ BIT(14)
#define DMA_IRQ_ALL 0x00007000u
#define DMA_IRQ_ENABLE (DMA_IOC | DMA_ERR_IRQ)
#define MONITOR_MS 20

struct ax7cap {
	struct device *dev;
	struct miscdevice misc;
	struct kref ref;
	struct mutex lock;
	wait_queue_head_t wait;
	struct delayed_work monitor;
	void __iomem *cap, *dma;
	struct clk *clock;
	int irq;
	bool removed, opened, poisoned;
	void *buffer;
	dma_addr_t buffer_dma;
	size_t buffer_bytes;
	unsigned long deadline;
	u32 expected_source, expected_rate;
	atomic_t irq_events;
	struct ax7z020_capture_caps caps;
	struct ax7z020_capture_status status;
};

static bool ax7cap_active(struct ax7cap *c)
{
	return c->status.state == AX7Z020_CAPTURE_PREPARING ||
	       c->status.state == AX7Z020_CAPTURE_RUNNING;
}

/* IRQ handler never takes lock, so synchronize_irq is safe while holding it.
 * Abort stops the source and drops its pending FIFO. Reset MUST have completed
 * before coherent memory can be released or reused. No display reset is used.
 */
static int ax7cap_quiesce(struct ax7cap *c)
{
	u32 value;
	int ret;
	writel(CAP_ABORT, c->cap + CAP_COMMAND);
	writel(0, c->dma + DMA_CONTROL);
	readl(c->dma + DMA_CONTROL);
	synchronize_irq(c->irq);
	writel(DMA_RESET, c->dma + DMA_CONTROL);
	ret = readl_poll_timeout(c->dma + DMA_CONTROL, value,
				 !(value & DMA_RESET), 10, 100000);
	if (!ret)
		ret = readl_poll_timeout(c->dma + DMA_STATUS, value,
					 value & DMA_HALTED, 10, 100000);
	if (!ret)
		ret = readl_poll_timeout(c->cap + CAP_STATUS, value,
					 !(value & CAP_ACTIVE), 10, 100000);
	writel(DMA_IRQ_ALL, c->dma + DMA_STATUS);
	atomic_set(&c->irq_events, 0);
	if (ret)
		c->poisoned = true;
	return ret;
}

static void ax7cap_snapshot(struct ax7cap *c)
{
	c->status.hw_status = readl(c->cap + CAP_STATUS);
	c->status.dma_status = readl(c->dma + DMA_STATUS);
	c->status.dma_length = readl(c->dma + DMA_LENGTH);
	c->status.captured_records = readl(c->cap + CAP_CAPTURED);
	c->status.sent_records = readl(c->cap + CAP_SENT);
	c->status.dropped_records = readl(c->cap + CAP_DROPPED);
	c->status.sample_rate_hz = readl(c->cap + CAP_RATE);
	c->status.flags = c->status.hw_status & CAP_IS_TEST ?
		AX7Z020_CAPTURE_START_TEST_PATTERN : 0;
}

static void ax7cap_finish(struct ax7cap *c, u32 state, int error)
{
	u32 events = c->status.dma_status & DMA_IRQ_ALL;
	int ret;
	/* Snapshot evidence before resetting the DMA status/count registers. */
	ax7cap_snapshot(c);
	c->status.dma_status |= events | atomic_xchg(&c->irq_events, 0);
	ret = ax7cap_quiesce(c);
	if (ret) {
		state = AX7Z020_CAPTURE_FAILED;
		error = EIO;
		dev_crit(c->dev, "DMA did not quiesce; buffer quarantined until reboot\n");
	}
	c->status.error_code = error;
	c->status.state = state;
	cancel_delayed_work(&c->monitor);
	wake_up_interruptible(&c->wait);
}

static void ax7cap_check(struct ax7cap *c)
{
	u32 events;
	if (c->status.state != AX7Z020_CAPTURE_RUNNING)
		return;
	ax7cap_snapshot(c);
	events = atomic_xchg(&c->irq_events, 0);
	/* IRQ acknowledges W1C bits; retain them in the terminal evidence. */
	c->status.dma_status |= events;
	if ((c->status.dma_status & DMA_ERRORS) ||
	    (c->status.hw_status & (CAP_ERRORS | CAP_ADC_ERRORS)) ||
	    readl(c->cap + CAP_REJECT) ||
	    c->status.sample_rate_hz != c->expected_rate ||
	    readl(c->cap + CAP_SOURCE) != c->expected_source ||
	    !!(c->status.hw_status & CAP_IS_TEST) != c->expected_source) {
		ax7cap_finish(c, AX7Z020_CAPTURE_FAILED, EIO);
	} else if (c->status.dma_status & DMA_IOC) {
		/* Idle, TLAST byte count and all producer counters must agree. */
		if (!(c->status.dma_status & DMA_IDLE) ||
		    c->status.dma_length != c->buffer_bytes ||
		    c->status.captured_records != c->status.requested_records ||
		    c->status.sent_records != c->status.requested_records ||
		    c->status.dropped_records ||
		    (c->status.hw_status & 7) != CAP_COMPLETE) {
			ax7cap_finish(c, AX7Z020_CAPTURE_FAILED, EPROTO);
		} else {
			/* dma_alloc_coherent handles cache ownership; order the reads. */
			dma_rmb();
			ax7cap_finish(c, AX7Z020_CAPTURE_READY, 0);
		}
	} else if (time_after_eq(jiffies, c->deadline)) {
		ax7cap_finish(c, AX7Z020_CAPTURE_FAILED, ETIMEDOUT);
	}
}

static void ax7cap_monitor(struct work_struct *work)
{
	struct ax7cap *c = container_of(to_delayed_work(work), struct ax7cap, monitor);
	mutex_lock(&c->lock);
	if (!c->removed) {
		ax7cap_check(c);
		if (c->status.state == AX7Z020_CAPTURE_RUNNING)
			mod_delayed_work(system_wq, &c->monitor, msecs_to_jiffies(MONITOR_MS));
	}
	mutex_unlock(&c->lock);
}

static irqreturn_t ax7cap_irq(int irq, void *opaque)
{
	struct ax7cap *c = opaque;
	u32 value;
	if (READ_ONCE(c->removed))
		return IRQ_NONE;
	value = readl(c->dma + DMA_STATUS);
	if (!(value & DMA_IRQ_ALL))
		return IRQ_NONE;
	atomic_or(value, &c->irq_events);
	writel(value & DMA_IRQ_ALL, c->dma + DMA_STATUS);
	mod_delayed_work(system_wq, &c->monitor, 0);
	return IRQ_HANDLED;
}

static void ax7cap_free_buffer(struct ax7cap *c)
{
	if (c->buffer && !c->poisoned) {
		dma_free_coherent(c->dev, c->buffer_bytes, c->buffer, c->buffer_dma);
		c->buffer = NULL;
		c->buffer_bytes = 0;
	}
}

static void ax7cap_release_ref(struct kref *ref)
{
	struct ax7cap *c = container_of(ref, struct ax7cap, ref);
	/* In the exceptional reset-failure case neither allocator memory nor its
	 * device reference is released: hardware could still write that address.
	 */
	if (!(c->poisoned && c->buffer))
		put_device(c->dev);
	kfree(c);
}

static int ax7cap_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct ax7cap *c = container_of(misc, struct ax7cap, misc);
	int ret = 0;
	mutex_lock(&c->lock);
	if (c->removed)
		ret = -ENODEV;
	else if (c->opened)
		ret = -EBUSY;
	else {
		kref_get(&c->ref);
		c->opened = true;
		file->private_data = c;
	}
	mutex_unlock(&c->lock);
	return ret;
}

static int ax7cap_release(struct inode *inode, struct file *file)
{
	struct ax7cap *c = file->private_data;
	mutex_lock(&c->lock);
	if (!c->removed && ax7cap_active(c))
		ax7cap_finish(c, AX7Z020_CAPTURE_CANCELLED, ECANCELED);
	ax7cap_free_buffer(c);
	if (!c->removed && !c->poisoned) {
		memset(&c->status, 0, sizeof(c->status));
		c->status.abi_version = AX7Z020_CAPTURE_ABI;
		c->status.state = AX7Z020_CAPTURE_IDLE;
	}
	c->opened = false;
	mutex_unlock(&c->lock);
	kref_put(&c->ref, ax7cap_release_ref);
	return 0;
}

static int ax7cap_selector(u32 rate)
{
	switch (rate) {
	case 50000: return 0;
	case 25000: return 1;
	case 10000: return 2;
	case 5000: return 3;
	default: return -EINVAL;
	}
}

static int ax7cap_start(struct ax7cap *c, struct file *file,
			const struct ax7z020_capture_plan *p)
{
	u32 value, source = !!(p->flags & AX7Z020_CAPTURE_START_TEST_PATTERN);
	int selector = ax7cap_selector(p->sample_rate_hz);
	int ret;
	if (p->abi_version != AX7Z020_CAPTURE_ABI || selector < 0 ||
	    !p->requested_records || p->requested_records > c->caps.max_records ||
	    (p->flags & ~AX7Z020_CAPTURE_START_TEST_PATTERN) ||
	    memchr_inv(p->reserved, 0, sizeof(p->reserved)))
		return -EINVAL;
	if (ax7cap_active(c))
		return -EBUSY;
	if (c->poisoned)
		return -EIO;
	/* A new START explicitly invalidates the previous result. */
	ret = ax7cap_quiesce(c);
	if (ret) {
		c->status.state = AX7Z020_CAPTURE_FAILED;
		c->status.error_code = EIO;
		wake_up_interruptible(&c->wait);
		return ret;
	}
	ax7cap_free_buffer(c);
	memset(&c->status, 0, sizeof(c->status));
	c->status.abi_version = AX7Z020_CAPTURE_ABI;
	c->status.state = AX7Z020_CAPTURE_PREPARING;
	c->status.requested_records = p->requested_records;
	c->expected_source = source;
	c->expected_rate = p->sample_rate_hz;
	c->buffer_bytes = (size_t)p->requested_records * c->caps.record_bytes;
	c->buffer = dma_alloc_coherent(c->dev, c->buffer_bytes,
				       &c->buffer_dma, GFP_KERNEL);
	if (!c->buffer) {
		ret = -ENOMEM;
		goto failed;
	}
	if (upper_32_bits(c->buffer_dma)) {
		ret = -ERANGE;
		goto failed;
	}
	writel(CAP_CLEAR_REJECT, c->cap + CAP_COMMAND);
	writel(selector, c->cap + CAP_RATE_SELECT);
	writel(source, c->cap + CAP_SOURCE);
	writel(p->requested_records, c->cap + CAP_TARGET);
	if (readl(c->cap + CAP_RATE) != p->sample_rate_hz ||
	    readl(c->cap + CAP_RATE_SELECT) != selector ||
	    readl(c->cap + CAP_SOURCE) != source ||
	    readl(c->cap + CAP_TARGET) != p->requested_records ||
	    readl(c->cap + CAP_REJECT)) {
		ret = -EPROTO;
		goto failed;
	}
	writel(CAP_ADC_RESET, c->cap + CAP_COMMAND);
	if (!source) {
		/* Reset deasserts ready first; do not accept a stale pre-reset count. */
		ret = readl_poll_timeout(c->cap + CAP_ADC_FRAMES, value,
					 value < 4, 10, 100000);
		if (!ret)
			ret = readl_poll_timeout(c->cap + CAP_ADC_FRAMES, value,
						 value >= 4, 50, 2000000);
		if (ret || (readl(c->cap + CAP_STATUS) & CAP_ADC_ERRORS)) {
			ret = ret ?: -EIO;
			goto failed;
		}
	}
	if (readl(c->cap + CAP_REJECT)) {
		ret = -EPROTO;
		goto failed;
	}
	writel(DMA_IRQ_ALL, c->dma + DMA_STATUS);
	atomic_set(&c->irq_events, 0);
	writel(DMA_RUN | DMA_IRQ_ENABLE, c->dma + DMA_CONTROL);
	ret = readl_poll_timeout(c->dma + DMA_STATUS, value,
				 !(value & DMA_HALTED), 10, 100000);
	if (ret)
		goto failed;
	writel(lower_32_bits(c->buffer_dma), c->dma + DMA_DESTINATION);
	writel(0, c->dma + DMA_DESTINATION_HI);
	dma_wmb();
	writel(c->buffer_bytes, c->dma + DMA_LENGTH); /* LAST: arm receiver. */
	readl(c->dma + DMA_CONTROL);
	c->status.state = AX7Z020_CAPTURE_RUNNING;
	c->status.sample_rate_hz = p->sample_rate_hz;
	c->status.flags = source ? AX7Z020_CAPTURE_START_TEST_PATTERN : 0;
	c->deadline = jiffies + msecs_to_jiffies(
		DIV_ROUND_UP_ULL((u64)p->requested_records * 1000, p->sample_rate_hz) + 5000);
	file->f_pos = 0;
	writel(CAP_START, c->cap + CAP_COMMAND); /* LAST: start producer. */
	mod_delayed_work(system_wq, &c->monitor, msecs_to_jiffies(MONITOR_MS));
	return 0;
failed:
	ax7cap_finish(c, AX7Z020_CAPTURE_FAILED, -ret);
	ax7cap_free_buffer(c);
	return ret;
}

static long ax7cap_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ax7cap *c = file->private_data;
	struct ax7z020_capture_plan plan;
	void __user *user = (void __user *)arg;
	int ret = 0;
	if (cmd == AX7Z020_CAPTURE_START && copy_from_user(&plan, user, sizeof(plan)))
		return -EFAULT;
	if (mutex_lock_interruptible(&c->lock))
		return -ERESTARTSYS;
	if (c->removed) {
		ret = -ENODEV;
		goto out;
	}
	switch (cmd) {
	case AX7Z020_CAPTURE_GET_CAPS:
		if (copy_to_user(user, &c->caps, sizeof(c->caps))) ret = -EFAULT;
		break;
	case AX7Z020_CAPTURE_GET_STATUS:
		ax7cap_check(c);
		if (c->status.state == AX7Z020_CAPTURE_IDLE)
			ax7cap_snapshot(c);
		if (copy_to_user(user, &c->status, sizeof(c->status))) ret = -EFAULT;
		break;
	case AX7Z020_CAPTURE_START:
		ret = ax7cap_start(c, file, &plan);
		break;
	case AX7Z020_CAPTURE_CANCEL:
		if (ax7cap_active(c))
			ax7cap_finish(c, AX7Z020_CAPTURE_CANCELLED, ECANCELED);
		if (c->poisoned) ret = -EIO;
		break;
	default:
		ret = -ENOTTY;
	}
out:
	mutex_unlock(&c->lock);
	return ret;
}

static ssize_t ax7cap_read(struct file *file, char __user *out, size_t count, loff_t *pos)
{
	struct ax7cap *c = file->private_data;
	ssize_t ret;
	if (!count)
		return 0;
retry:
	if (mutex_lock_interruptible(&c->lock)) return -ERESTARTSYS;
	if (c->removed) { ret = -ENODEV; goto done; }
	if (ax7cap_active(c)) {
		mutex_unlock(&c->lock);
		if (file->f_flags & O_NONBLOCK) return -EAGAIN;
		ret = wait_event_interruptible(c->wait,
			READ_ONCE(c->removed) ||
			(READ_ONCE(c->status.state) != AX7Z020_CAPTURE_RUNNING &&
			 READ_ONCE(c->status.state) != AX7Z020_CAPTURE_PREPARING));
		if (ret) return ret;
		goto retry;
	}
	if (c->status.state != AX7Z020_CAPTURE_READY) {
		ret = c->status.error_code ? -(int)c->status.error_code : -ENODATA;
		goto done;
	}
	if (*pos < 0 || *pos > c->buffer_bytes) { ret = -EINVAL; goto done; }
	count = min_t(size_t, count, c->buffer_bytes - *pos);
	if (copy_to_user(out, (char *)c->buffer + *pos, count)) { ret = -EFAULT; goto done; }
	*pos += count;
	ret = count;
done:
	mutex_unlock(&c->lock);
	return ret;
}

static __poll_t ax7cap_poll(struct file *file, poll_table *wait)
{
	struct ax7cap *c = file->private_data;
	__poll_t result = 0;
	poll_wait(file, &c->wait, wait);
	mutex_lock(&c->lock);
	if (c->removed) result = EPOLLERR | EPOLLHUP;
	else if (c->status.state == AX7Z020_CAPTURE_READY) result = EPOLLIN | EPOLLRDNORM;
	else if (c->status.error_code) result = EPOLLERR;
	mutex_unlock(&c->lock);
	return result;
}

static const struct file_operations ax7cap_fops = {
	.owner = THIS_MODULE, .open = ax7cap_open, .release = ax7cap_release,
	.unlocked_ioctl = ax7cap_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.read = ax7cap_read, .poll = ax7cap_poll, .llseek = no_llseek,
};

static int ax7cap_probe(struct platform_device *pdev)
{
	struct ax7cap *c;
	u32 features;
	int ret;
	BUILD_BUG_ON(sizeof(struct ax7z020_capture_caps) != 64);
	BUILD_BUG_ON(sizeof(struct ax7z020_capture_plan) != 32);
	BUILD_BUG_ON(sizeof(struct ax7z020_capture_status) != 64);
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c) return -ENOMEM;
	c->dev = get_device(&pdev->dev);
	kref_init(&c->ref);
	mutex_init(&c->lock);
	init_waitqueue_head(&c->wait);
	INIT_DELAYED_WORK(&c->monitor, ax7cap_monitor);
	atomic_set(&c->irq_events, 0);
	c->cap = devm_platform_ioremap_resource_byname(pdev, "capture");
	if (IS_ERR(c->cap)) { ret = PTR_ERR(c->cap); goto failed; }
	c->dma = devm_platform_ioremap_resource_byname(pdev, "s2mm");
	if (IS_ERR(c->dma)) { ret = PTR_ERR(c->dma); goto failed; }
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) goto failed;
	c->clock = devm_clk_get_enabled(&pdev->dev, "axi");
	if (IS_ERR(c->clock)) { ret = PTR_ERR(c->clock); goto failed; }
	c->caps.abi_version = AX7Z020_CAPTURE_ABI;
	c->caps.hardware_id = readl(c->cap + CAP_ID);
	c->caps.revision = readl(c->cap + CAP_REVISION);
	c->caps.clock_hz = readl(c->cap + CAP_CLOCK);
	c->caps.record_bytes = readl(c->cap + CAP_RECORD_BYTES);
	c->caps.speed_filter_cycles = readl(c->cap + CAP_FILTER);
	c->caps.speed_timeout_cycles = readl(c->cap + CAP_TIMEOUT);
	c->caps.adc_range_uv = readl(c->cap + CAP_RANGE);
	c->caps.channel_map = readl(c->cap + CAP_CHANNEL_MAP);
	features = readl(c->cap + CAP_FEATURES);
	c->caps.features = features & 0xff;
	c->caps.rate_mask = (features >> 8) & 0xff;
	c->caps.max_records = AX7Z020_CAPTURE_MAX_RECORDS;
	if (c->caps.hardware_id != AX7Z020_CAPTURE_HW_ID ||
	    c->caps.revision != AX7Z020_CAPTURE_REVISION ||
	    c->caps.clock_hz != 50000000 || clk_get_rate(c->clock) != 50000000 ||
	    c->caps.record_bytes != AX7Z020_CAPTURE_RECORD_BYTES ||
	    c->caps.speed_filter_cycles != 5 || c->caps.speed_timeout_cycles != 100000000 ||
	    c->caps.adc_range_uv != 5000000 || c->caps.channel_map != 0x50301 ||
	    c->caps.features != (AX7Z020_CAPTURE_F_TEST_PATTERN | AX7Z020_CAPTURE_F_SELECTABLE_RATE) ||
	    c->caps.rate_mask != AX7Z020_CAPTURE_RATE_MASK) {
		dev_err(&pdev->dev, "combined MRG1 register/clock contract mismatch\n");
		ret = -ENODEV;
		goto failed;
	}
	c->irq = platform_get_irq(pdev, 0);
	if (c->irq < 0) { ret = c->irq; goto failed; }
	/* Mask before request_irq to avoid inheriting an active boot-time IRQ. */
	writel(0, c->dma + DMA_CONTROL);
	writel(DMA_IRQ_ALL, c->dma + DMA_STATUS);
	ret = devm_request_irq(&pdev->dev, c->irq, ax7cap_irq, 0, "ax7z020-capture", c);
	if (ret) goto failed;
	ret = ax7cap_quiesce(c);
	if (ret) goto failed_irq;
	c->status.abi_version = AX7Z020_CAPTURE_ABI;
	c->status.state = AX7Z020_CAPTURE_IDLE;
	c->misc.minor = MISC_DYNAMIC_MINOR;
	c->misc.name = "ax7z020-capture";
	c->misc.fops = &ax7cap_fops;
	c->misc.parent = &pdev->dev;
	c->misc.mode = 0600;
	ret = misc_register(&c->misc);
	if (ret) goto failed_irq;
	platform_set_drvdata(pdev, c);
	dev_info(&pdev->dev, "MRG1 capture: 50MHz, 32-bit coherent DMA, exclusive /dev/%s\n", c->misc.name);
	return 0;
failed_irq:
	WRITE_ONCE(c->removed, true);
	devm_free_irq(&pdev->dev, c->irq, c);
	cancel_delayed_work_sync(&c->monitor);
failed:
	kref_put(&c->ref, ax7cap_release_ref);
	return ret;
}

static void ax7cap_remove(struct platform_device *pdev)
{
	struct ax7cap *c = platform_get_drvdata(pdev);
	misc_deregister(&c->misc);
	mutex_lock(&c->lock);
	/* IRQ may still finish here, but it cannot queue work after free_irq. */
	ax7cap_finish(c, AX7Z020_CAPTURE_CANCELLED, ECANCELED);
	WRITE_ONCE(c->removed, true);
	ax7cap_free_buffer(c);
	wake_up_interruptible(&c->wait);
	mutex_unlock(&c->lock);
	devm_free_irq(&pdev->dev, c->irq, c);
	cancel_delayed_work_sync(&c->monitor);
	kref_put(&c->ref, ax7cap_release_ref);
}

static void ax7cap_shutdown(struct platform_device *pdev)
{
	struct ax7cap *c = platform_get_drvdata(pdev);
	mutex_lock(&c->lock);
	if (!c->removed) ax7cap_finish(c, AX7Z020_CAPTURE_CANCELLED, ECANCELED);
	mutex_unlock(&c->lock);
}

static const struct of_device_id ax7cap_match[] = {
	{ .compatible = "codex,ax7z020-capture-mrg1" }, { }
};
MODULE_DEVICE_TABLE(of, ax7cap_match);
static struct platform_driver ax7cap_driver = {
	.probe = ax7cap_probe, .remove_new = ax7cap_remove, .shutdown = ax7cap_shutdown,
	.driver = { .name = "ax7z020-capture", .of_match_table = ax7cap_match },
};
module_platform_driver(ax7cap_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AX7Z020 combined CAP2 finite capture + exclusive simple S2MM DMA");

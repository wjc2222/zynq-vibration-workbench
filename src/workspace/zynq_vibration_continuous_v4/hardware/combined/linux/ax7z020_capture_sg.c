// SPDX-License-Identifier: GPL-2.0-only
/* Exclusive AX7Z020 CAP2 finite + additive STR1 stream SG S2MM capture.
 * This driver owns both MMIO windows and their single S2MM interrupt.
 * Userspace receives copies; physical addresses and mmap are never exposed.
 */
#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/crc32.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <asm/unaligned.h>
#include "ax7z020_capture_preview.h"
#include "ax7z020_capture_stream.h"

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
#define DMA_CURRENT_BD 0x38
#define DMA_CURRENT_BD_HI 0x3c
#define DMA_TAIL_BD 0x40
#define DMA_TAIL_BD_HI 0x44
#define DMA_RUN BIT(0)
#define DMA_RESET BIT(2)
#define DMA_HALTED BIT(0)
#define DMA_IDLE BIT(1)
#define DMA_SG_INCLUDED BIT(3)
#define DMA_ERRORS 0x00000770u
#define DMA_IOC BIT(12)
#define DMA_ERR_IRQ BIT(14)
#define DMA_IRQ_ALL 0x00007000u
#define DMA_IRQ_ENABLE (DMA_IOC | DMA_ERR_IRQ)
#define MONITOR_MS 5
#define FINAL_SETTLE_MS 100
#define BD_COMPLETE BIT(31)
#define BD_ERRORS GENMASK(30, 28)
#define BD_SOF BIT(27)
#define BD_EOF BIT(26)
#define BD_BYTES GENMASK(25, 0)
#define BD_ALIGN 64u
#define STR_ID 0x70
#define STR_MODE 0x74
#define STR_TOTAL_LO 0x78
#define STR_TOTAL_HI 0x7c
#define STR_CHUNK 0x80
#define STR_COMMAND 0x84
#define STR_STATE 0x88
#define STR_SNAPSHOT_SEQUENCE 0x8c
#define STR_PRODUCED 0x90
#define STR_SENT 0x98
#define STR_DROPPED 0xa0
#define STR_STOP_RECORDS 0xa8
#define STR_FIRST_TICK 0xb0
#define STR_LAST_TICK 0xb8
#define STR_START BIT(0)
#define STR_STOP BIT(1)
#define STR_SNAPSHOT BIT(2)
#define STR_ACTIVE BIT(0)
#define STR_SOURCE_DONE BIT(2)
#define STR_STREAM_DONE BIT(3)
#define STR_ERROR BIT(4)
#define STR_OVERFLOW BIT(5)

/* PG021 S2MM descriptor, no control/status stream or app fields enabled. */
struct ax7cap_bd {
	__le32 next, next_hi, buffer, buffer_hi, reserved0[2];
	__le32 control, status, app[5], padding[3];
} __aligned(BD_ALIGN);

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
	struct ax7cap_bd *descriptors;
	dma_addr_t descriptors_dma;
	size_t descriptors_bytes;
	u32 descriptor_count, completed_chunks, chunk_records, committed_records;
	u32 running_crc;
	u64 capture_id, first_tick;
	bool have_first_tick;
	unsigned long deadline;
	unsigned long final_settle_deadline;
	bool ioc_seen;
	u32 expected_source, expected_rate;
	atomic_t irq_events;
	struct ax7z020_capture_caps caps;
	struct ax7z020_capture_status status;
	bool stream_available, stream_mode;
	struct ax7z020_stream_status stream;
};

static bool ax7stream_active(struct ax7cap *c)
{
	return c->stream_mode && (c->stream.state == AX7Z020_STREAM_PREPARING ||
		c->stream.state == AX7Z020_STREAM_RUNNING ||
		c->stream.state == AX7Z020_STREAM_STOPPING);
}
static void ax7stream_check(struct ax7cap *c);
static void ax7stream_finish(struct ax7cap *c, u32 state, int error);

static bool ax7cap_active(struct ax7cap *c)
{
	return ax7stream_active(c) || c->status.state == AX7Z020_CAPTURE_PREPARING ||
	       c->status.state == AX7Z020_CAPTURE_RUNNING;
}

/* IRQ handler never takes lock, so synchronize_irq is safe while holding it.
 * Abort stops the source and drops its pending FIFO. Reset MUST have completed
 * before coherent memory can be released or reused. No display reset is used.
 */
static int ax7cap_quiesce_source(struct ax7cap *c, bool abort_source)
{
	u32 value;
	int ret;
	if (abort_source)
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

static int ax7cap_quiesce(struct ax7cap *c)
{
	return ax7cap_quiesce_source(c, true);
}

static void ax7cap_snapshot(struct ax7cap *c)
{
	c->status.hw_status = readl(c->cap + CAP_STATUS);
	c->status.dma_status = readl(c->dma + DMA_STATUS);
	/* In SG mode LENGTH is not a progress register. Only verified BD bytes. */
	c->status.dma_length = c->committed_records * c->caps.record_bytes;
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

static int ax7cap_validate_records(struct ax7cap *c, u32 first, u32 count)
{
	const u8 *data = c->buffer;
	u64 step = c->caps.clock_hz / c->expected_rate;
	u32 i;
	for (i = first; i < first + count; i++) {
		const u8 *record = data + (size_t)i * c->caps.record_bytes;
		u64 tick = get_unaligned_le64(record + 8);
		u16 flags = get_unaligned_le16(record + 22);
		if (!c->have_first_tick) {
			c->first_tick = tick;
			if (tick > U64_MAX - (u64)(c->status.requested_records - 1) * step)
				return -EPROTO;
			c->have_first_tick = true;
		}
		if (get_unaligned_le64(record) != i ||
		    tick != c->first_tick + (u64)i * step || !(flags & 1) ||
		    (flags & ~31u) ||
		    ((flags & 2) && (!get_unaligned_le32(record + 24) || (flags & 4))))
			return -EPROTO;
	}
	return 0;
}

/* No publication based on AXIS handshakes or PL produced/sent counters.
 * The complete bit is the DMA hardware's ownership handoff after this BD's
 * data transfer. All BDs are preposted, finite and never reused during a job.
 */
static int ax7cap_collect_completed(struct ax7cap *c)
{
	bool advanced = false;
	while (c->completed_chunks < c->descriptor_count) {
		u32 index = c->completed_chunks;
		u32 status = le32_to_cpu(READ_ONCE(c->descriptors[index].status));
		u32 count = min(c->chunk_records,
				c->status.requested_records - c->committed_records);
		u32 bytes = count * c->caps.record_bytes;
		int ret;
		if (!(status & BD_COMPLETE))
			break;
		dma_rmb();
		if ((status & BD_ERRORS) || (status & BD_BYTES) != bytes ||
		    !!(status & BD_SOF) != (index == 0) ||
		    !!(status & BD_EOF) != (index == c->descriptor_count - 1))
			return -EPROTO;
		ret = ax7cap_validate_records(c, c->committed_records, count);
		if (ret)
			return ret;
		c->running_crc = crc32_le(c->running_crc,
			(u8 *)c->buffer + (size_t)c->committed_records * c->caps.record_bytes,
			bytes);
		c->committed_records += count;
		c->completed_chunks++;
		advanced = true;
	}
	c->status.dma_length = c->committed_records * c->caps.record_bytes;
	if (advanced)
		wake_up_interruptible(&c->wait);
	return 0;
}

static bool ax7cap_bad_status(struct ax7cap *c)
{
	return (c->status.dma_status & DMA_ERRORS) ||
	       !(c->status.dma_status & DMA_SG_INCLUDED) ||
	       (c->status.hw_status & (CAP_ERRORS | CAP_ADC_ERRORS)) ||
	       c->status.dropped_records || readl(c->cap + CAP_REJECT) ||
	       c->status.sample_rate_hz != c->expected_rate ||
	       readl(c->cap + CAP_SOURCE) != c->expected_source ||
	       !!(c->status.hw_status & CAP_IS_TEST) != c->expected_source;
}

static void ax7cap_check(struct ax7cap *c)
{
	u32 events;
	int ret;
	if (c->stream_mode) {
		ax7stream_check(c);
		return;
	}
	if (c->status.state != AX7Z020_CAPTURE_RUNNING)
		return;
	ax7cap_snapshot(c);
	events = atomic_xchg(&c->irq_events, 0);
	/* IRQ acknowledges W1C bits; retain them in the terminal evidence. */
	c->status.dma_status |= events;
	if (c->status.dma_status & DMA_IOC) {
		if (!c->ioc_seen)
			c->final_settle_deadline = jiffies + msecs_to_jiffies(FINAL_SETTLE_MS);
		c->ioc_seen = true;
	}
	if (c->ioc_seen)
		c->status.dma_status |= DMA_IOC;
	if (ax7cap_bad_status(c)) {
		ax7cap_finish(c, AX7Z020_CAPTURE_FAILED, EIO);
		return;
	}
	ret = ax7cap_collect_completed(c);
	if (ret) {
		ax7cap_finish(c, AX7Z020_CAPTURE_FAILED, -ret);
		return;
	}
	if (c->ioc_seen) {
		/* IOC may arrive between the earlier CAP and DMA register reads.
		 * Resample final state, retaining the IRQ evidence already W1C-acked.
		 * A bounded wait changes no acceptance predicate: incomplete BDs are
		 * never read/published and every exact final check must still pass.
		 */
		dma_rmb();
		ret = ax7cap_collect_completed(c);
		if (ret) {
			ax7cap_finish(c, AX7Z020_CAPTURE_FAILED, -ret);
			return;
		}
		ax7cap_snapshot(c);
		c->status.dma_status |= DMA_IOC;
		if (ax7cap_bad_status(c)) {
			ax7cap_finish(c, AX7Z020_CAPTURE_FAILED, EIO);
			return;
		}
		/* Idle, TLAST byte count and all producer counters must agree. */
		if (!(c->status.dma_status & DMA_IDLE) ||
		    c->completed_chunks != c->descriptor_count ||
		    c->committed_records != c->status.requested_records ||
		    c->status.dma_length != c->buffer_bytes ||
		    c->status.captured_records != c->status.requested_records ||
		    c->status.sent_records != c->status.requested_records ||
		    c->status.dropped_records ||
		    (c->status.hw_status & 7) != CAP_COMPLETE) {
			if (time_after_eq(jiffies, c->final_settle_deadline))
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
		if (c->status.state == AX7Z020_CAPTURE_RUNNING || ax7stream_active(c))
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
	if (c->descriptors && !c->poisoned) {
		dma_free_coherent(c->dev, c->descriptors_bytes, c->descriptors,
				  c->descriptors_dma);
		c->descriptors = NULL;
		c->descriptors_bytes = 0;
	}
	if (!c->poisoned) {
		c->buffer_bytes = 0;
		c->descriptors_bytes = 0;
		c->descriptor_count = 0;
		c->completed_chunks = 0;
		c->committed_records = 0;
		c->chunk_records = 0;
		c->running_crc = ~0u;
		c->have_first_tick = false;
		c->ioc_seen = false;
		c->final_settle_deadline = 0;
	}
}

static void ax7cap_release_ref(struct kref *ref)
{
	struct ax7cap *c = container_of(ref, struct ax7cap, ref);
	/* In the exceptional reset-failure case neither allocator memory nor its
	 * device reference is released: hardware could still write that address.
	 */
	if (!(c->poisoned && (c->buffer || c->descriptors)))
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
	if (!c->removed && ax7cap_active(c)) {
		if (c->stream_mode)
			ax7stream_finish(c, AX7Z020_STREAM_CANCELLED, ECANCELED);
		else
			ax7cap_finish(c, AX7Z020_CAPTURE_CANCELLED, ECANCELED);
	}
	ax7cap_free_buffer(c);
	if (!c->removed && !c->poisoned) {
		memset(&c->status, 0, sizeof(c->status));
		c->status.abi_version = AX7Z020_CAPTURE_ABI;
		c->status.state = AX7Z020_CAPTURE_IDLE;
		c->stream_mode = false;
		memset(&c->stream, 0, sizeof(c->stream));
		c->stream.abi_version = AX7Z020_STREAM_ABI;
		c->stream.state = AX7Z020_STREAM_IDLE;
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

static int ax7cap_build_chain(struct ax7cap *c)
{
	u32 i;
	c->chunk_records = c->expected_rate / 25;
	c->descriptor_count = DIV_ROUND_UP(c->status.requested_records, c->chunk_records);
	c->descriptors_bytes = (size_t)c->descriptor_count * sizeof(*c->descriptors);
	c->descriptors = dma_alloc_coherent(c->dev, c->descriptors_bytes,
					    &c->descriptors_dma, GFP_KERNEL);
	if (!c->descriptors)
		return -ENOMEM;
	if (!IS_ALIGNED(c->descriptors_dma, BD_ALIGN) ||
	    upper_32_bits((u64)c->descriptors_dma + c->descriptors_bytes - 1))
		return -ERANGE;
	memset(c->descriptors, 0, c->descriptors_bytes);
	for (i = 0; i < c->descriptor_count; i++) {
		u32 first = i * c->chunk_records;
		u32 count = min(c->chunk_records, c->status.requested_records - first);
		dma_addr_t next = c->descriptors_dma +
			((i + 1) % c->descriptor_count) * sizeof(*c->descriptors);
		struct ax7cap_bd *bd = &c->descriptors[i];
		bd->next = cpu_to_le32(lower_32_bits(next));
		bd->buffer = cpu_to_le32(lower_32_bits(c->buffer_dma +
					 (size_t)first * c->caps.record_bytes));
		bd->control = cpu_to_le32(count * c->caps.record_bytes);
	}
	/* All descriptors/data addresses are committed before the hardware tail.
	 * The valid final next pointer wraps to the first, but cyclic mode is OFF
	 * and the finite tail prevents any descriptor from being consumed twice.
	 */
	dma_wmb();
	return 0;
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
	c->stream_mode = false;
	memset(&c->stream, 0, sizeof(c->stream));
	c->stream.abi_version = AX7Z020_STREAM_ABI;
	c->stream.state = AX7Z020_STREAM_IDLE;
	if (c->stream_available)
		writel(0, c->cap + STR_MODE);
	memset(&c->status, 0, sizeof(c->status));
	c->status.abi_version = AX7Z020_CAPTURE_ABI;
	c->status.state = AX7Z020_CAPTURE_PREPARING;
	c->status.requested_records = p->requested_records;
	c->expected_source = source;
	c->expected_rate = p->sample_rate_hz;
	c->capture_id++;
	if (!c->capture_id)
		c->capture_id++;
	c->running_crc = ~0u;
	c->have_first_tick = false;
	c->buffer_bytes = (size_t)p->requested_records * c->caps.record_bytes;
	c->buffer = dma_alloc_coherent(c->dev, c->buffer_bytes,
				       &c->buffer_dma, GFP_KERNEL);
	if (!c->buffer) {
		ret = -ENOMEM;
		goto failed;
	}
	if (upper_32_bits((u64)c->buffer_dma + c->buffer_bytes - 1)) {
		ret = -ERANGE;
		goto failed;
	}
	ret = ax7cap_build_chain(c);
	if (ret)
		goto failed;
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
	/* CURDESC must be written while stopped; prepost the entire finite chain.
	 * Interrupt threshold1 means the single packet's final EOF raises IOC.
	 */
	writel(lower_32_bits(c->descriptors_dma), c->dma + DMA_CURRENT_BD);
	writel(0, c->dma + DMA_CURRENT_BD_HI);
	writel(DMA_RUN | DMA_IRQ_ENABLE | BIT(16), c->dma + DMA_CONTROL);
	ret = readl_poll_timeout(c->dma + DMA_STATUS, value,
				 !(value & DMA_HALTED), 10, 100000);
	if (ret)
		goto failed;
	dma_wmb();
	writel(0, c->dma + DMA_TAIL_BD_HI);
	writel(lower_32_bits(c->descriptors_dma +
		(c->descriptor_count - 1) * sizeof(*c->descriptors)),
		c->dma + DMA_TAIL_BD); /* LAST: arm the whole finite chain. */
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

/* PG021 7.1, Descriptor Management / S2MM_TAILDESC:
 * https://docs.amd.com/r/en-US/pg021_axi_dma/Descriptor-Management
 * Only Completed BDs may be processed and reallocated. Clear completion before
 * moving TAILDESC to the last reallocated BD. A tail write resumes an idle SG
 * engine or moves its future pause point. Cyclic mode is deliberately OFF:
 * unread CPU-owned records must never be silently overwritten.
 */
static u64 ax7stream_reg64(struct ax7cap *c, u32 offset)
{
	return (u64)readl(c->cap + offset) |
		((u64)readl(c->cap + offset + 4) << 32);
}

static bool ax7stream_multiple(u64 count, u32 divisor)
{
	u32 remainder;
	div_u64_rem(count, divisor, &remainder);
	return remainder == 0;
}

static int ax7stream_snapshot(struct ax7cap *c)
{
	u32 previous = readl(c->cap + STR_SNAPSHOT_SEQUENCE), value;
	int ret;
	writel(STR_SNAPSHOT, c->cap + STR_COMMAND);
	ret = readl_poll_timeout(c->cap + STR_SNAPSHOT_SEQUENCE, value,
		value != previous, 1, 100000);
	if (ret)
		return ret;
	c->stream.hw_status = readl(c->cap + STR_STATE);
	c->stream.produced_records = ax7stream_reg64(c, STR_PRODUCED);
	c->stream.sent_records = ax7stream_reg64(c, STR_SENT);
	c->stream.dropped_records = ax7stream_reg64(c, STR_DROPPED);
	c->stream.stop_records = ax7stream_reg64(c, STR_STOP_RECORDS);
	c->stream.dma_status = readl(c->dma + DMA_STATUS) |
		atomic_xchg(&c->irq_events, 0);
	return 0;
}

static int ax7stream_collect(struct ax7cap *c)
{
	u32 records = c->stream.chunk_records, bytes = records * 32;
	bool advanced = false;
	int error = 0;
	while (c->stream.completed_chunks - c->stream.delivered_chunks < c->descriptor_count) {
		u32 index = c->stream.completed_chunks & (AX7Z020_STREAM_RING_CHUNKS - 1);
		u32 status = le32_to_cpu(READ_ONCE(c->descriptors[index].status));
		const u8 *data = (u8 *)c->buffer + (size_t)index * bytes;
		u64 first = c->stream.committed_records;
		u64 step = c->caps.clock_hz / c->stream.sample_rate_hz;
		u32 i;
		if (!(status & BD_COMPLETE))
			break;
		dma_rmb();
		/* Each stream chunk is exactly one complete AXIS packet/one BD. */
		if ((status & BD_ERRORS) || (status & BD_BYTES) != bytes ||
		    !(status & BD_SOF) || !(status & BD_EOF) || first > U64_MAX - records) {
			error = -EPROTO;
			goto out;
		}
		for (i = 0; i < records; ++i) {
			const u8 *record = data + (size_t)i * 32;
			u64 tick = get_unaligned_le64(record + 8), sequence = first + i;
			u16 flags = get_unaligned_le16(record + 22);
			if (!c->have_first_tick) {
				c->stream.first_tick = tick;
				c->have_first_tick = true;
			}
			if (sequence > div_u64(U64_MAX - c->stream.first_tick, (u32)step) ||
			    get_unaligned_le64(record) != sequence ||
			    tick != c->stream.first_tick + sequence * step ||
			    !(flags & 1) || (flags & ~31u) ||
			    ((flags & 2) && (!get_unaligned_le32(record + 24) || (flags & 4)))) {
				error = -EPROTO;
				goto out;
			}
		}
		c->stream.last_tick = get_unaligned_le64(data + bytes - 24);
		c->stream.committed_records += records;
		c->stream.completed_chunks++;
		advanced = true;
	}
out:
	c->stream.queued_chunks = c->stream.completed_chunks - c->stream.delivered_chunks;
	if (advanced) {
		c->deadline = jiffies + msecs_to_jiffies(2000);
		wake_up_interruptible(&c->wait);
	}
	return error;
}

static void ax7stream_finish(struct ax7cap *c, u32 state, int error)
{
	int ret;
	/* Retain the validated prefix and evidence before ABORT/reset. Normal STOP
	 * reaches this function only AFTER source, stream and last BD have drained.
	 */
	if (c->stream.state != AX7Z020_STREAM_PREPARING && ax7stream_snapshot(c) && !error)
		error = EIO;
	ret = ax7cap_quiesce_source(c, state != AX7Z020_STREAM_READY || error);
	if (ret) {
		state = AX7Z020_STREAM_FAILED;
		error = EIO;
		dev_crit(c->dev, "stream DMA did not quiesce; buffer quarantined\n");
	}
	if (error && state == AX7Z020_STREAM_READY)
		state = AX7Z020_STREAM_FAILED;
	c->stream.error_code = error;
	c->stream.state = state;
	cancel_delayed_work(&c->monitor);
	wake_up_interruptible(&c->wait);
}

static void ax7stream_check(struct ax7cap *c)
{
	u32 state;
	int ret;
	if (!ax7stream_active(c))
		return;
	ret = ax7stream_snapshot(c);
	if (ret) {
		ax7stream_finish(c, AX7Z020_STREAM_FAILED, -ret);
		return;
	}
	/* Even on producer/DMA failure retain every preceding validated BD. */
	ret = ax7stream_collect(c);
	if (ret) {
		ax7stream_finish(c, AX7Z020_STREAM_FAILED, -ret);
		return;
	}
	state = c->stream.hw_status;
	if ((c->stream.dma_status & DMA_ERRORS) ||
	    !(c->stream.dma_status & DMA_SG_INCLUDED) ||
	    (state & (STR_ERROR | STR_OVERFLOW)) || c->stream.dropped_records ||
	    (readl(c->cap + CAP_STATUS) & (CAP_ERRORS | CAP_ADC_ERRORS)) ||
	    readl(c->cap + CAP_REJECT) || readl(c->cap + CAP_RATE) != c->expected_rate ||
	    readl(c->cap + CAP_SOURCE) != c->expected_source) {
		ax7stream_finish(c, AX7Z020_STREAM_FAILED, EIO);
		return;
	}
	if ((state & (STR_SOURCE_DONE | STR_STREAM_DONE | STR_ACTIVE)) ==
	    (STR_SOURCE_DONE | STR_STREAM_DONE)) {
		u64 final = c->stream.stop_records;
		/* DMA may await another armed BD, so DMA_IDLE is not a stream-end
		 * predicate. The exact final BD writeback is the ownership authority.
		 */
		if (final && ax7stream_multiple(final, c->stream.chunk_records) &&
		    final == c->stream.produced_records && final == c->stream.sent_records &&
		    final == c->stream.committed_records &&
		    ((!c->stream.total_records) || final <= c->stream.total_records) &&
		    ((c->stream.flags & AX7Z020_STREAM_STATUS_STOP_REQUESTED) ||
		     final == c->stream.total_records)) {
			ax7stream_finish(c, AX7Z020_STREAM_READY, 0);
			return;
		}
		if (!c->final_settle_deadline)
			c->final_settle_deadline = jiffies + msecs_to_jiffies(FINAL_SETTLE_MS);
		if (time_after_eq(jiffies, c->final_settle_deadline))
			ax7stream_finish(c, AX7Z020_STREAM_FAILED, EPROTO);
		return;
	}
	if (c->stream.queued_chunks == c->descriptor_count) {
		/* Tail mode paused safely; do not wait for PL FIFO to overflow. */
		ax7stream_finish(c, AX7Z020_STREAM_FAILED, ENOBUFS);
		return;
	}
	if (time_after_eq(jiffies, c->deadline))
		ax7stream_finish(c, AX7Z020_STREAM_FAILED, ETIMEDOUT);
}

static int ax7stream_start(struct ax7cap *c, struct file *file,
			   const struct ax7z020_stream_plan *p)
{
	u32 i, value, source = !!(p->flags & AX7Z020_STREAM_START_TEST_PATTERN);
	int selector = ax7cap_selector(p->sample_rate_hz), ret;
	if (!c->stream_available)
		return -EOPNOTSUPP;
	if (p->abi_version != AX7Z020_STREAM_ABI || selector < 0 ||
	    (p->ring_chunks && p->ring_chunks != AX7Z020_STREAM_RING_CHUNKS) ||
	    (p->flags & ~AX7Z020_STREAM_START_TEST_PATTERN) ||
	    (p->total_records && !ax7stream_multiple(p->total_records, p->sample_rate_hz)) ||
	    memchr_inv(p->reserved, 0, sizeof(p->reserved)))
		return -EINVAL;
	if (ax7cap_active(c))
		return -EBUSY;
	if (c->poisoned)
		return -EIO;
	ret = ax7cap_quiesce(c);
	if (ret)
		return ret;
	ax7cap_free_buffer(c);
	memset(&c->status, 0, sizeof(c->status));
	c->status.abi_version = AX7Z020_CAPTURE_ABI;
	c->status.state = AX7Z020_CAPTURE_IDLE;
	memset(&c->stream, 0, sizeof(c->stream));
	c->stream_mode = true;
	c->stream.abi_version = AX7Z020_STREAM_ABI;
	c->stream.state = AX7Z020_STREAM_PREPARING;
	c->stream.sample_rate_hz = p->sample_rate_hz;
	c->stream.chunk_records = p->sample_rate_hz / 25;
	c->stream.ring_chunks = AX7Z020_STREAM_RING_CHUNKS;
	c->stream.total_records = p->total_records;
	c->stream.flags = source ? AX7Z020_STREAM_STATUS_TEST_PATTERN : 0;
	c->expected_source = source;
	c->expected_rate = p->sample_rate_hz;
	c->capture_id++;
	if (!c->capture_id)
		c->capture_id++;
	c->stream.capture_id = c->capture_id;
	c->descriptor_count = AX7Z020_STREAM_RING_CHUNKS;
	c->buffer_bytes = (size_t)c->descriptor_count * c->stream.chunk_records * 32;
	c->descriptors_bytes = (size_t)c->descriptor_count * sizeof(*c->descriptors);
	c->buffer = dma_alloc_coherent(c->dev, c->buffer_bytes, &c->buffer_dma, GFP_KERNEL);
	c->descriptors = dma_alloc_coherent(c->dev, c->descriptors_bytes,
		&c->descriptors_dma, GFP_KERNEL);
	if (!c->buffer || !c->descriptors) { ret = -ENOMEM; goto failed; }
	if (!IS_ALIGNED(c->descriptors_dma, BD_ALIGN) ||
	    upper_32_bits((u64)c->buffer_dma + c->buffer_bytes - 1) ||
	    upper_32_bits((u64)c->descriptors_dma + c->descriptors_bytes - 1)) {
		ret = -ERANGE; goto failed;
	}
	memset(c->descriptors, 0, c->descriptors_bytes);
	for (i = 0; i < c->descriptor_count; ++i) {
		struct ax7cap_bd *bd = &c->descriptors[i];
		bd->next = cpu_to_le32(lower_32_bits(c->descriptors_dma +
			((i + 1) % c->descriptor_count) * sizeof(*bd)));
		bd->buffer = cpu_to_le32(lower_32_bits(c->buffer_dma +
			(size_t)i * c->stream.chunk_records * 32));
		bd->control = cpu_to_le32(c->stream.chunk_records * 32);
	}
	writel(CAP_CLEAR_REJECT, c->cap + CAP_COMMAND);
	writel(selector, c->cap + CAP_RATE_SELECT);
	writel(source, c->cap + CAP_SOURCE);
	writel(1, c->cap + STR_MODE);
	writel(lower_32_bits(p->total_records), c->cap + STR_TOTAL_LO);
	writel(upper_32_bits(p->total_records), c->cap + STR_TOTAL_HI);
	writel(c->stream.chunk_records, c->cap + STR_CHUNK);
	if (readl(c->cap + STR_MODE) != 1 || readl(c->cap + STR_CHUNK) != c->stream.chunk_records ||
	    readl(c->cap + STR_TOTAL_LO) != lower_32_bits(p->total_records) ||
	    readl(c->cap + STR_TOTAL_HI) != upper_32_bits(p->total_records) ||
	    readl(c->cap + CAP_RATE) != p->sample_rate_hz ||
	    readl(c->cap + CAP_SOURCE) != source || readl(c->cap + CAP_REJECT)) {
		ret = -EPROTO; goto failed;
	}
	writel(CAP_ADC_RESET, c->cap + CAP_COMMAND);
	if (!source) {
		ret = readl_poll_timeout(c->cap + CAP_ADC_FRAMES, value, value < 4, 10, 100000);
		if (!ret)
			ret = readl_poll_timeout(c->cap + CAP_ADC_FRAMES, value, value >= 4, 50, 2000000);
		if (ret || (readl(c->cap + CAP_STATUS) & CAP_ADC_ERRORS)) {
			ret = ret ?: -EIO; goto failed;
		}
	}
	if (readl(c->cap + CAP_REJECT)) { ret = -EPROTO; goto failed; }
	writel(DMA_IRQ_ALL, c->dma + DMA_STATUS);
	atomic_set(&c->irq_events, 0);
	dma_wmb();
	writel(lower_32_bits(c->descriptors_dma), c->dma + DMA_CURRENT_BD);
	writel(0, c->dma + DMA_CURRENT_BD_HI);
	writel(DMA_RUN | DMA_IRQ_ENABLE | BIT(16), c->dma + DMA_CONTROL);
	ret = readl_poll_timeout(c->dma + DMA_STATUS, value, !(value & DMA_HALTED), 10, 100000);
	if (ret) goto failed;
	writel(0, c->dma + DMA_TAIL_BD_HI);
	writel(lower_32_bits(c->descriptors_dma +
		(c->descriptor_count - 1) * sizeof(*c->descriptors)), c->dma + DMA_TAIL_BD);
	readl(c->dma + DMA_CONTROL);
	c->stream.state = AX7Z020_STREAM_RUNNING;
	c->deadline = jiffies + msecs_to_jiffies(2000);
	file->f_pos = 0;
	writel(STR_START, c->cap + STR_COMMAND);
	mod_delayed_work(system_wq, &c->monitor, msecs_to_jiffies(MONITOR_MS));
	return 0;
failed:
	ax7stream_finish(c, AX7Z020_STREAM_FAILED, -ret);
	return ret;
}

static ssize_t ax7stream_read_locked(struct ax7cap *c, char __user *out, size_t count)
{
	u32 index, bytes = c->stream.chunk_records * 32;
	if (c->stream.delivered_chunks == c->stream.completed_chunks) {
		if (ax7stream_active(c)) return -EAGAIN;
		if (c->stream.state == AX7Z020_STREAM_READY) return 0;
		return c->stream.error_code ? -(int)c->stream.error_code : -ENODATA;
	}
	if (count < bytes) return -EMSGSIZE;
	index = c->stream.delivered_chunks & (AX7Z020_STREAM_RING_CHUNKS - 1);
	if (copy_to_user(out, (u8 *)c->buffer + (size_t)index * bytes, bytes))
		return -EFAULT;
	c->stream.delivered_chunks++;
	c->stream.delivered_records += c->stream.chunk_records;
	c->stream.queued_chunks = c->stream.completed_chunks - c->stream.delivered_chunks;
	if (ax7stream_active(c)) {
		/* Copy succeeded: ownership can move from CPU back to DMA. Fixed
		 * next/buffer/control remain immutable. Never rearm before this point.
		 */
		WRITE_ONCE(c->descriptors[index].status, cpu_to_le32(0));
		dma_wmb();
		writel(lower_32_bits(c->descriptors_dma + index * sizeof(*c->descriptors)),
			c->dma + DMA_TAIL_BD);
	}
	return bytes;
}

static int ax7cap_preview_info(struct ax7cap *c, void __user *user)
{
	struct ax7z020_capture_preview_info info = {0};
	ax7cap_check(c);
	if (c->status.state == AX7Z020_CAPTURE_IDLE)
		ax7cap_snapshot(c);
	info.abi_version = AX7Z020_CAPTURE_PREVIEW_ABI;
	info.state = c->status.state;
	info.error_code = c->status.error_code;
	info.source_flags = c->status.flags;
	info.capture_id = c->status.state == AX7Z020_CAPTURE_IDLE ? 0 : c->capture_id;
	info.sample_rate_hz = c->status.sample_rate_hz;
	info.record_bytes = c->caps.record_bytes;
	info.requested_records = c->status.requested_records;
	info.committed_records = c->committed_records;
	info.chunk_records = c->chunk_records;
	info.completed_chunks = c->completed_chunks;
	info.total_chunks = c->descriptor_count;
	info.features = c->caps.features;
	info.committed_crc32 = c->running_crc ^ ~0u;
	return copy_to_user(user, &info, sizeof(info)) ? -EFAULT : 0;
}

static int ax7cap_preview_copy(struct ax7cap *c, void __user *user)
{
	struct ax7z020_capture_preview_copy *copy;
	u32 first, requested, flags, count, bytes;
	u64 capture_id;
	int ret = 0;
	/* The8272-byte result must never be placed on ARM's kernel stack. */
	copy = kzalloc(sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	if (copy_from_user(copy, user, offsetof(struct ax7z020_capture_preview_copy, records))) {
		ret = -EFAULT;
		goto out;
	}
	first = copy->first_record;
	requested = copy->requested_records;
	flags = copy->flags;
	capture_id = copy->capture_id;
	if (copy->abi_version != AX7Z020_CAPTURE_PREVIEW_ABI ||
	    !requested || requested > AX7Z020_CAPTURE_PREVIEW_MAX_RECORDS ||
	    (flags & ~AX7Z020_CAPTURE_PREVIEW_LATEST) ||
	    memchr_inv(copy->reserved, 0, sizeof(copy->reserved))) {
		ret = -EINVAL;
		goto out;
	}
	ax7cap_check(c);
	if (!capture_id || capture_id != c->capture_id) {
		ret = -ESTALE;
		goto out;
	}
	if (c->status.state != AX7Z020_CAPTURE_RUNNING &&
	    c->status.state != AX7Z020_CAPTURE_READY) {
		ret = c->status.error_code ? -(int)c->status.error_code : -ENODATA;
		goto out;
	}
	if (!(flags & AX7Z020_CAPTURE_PREVIEW_LATEST) && first > c->status.requested_records) {
		ret = -ERANGE;
		goto out;
	}
	if (flags & AX7Z020_CAPTURE_PREVIEW_LATEST)
		first = c->committed_records - min(requested, c->committed_records);
	if (first >= c->committed_records &&
	    !(first == c->committed_records && c->status.state == AX7Z020_CAPTURE_READY)) {
		ret = -EAGAIN;
		goto out;
	}
	count = min(requested, c->committed_records - first);
	bytes = count * c->caps.record_bytes;
	memset(copy, 0, sizeof(*copy));
	copy->abi_version = AX7Z020_CAPTURE_PREVIEW_ABI;
	copy->flags = flags;
	copy->first_record = first;
	copy->requested_records = requested;
	copy->capture_id = c->capture_id;
	copy->record_count = count;
	copy->committed_records = c->committed_records;
	copy->sample_rate_hz = c->status.sample_rate_hz;
	copy->source_flags = c->status.flags;
	copy->state = c->status.state;
	copy->record_bytes = c->caps.record_bytes;
	copy->chunk_records = c->chunk_records;
	copy->validity_flags = AX7Z020_CAPTURE_PREVIEW_V_COMMITTED |
		AX7Z020_CAPTURE_PREVIEW_V_RECORDS_VALID |
		AX7Z020_CAPTURE_PREVIEW_V_SOURCE_VERIFIED |
		AX7Z020_CAPTURE_PREVIEW_V_CRC32;
	if (c->status.state == AX7Z020_CAPTURE_READY)
		copy->validity_flags |= AX7Z020_CAPTURE_PREVIEW_V_JOB_COMPLETE;
	dma_rmb();
	memcpy(copy->records, (u8 *)c->buffer + (size_t)first * c->caps.record_bytes, bytes);
	copy->payload_crc32 = crc32_le(~0u, copy->records, bytes) ^ ~0u;
	if (copy_to_user(user, copy, sizeof(*copy)))
		ret = -EFAULT;
out:
	kfree(copy);
	return ret;
}

static long ax7cap_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ax7cap *c = file->private_data;
	struct ax7z020_capture_plan plan;
	struct ax7z020_stream_plan stream_plan;
	void __user *user = (void __user *)arg;
	int ret = 0;
	if (cmd == AX7Z020_CAPTURE_START && copy_from_user(&plan, user, sizeof(plan)))
		return -EFAULT;
	if (cmd == AX7Z020_STREAM_START && copy_from_user(&stream_plan, user, sizeof(stream_plan)))
		return -EFAULT;
	if (mutex_lock_interruptible(&c->lock))
		return -ERESTARTSYS;
	if (c->removed) {
		ret = -ENODEV;
		goto out;
	}
	switch (cmd) {
	case AX7Z020_STREAM_GET_CAPS: {
		struct ax7z020_stream_caps caps = {
			.abi_version = AX7Z020_STREAM_ABI,
			.features = AX7Z020_STREAM_F_TIMED | AX7Z020_STREAM_F_BOUNDARY_STOP | AX7Z020_STREAM_F_VERIFIED_READ,
			.clock_hz = 50000000, .record_bytes = 32, .rate_mask = AX7Z020_CAPTURE_RATE_MASK,
			.chunk_period_us = AX7Z020_STREAM_CHUNK_PERIOD_US,
			.ring_chunks = AX7Z020_STREAM_RING_CHUNKS,
			.max_chunk_records = AX7Z020_STREAM_MAX_CHUNK_RECORDS,
			.max_chunk_bytes = AX7Z020_STREAM_MAX_CHUNK_BYTES,
			.max_ring_bytes = AX7Z020_STREAM_RING_CHUNKS * AX7Z020_STREAM_MAX_CHUNK_BYTES
		};
		if (!c->stream_available) ret = -EOPNOTSUPP;
		else if (copy_to_user(user, &caps, sizeof(caps))) ret = -EFAULT;
		break;
	}
	case AX7Z020_STREAM_START:
		ret = ax7stream_start(c, file, &stream_plan);
		break;
	case AX7Z020_STREAM_STOP:
		if (!c->stream_mode) { ret = -ENODATA; break; }
		ax7stream_check(c);
		if (ax7stream_active(c)) {
			c->stream.flags |= AX7Z020_STREAM_STATUS_STOP_REQUESTED;
			c->stream.state = AX7Z020_STREAM_STOPPING;
			writel(STR_STOP, c->cap + STR_COMMAND);
		}
		break;
	case AX7Z020_STREAM_GET_STATUS:
		if (!c->stream_available) { ret = -EOPNOTSUPP; break; }
		ax7stream_check(c);
		if (copy_to_user(user, &c->stream, sizeof(c->stream))) ret = -EFAULT;
		break;
	case AX7Z020_CAPTURE_GET_PREVIEW_INFO:
		if (c->stream_mode) { ret = -EOPNOTSUPP; break; }
		ret = ax7cap_preview_info(c, user);
		break;
	case AX7Z020_CAPTURE_COPY_PREVIEW:
		if (c->stream_mode) { ret = -EOPNOTSUPP; break; }
		ret = ax7cap_preview_copy(c, user);
		break;
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
		if (c->stream_mode) {
			ax7stream_check(c);
			if (ax7stream_active(c))
				ax7stream_finish(c, AX7Z020_STREAM_CANCELLED, ECANCELED);
		} else if (ax7cap_active(c))
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
	if (c->stream_mode) {
		ax7stream_check(c);
		ret = ax7stream_read_locked(c, out, count);
		if (ret != -EAGAIN || (file->f_flags & O_NONBLOCK)) goto done;
		mutex_unlock(&c->lock);
		ret = wait_event_interruptible(c->wait,
			READ_ONCE(c->removed) ||
			READ_ONCE(c->stream.completed_chunks) != READ_ONCE(c->stream.delivered_chunks) ||
			(READ_ONCE(c->stream.state) != AX7Z020_STREAM_RUNNING &&
			 READ_ONCE(c->stream.state) != AX7Z020_STREAM_STOPPING));
		if (ret) return ret;
		goto retry;
	}
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
	else if (c->stream_mode) {
		ax7stream_check(c);
		if (c->stream.completed_chunks != c->stream.delivered_chunks)
			result |= EPOLLIN | EPOLLRDNORM;
		if (c->stream.state == AX7Z020_STREAM_READY && !c->stream.queued_chunks)
			result |= EPOLLHUP;
		if (c->stream.error_code) result |= EPOLLERR;
	}
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
	u32 value;
	int ret;
	BUILD_BUG_ON(sizeof(struct ax7z020_capture_caps) != 64);
	BUILD_BUG_ON(sizeof(struct ax7z020_capture_plan) != 32);
	BUILD_BUG_ON(sizeof(struct ax7z020_capture_status) != 64);
	BUILD_BUG_ON(sizeof(struct ax7z020_stream_caps) != 64);
	BUILD_BUG_ON(sizeof(struct ax7z020_stream_plan) != 48);
	BUILD_BUG_ON(sizeof(struct ax7z020_stream_status) != 160);
	BUILD_BUG_ON(sizeof(struct ax7cap_bd) != BD_ALIGN);
	BUILD_BUG_ON(sizeof(struct ax7z020_capture_preview_info) != 64);
	BUILD_BUG_ON(sizeof(struct ax7z020_capture_preview_copy) != 8272);
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c) return -ENOMEM;
	c->dev = get_device(&pdev->dev);
	kref_init(&c->ref);
	mutex_init(&c->lock);
	init_waitqueue_head(&c->wait);
	INIT_DELAYED_WORK(&c->monitor, ax7cap_monitor);
	atomic_set(&c->irq_events, 0);
	c->running_crc = ~0u;
	c->cap = devm_platform_ioremap_resource_byname(pdev, "capture");
	if (IS_ERR(c->cap)) { ret = PTR_ERR(c->cap); goto failed; }
	c->dma = devm_platform_ioremap_resource_byname(pdev, "s2mm");
	if (IS_ERR(c->dma)) { ret = PTR_ERR(c->dma); goto failed; }
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) goto failed;
	c->clock = devm_clk_get_enabled(&pdev->dev, "axi");
	if (IS_ERR(c->clock)) { ret = PTR_ERR(c->clock); goto failed; }
	if (of_property_read_u32(pdev->dev.of_node, "codex,sg-length-width", &value) || value != 26 ||
	    of_property_read_u32(pdev->dev.of_node, "codex,dma-address-width", &value) || value != 32 ||
	    of_property_read_u32(pdev->dev.of_node, "codex,sg-control-status-stream", &value) || value != 0 ||
	    of_property_read_u32(pdev->dev.of_node, "codex,preview-period-us", &value) || value != AX7Z020_CAPTURE_PREVIEW_PERIOD_US) {
		dev_err(&pdev->dev, "SG DT capability declaration mismatch\n");
		ret = -ENODEV;
		goto failed;
	}
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
	if (!(readl(c->dma + DMA_STATUS) & DMA_SG_INCLUDED)) {
		dev_err(&pdev->dev, "SG preview driver refuses simple DMA hardware\n");
		ret = -ENODEV;
		goto failed;
	}
	c->caps.features |= AX7Z020_CAPTURE_F_PREVIEW_COMMITTED | AX7Z020_CAPTURE_F_SG_CHAIN;
	c->stream_available = readl(c->cap + STR_ID) == AX7Z020_STREAM_HW_ID;
	c->stream.abi_version = AX7Z020_STREAM_ABI;
	c->stream.state = AX7Z020_STREAM_IDLE;
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
	dev_info(&pdev->dev, "MRG1 SG capture: 50MHz, preposted finite chain,40ms committed preview /dev/%s\n", c->misc.name);
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
	if (c->stream_mode)
		ax7stream_finish(c, AX7Z020_STREAM_CANCELLED, ECANCELED);
	else
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
	if (!c->removed) {
		if (c->stream_mode)
			ax7stream_finish(c, AX7Z020_STREAM_CANCELLED, ECANCELED);
		else
			ax7cap_finish(c, AX7Z020_CAPTURE_CANCELLED, ECANCELED);
	}
	mutex_unlock(&c->lock);
}

static const struct of_device_id ax7cap_match[] = {
	{ .compatible = "codex,ax7z020-capture-sg1" }, { }
};
MODULE_DEVICE_TABLE(of, ax7cap_match);
static struct platform_driver ax7cap_driver = {
	.probe = ax7cap_probe, .remove_new = ax7cap_remove, .shutdown = ax7cap_shutdown,
	.driver = { .name = "ax7z020-capture", .of_match_table = ax7cap_match },
};
module_platform_driver(ax7cap_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AX7Z020 finite/STR1 stream SG S2MM capture with owned DMA blocks");

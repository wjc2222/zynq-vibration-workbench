// SPDX-License-Identifier: GPL-2.0-only
/* Fixed AX7Z020 720p fbdev: owns VDMA MM2S, VTC and status GPIO together.
 * There must be no separate xilinx-vdma, vtc or gpio driver for these ranges.
 * Register definitions were checked against AMD embeddedsw 2025.2 sources.
 */
#include <linux/align.h>
#include <linux/build_bug.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fb.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#ifdef MODULE
#error "AX7Z020 v1 fbdev is a built-in fixed-platform driver; loadable modules are unsupported"
#endif

#define WIDTH 1280U
#define HEIGHT 720U
#define STRIDE (WIDTH * 4U)
#define FRAME_BYTES (STRIDE * HEIGHT)
#define BUFFER_BYTES (FRAME_BYTES * 2U)
#define VDMA_CR 0x00
#define VDMA_SR 0x04
#define VDMA_FRMSTORE 0x18
#define VDMA_PARK 0x28
#define VDMA_VSIZE 0x50
#define VDMA_HSIZE 0x54
#define VDMA_STRIDE 0x58
#define VDMA_START0 0x5c
#define VDMA_START1 0x60
#define VDMA_RUN BIT(0)
#define VDMA_RESET BIT(2)
#define VDMA_HALTED BIT(0)
#define VDMA_ERROR 0x00000ff0U
#define VDMA_IRQ_FRAME BIT(12)
#define VDMA_IRQ_ERROR BIT(14)
#define VDMA_IRQ_ALL 0x00007000U
#define VDMA_READ_CURRENT GENMASK(20, 16)
#define GPIO_STATUS 0x00
#define GPIO_CONTROL 0x08
#define VIDEO_HOLD BIT(0)
#define CLEAR_ERRORS BIT(1)
#define STATUS_LOCKED (BIT(0) | BIT(1))
#define STATUS_STICKY (BIT(4) | BIT(5))
#define VTC_CONTROL 0x00
#define VTC_ACTIVE 0x60
#define VTC_ENCODING 0x68
#define VTC_POLARITY 0x6c
#define VTC_HSIZE 0x70
#define VTC_VSIZE 0x74
#define VTC_HSYNC 0x78
#define VTC_VBLANK_H 0x7c
#define VTC_VSYNC 0x80
#define VTC_VSYNC_H 0x84
#define VTC_VBLANK_H_F1 0x88
#define VTC_VSYNC_F1 0x8c
#define VTC_VSYNC_H_F1 0x90
/* Select all generator timing/polarity sources, progressive, register update,
 * generator enable and core enable. No detector or auto-sync is enabled. */
#define VTC_GENERATOR_CONTROL 0x03f7ef07U

struct ax7fb {
	struct device *dev;
	struct fb_info *info;
	void __iomem *vdma, *vtc, *gpio;
	struct clk *axi_clk;
	void *buffer;
	dma_addr_t dma;
	struct mutex lock;
	u32 pseudo_palette[16];
	atomic64_t irq_frames;
	atomic_t dma_failed;
	bool running;
	bool irq_requested;
	int irq;
	bool shutting_down;
};

static int ax7fb_dma_reset(struct ax7fb *fb)
{
	u32 value;
	writel(VDMA_RESET, fb->vdma + VDMA_CR);
	return readl_poll_timeout(fb->vdma + VDMA_CR, value,
			 !(value & VDMA_RESET), 10, 100000);
}

static int ax7fb_stop(struct ax7fb *fb)
{
	u32 value;
	int ret;
	writel(VIDEO_HOLD | CLEAR_ERRORS, fb->gpio + GPIO_CONTROL);
	/* HOLD already resets VTC/VideoOut/PHY. Never access VTC registers here:
	 * PG016 specifies SLVERR while its pixel RESETn is asserted, including
	 * the error path where MMCM unlock asserted reset before we arrived. */
	writel(0, fb->vdma + VDMA_CR);
	ret = readl_poll_timeout(fb->vdma + VDMA_SR, value,
				 value & VDMA_HALTED, 10, 100000);
	/* Reset also clears outstanding VDMA transactions and its IRQ sources. */
	if (ax7fb_dma_reset(fb))
		return -ETIMEDOUT;
	fb->running = false;
	if (ret)
		dev_warn(fb->dev, "VDMA halt timed out; subsequent reset completed\n");
	return 0;
}

static void ax7fb_program_vtc(struct ax7fb *fb)
{
	writel(0, fb->vtc + VTC_CONTROL);
	writel((HEIGHT << 16) | WIDTH, fb->vtc + VTC_ACTIVE);
	writel(0, fb->vtc + VTC_ENCODING);
	writel(0x7f, fb->vtc + VTC_POLARITY);
	writel(1650, fb->vtc + VTC_HSIZE);
	writel((750U << 16) | 750U, fb->vtc + VTC_VSIZE);
	writel((1430U << 16) | 1390U, fb->vtc + VTC_HSYNC);
	/* VTC register convention uses VSyncStart = active+frontporch-1;
	 * horizontal offsets align vertical sync to horizontal sync. */
	writel((1280U << 16) | 1280U, fb->vtc + VTC_VBLANK_H);
	writel((729U << 16) | 724U, fb->vtc + VTC_VSYNC);
	writel((1390U << 16) | 1390U, fb->vtc + VTC_VSYNC_H);
	writel((1280U << 16) | 1280U, fb->vtc + VTC_VBLANK_H_F1);
	writel((729U << 16) | 724U, fb->vtc + VTC_VSYNC_F1);
	writel((1390U << 16) | 1390U, fb->vtc + VTC_VSYNC_H_F1);
	writel(VTC_GENERATOR_CONTROL, fb->vtc + VTC_CONTROL);
}

static irqreturn_t ax7fb_irq(int irq, void *arg)
{
	struct ax7fb *fb = arg;
	u32 status = readl(fb->vdma + VDMA_SR);
	u32 pending = status & VDMA_IRQ_ALL;
	if (!pending)
		return IRQ_NONE;
	writel(pending, fb->vdma + VDMA_SR);
	if (pending & VDMA_IRQ_FRAME)
		atomic64_inc(&fb->irq_frames);
	if ((pending & VDMA_IRQ_ERROR) || (status & VDMA_ERROR)) {
		atomic_set(&fb->dma_failed, 1);
		/* Keep the fault visible; disable its interrupt instead of silently
		 * restarting DMA underneath userspace drawing or page changes. */
		writel(readl(fb->vdma + VDMA_CR) & ~VDMA_IRQ_ERROR,
		       fb->vdma + VDMA_CR);
		dev_err_ratelimited(fb->dev, "VDMA fault, status=%#x\n", status);
	}
	return IRQ_HANDLED;
}

static int ax7fb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	if (var->xres != WIDTH || var->yres != HEIGHT ||
	    var->xres_virtual != WIDTH || var->yres_virtual != HEIGHT * 2 ||
	    var->bits_per_pixel != 32 || var->xoffset ||
	    (var->yoffset != 0 && var->yoffset != HEIGHT) ||
	    (var->vmode & FB_VMODE_YWRAP) || var->rotate || var->grayscale)
		return -EINVAL;
	var->red = (struct fb_bitfield){ .offset = 16, .length = 8 };
	var->green = (struct fb_bitfield){ .offset = 8, .length = 8 };
	var->blue = (struct fb_bitfield){ .offset = 0, .length = 8 };
	var->transp = (struct fb_bitfield){ .offset = 24, .length = 0 };
	var->vmode = FB_VMODE_NONINTERLACED;
	/* The hardware clock/timing are fixed. Do not accept a software-only
	 * timing change and then report a mode the PL is not producing. */
	var->pixclock = 13468;
	var->left_margin = 220; var->right_margin = 110;
	var->upper_margin = 20; var->lower_margin = 5;
	var->hsync_len = 40; var->vsync_len = 5;
	var->sync = FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT;
	var->nonstd = 0;
	return 0;
}

static int ax7fb_setcolreg(unsigned int reg, unsigned int red,
	unsigned int green, unsigned int blue, unsigned int transp,
	struct fb_info *info)
{
	struct ax7fb *fb = info->par;
	if (reg >= ARRAY_SIZE(fb->pseudo_palette))
		return -EINVAL;
	fb->pseudo_palette[reg] = ((red >> 8) << 16) |
				 ((green >> 8) << 8) | (blue >> 8);
	return 0;
}

static int ax7fb_pan(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct ax7fb *fb = info->par;
	u32 park_status, frame;
	int ret;
	if (var->xoffset || (var->yoffset != 0 && var->yoffset != HEIGHT) ||
	    (var->vmode & FB_VMODE_YWRAP))
		return -EINVAL;
	frame = var->yoffset / HEIGHT;
	mutex_lock(&fb->lock);
	if (!fb->running || atomic_read(&fb->dma_failed)) {
		ret = -EIO;
		goto out;
	}
	/* Make WC writes visible to the DMA reader before requesting a page. */
	dma_wmb();
	writel(frame, fb->vdma + VDMA_PARK);
	ret = readl_poll_timeout(fb->vdma + VDMA_PARK, park_status,
				 ((park_status & VDMA_READ_CURRENT) >> 16) == frame,
				 100, 100000);
	if (!ret)
		info->var.yoffset = var->yoffset;
out:
	mutex_unlock(&fb->lock);
	return ret;
}

static int ax7fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct ax7fb *fb = info->par;
	unsigned long bytes = vma->vm_end - vma->vm_start;
	if (READ_ONCE(fb->shutting_down))
		return -ENODEV;
	if (vma->vm_pgoff > BUFFER_BYTES >> PAGE_SHIFT ||
	    bytes > BUFFER_BYTES - (vma->vm_pgoff << PAGE_SHIFT))
		return -EINVAL;
	return dma_mmap_wc(fb->dev, vma, fb->buffer, fb->dma, BUFFER_BYTES);
}

static int ax7fb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
	struct ax7fb *fb = info->par;
	u32 value, before, crtc;
	int ret;
	if (cmd != FBIO_WAITFORVSYNC)
		return -ENOTTY;
	if (copy_from_user(&crtc, (void __user *)arg, sizeof(crtc)))
		return -EFAULT;
	if (crtc)
		return -EINVAL;
	mutex_lock(&fb->lock);
	if (!fb->running || fb->shutting_down || atomic_read(&fb->dma_failed)) {
		ret = -EIO;
		goto out;
	}
	before = readl(fb->gpio + GPIO_STATUS) >> 16;
	ret = readl_poll_timeout(fb->gpio + GPIO_STATUS, value,
				(value >> 16) != before, 100, 100000);
out:
	mutex_unlock(&fb->lock);
	return ret;
}

static const struct fb_ops ax7fb_ops = {
	.owner = THIS_MODULE,
	.fb_read = fb_sys_read, .fb_write = fb_sys_write,
	.fb_check_var = ax7fb_check_var,
	.fb_setcolreg = ax7fb_setcolreg,
	.fb_pan_display = ax7fb_pan,
	.fb_fillrect = sys_fillrect, .fb_copyarea = sys_copyarea,
	.fb_imageblit = sys_imageblit,
	.fb_mmap = ax7fb_mmap, .fb_ioctl = ax7fb_ioctl,
};

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct ax7fb *fb = dev_get_drvdata(dev);
	u32 video = readl(fb->gpio + GPIO_STATUS);
	u32 dma = readl(fb->vdma + VDMA_SR);
	return sysfs_emit(buf,
		"video=%#08x dma=%#08x mmcm_locked=%u video_locked=%u underflow=%u overflow=%u underflow_sticky=%u overflow_sticky=%u vsync_counter=%u dma_irq_frames=%lld dma_failed=%d\n",
		video, dma, !!(video & BIT(0)), !!(video & BIT(1)),
		!!(video & BIT(2)), !!(video & BIT(3)), !!(video & BIT(4)),
		!!(video & BIT(5)), video >> 16,
		(long long)atomic64_read(&fb->irq_frames), atomic_read(&fb->dma_failed));
}
static DEVICE_ATTR_RO(status);

static void ax7fb_initial_pattern(struct ax7fb *fb)
{
	static const u32 colors[] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
				     0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
	u32 *pixels = fb->buffer;
	unsigned int x, y;
	for (y = 0; y < HEIGHT * 2; y++)
		for (x = 0; x < WIDTH; x++)
			pixels[y * WIDTH + x] = colors[x / 160];
	dma_wmb();
}

static int ax7fb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fb_info *info;
	struct ax7fb *fb;
	u32 value;
	int irq, ret, stop_ret;
	/* framebuffer_alloc() only aligns par to sizeof(long), i.e. 4 on ARM.
	 * irq_frames uses ARM doubleword exclusives and requires 8-byte alignment.
	 * Keep the original info allocation/free lifetime, reserving alignment slack.
	 */
	BUILD_BUG_ON(offsetof(struct ax7fb, irq_frames) % 8);
	info = framebuffer_alloc(sizeof(*fb) + __alignof__(*fb) - 1, dev);
	if (!info)
		return -ENOMEM;
	fb = PTR_ALIGN(info->par, __alignof__(*fb));
	info->par = fb;
	if (WARN_ON_ONCE(!IS_ALIGNED((unsigned long)&fb->irq_frames, 8))) {
		ret = -EINVAL;
		goto free_info;
	}
	fb->info = info; fb->dev = dev;
	mutex_init(&fb->lock);
	atomic64_set(&fb->irq_frames, 0); atomic_set(&fb->dma_failed, 0);
	platform_set_drvdata(pdev, fb);
	fb->vdma = devm_platform_ioremap_resource_byname(pdev, "vdma");
	fb->vtc = devm_platform_ioremap_resource_byname(pdev, "vtc");
	fb->gpio = devm_platform_ioremap_resource_byname(pdev, "status");
	if (IS_ERR(fb->vdma) || IS_ERR(fb->vtc) || IS_ERR(fb->gpio)) {
		ret = IS_ERR(fb->vdma) ? PTR_ERR(fb->vdma) :
		      IS_ERR(fb->vtc) ? PTR_ERR(fb->vtc) : PTR_ERR(fb->gpio);
		goto free_info;
	}
	fb->axi_clk = devm_clk_get(dev, "axi");
	if (IS_ERR(fb->axi_clk)) { ret = PTR_ERR(fb->axi_clk); goto free_info; }
	ret = clk_prepare_enable(fb->axi_clk);
	if (ret) goto free_info;
	writel(VIDEO_HOLD | CLEAR_ERRORS, fb->gpio + GPIO_CONTROL);
	ret = readl_poll_timeout(fb->gpio + GPIO_STATUS, value,
				 value & BIT(0), 100, 1000000);
	if (ret) { dev_err(dev, "Display MMCM did not lock\n"); goto disable_clock; }
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) goto disable_clock;
	fb->buffer = dma_alloc_wc(dev, BUFFER_BYTES, &fb->dma, GFP_KERNEL);
	if (!fb->buffer) { ret = -ENOMEM; goto disable_clock; }
	if (upper_32_bits(fb->dma) || upper_32_bits(fb->dma + BUFFER_BYTES - 1)) {
		ret = -ERANGE; goto free_dma;
	}
	ax7fb_initial_pattern(fb);
	ret = ax7fb_dma_reset(fb);
	if (ret) goto free_dma;
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) { ret = irq; goto free_dma; }
	ret = devm_request_irq(dev, irq, ax7fb_irq, 0, dev_name(dev), fb);
	if (ret) goto free_dma;
	fb->irq_requested = true; fb->irq = irq;
	writel(VDMA_IRQ_ALL, fb->vdma + VDMA_SR);
	writel(2, fb->vdma + VDMA_FRMSTORE);
	writel(0, fb->vdma + VDMA_PARK);
	writel(lower_32_bits(fb->dma), fb->vdma + VDMA_START0);
	writel(lower_32_bits(fb->dma + FRAME_BYTES), fb->vdma + VDMA_START1);
	writel(STRIDE, fb->vdma + VDMA_STRIDE);
	writel(STRIDE, fb->vdma + VDMA_HSIZE);
	writel((1U << 16) | VDMA_IRQ_FRAME | VDMA_IRQ_ERROR | VDMA_RUN,
	       fb->vdma + VDMA_CR);
	writel(HEIGHT, fb->vdma + VDMA_VSIZE); /* starts MM2S, written last */
	fb->running = true;
	writel(CLEAR_ERRORS, fb->gpio + GPIO_CONTROL); /* release video resets */
	/* Do not depend on VTC AXI register retention while pixel reset is held. */
	usleep_range(1000, 2000);
	ax7fb_program_vtc(fb);
	if ((readl(fb->vtc + VTC_HSIZE) & 0x3fff) != 1650 ||
	    (readl(fb->vtc + VTC_ACTIVE) & 0x3fff3fff) != ((HEIGHT << 16) | WIDTH)) {
		ret = -EIO; dev_err(dev, "VTC timing register readback mismatch\n"); goto stop;
	}
	ret = readl_poll_timeout(fb->gpio + GPIO_STATUS, value,
				 (value & STATUS_LOCKED) == STATUS_LOCKED,
				 1000, 1000000);
	if (ret) { dev_err(dev, "VideoOut did not lock: %#x\n", value); goto stop; }
	writel(0, fb->gpio + GPIO_CONTROL);
	msleep(40);
	value = readl(fb->gpio + GPIO_STATUS);
	if ((value & STATUS_LOCKED) != STATUS_LOCKED ||
	    (value & STATUS_STICKY) || atomic_read(&fb->dma_failed)) {
		dev_err(dev, "Video pipeline lost lock or reports FIFO/DMA error: %#x\n", value);
		ret = -EIO; goto stop;
	}
	strscpy(info->fix.id, "AX7Z020-720p", sizeof(info->fix.id));
	info->fix.smem_start = fb->dma; info->fix.smem_len = BUFFER_BYTES;
	info->fix.type = FB_TYPE_PACKED_PIXELS; info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.line_length = STRIDE; info->fix.ypanstep = HEIGHT;
	info->fix.accel = FB_ACCEL_NONE;
	info->var = (struct fb_var_screeninfo){ .xres = WIDTH, .yres = HEIGHT,
		.xres_virtual = WIDTH, .yres_virtual = HEIGHT * 2, .bits_per_pixel = 32,
		.pixclock = 13468, .left_margin = 220, .right_margin = 110,
		.upper_margin = 20, .lower_margin = 5, .hsync_len = 40, .vsync_len = 5,
		.sync = FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.width = -1, .height = -1, .activate = FB_ACTIVATE_NOW };
	ax7fb_check_var(&info->var, info);
	info->fbops = &ax7fb_ops; info->pseudo_palette = fb->pseudo_palette;
	info->screen_buffer = fb->buffer; info->screen_size = BUFFER_BYTES;
	info->flags = FBINFO_VIRTFB; /* DMA-backed system RAM; software fb helpers */
	ret = fb_alloc_cmap(&info->cmap, 16, 0);
	if (ret) goto stop;
	ret = device_create_file(dev, &dev_attr_status);
	if (ret) goto free_cmap;
	ret = register_framebuffer(info);
	if (ret) { device_remove_file(dev, &dev_attr_status); goto free_cmap; }
	dev_info(dev, "fb%d: 1280x720 XRGB8888, two pages, DMA=%pad; internal VideoOut locked (monitor unverified)\n",
		 info->node, &fb->dma);
	return 0;
free_cmap:
	fb_dealloc_cmap(&info->cmap);
stop:
	stop_ret = ax7fb_stop(fb);
	if (fb->irq_requested) {
		devm_free_irq(dev, fb->irq, fb);
		fb->irq_requested = false;
	}
	if (stop_ret) {
		/* A read master may still have requests outstanding. Retain its
		 * backing pages and clock rather than releasing live DMA memory. */
		dev_crit(dev, "VDMA reset failed; DMA allocation and clock retained, reboot required\n");
		goto free_info;
	}
free_dma:
	dma_free_wc(dev, BUFFER_BYTES, fb->buffer, fb->dma);
disable_clock:
	clk_disable_unprepare(fb->axi_clk);
free_info:
	framebuffer_release(info);
	return ret;
}

static void ax7fb_shutdown(struct platform_device *pdev)
{
	struct ax7fb *fb = platform_get_drvdata(pdev);
	int ret;
	mutex_lock(&fb->lock);
	WRITE_ONCE(fb->shutting_down, true);
	ret = ax7fb_stop(fb);
	fb->running = false;
	mutex_unlock(&fb->lock);
	disable_irq(fb->irq);
	if (ret)
		dev_crit(&pdev->dev, "VDMA reset failed during shutdown\n");
	/* This fixed built-in v1 has no runtime remove/unregister path. Keep
	 * buffers, fb_info and clocks alive for any remaining mappings until
	 * the system actually resets. Runtime overlays/PL reload are disabled. */
}

static const struct of_device_id ax7fb_match[] = {
	{ .compatible = "codex,ax7z020-vdma-fb" }, { }
};
MODULE_DEVICE_TABLE(of, ax7fb_match);
static struct platform_driver ax7fb_driver = {
	.probe = ax7fb_probe, .shutdown = ax7fb_shutdown,
	.driver = { .name = "codex-ax7z020-fb", .of_match_table = ax7fb_match,
		/* Fixed FPGA platform: do not expose runtime unbind while fb files
		 * or mmap VMAs may still be held by userspace. */
		.suppress_bind_attrs = true },
};
builtin_platform_driver(ax7fb_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AX7Z020 fixed 720p VDMA/VTC fbdev");
MODULE_AUTHOR("Codex project");

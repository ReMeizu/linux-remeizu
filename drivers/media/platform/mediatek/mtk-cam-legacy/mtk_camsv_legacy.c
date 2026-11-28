// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek legacy (MT6735/MT6755/MT6797) CamSV capture engines.
 *
 * Copyright (C) 2026 Meizu fleet kernel port
 *
 * The CamSV (camera video front-end) block contains up to three pairs of
 * small capture engines.  An engine receives the CSI-2 stream of one SENINF
 * RX port, packs the Bayer samples exactly as the sensor transmits them
 * (RAW8/RAW10/RAW12) and writes the frame through the IMGO DMA port into a
 * contiguous memory buffer.  That is what the vendor kernels expose through
 * the kd_camera camera interface and what this driver exposes as a V4L2
 * video node:
 *
 *   /dev/videoN  <- one CamSV engine (bounded by the media controller graph)
 *
 * Frame start is taken from the timing generator running off the SENINF
 * interface; a frame is complete when the engine signals its Done status.
 */

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "mtk-cam-legacy.h"

static inline struct mtk_camsv *to_camsv(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mtk_camsv, sd);
}

#define vb2_to_mtk_camsv_buf(vb2_buf)	\
	container_of(vb2_buf, struct mtk_camsv_buffer, vb.vb2_buf)

/* --------------------------------------------------------------------------
 * Pixel formats
 * ------------------------------------------------------------------------ */

struct mtk_camsv_format {
	u32 fourcc;
	u32 mbus_code;
	u32 bpp;		/* bytes per 8 pixels, padded to 32 bits */
	u32 pixel_mode;
};

/*
 * MediaTek packs the Bayer data byte-wise the same way the sensor sends it:
 * RAW8 occupies one byte, RAW10 4 bytes per 4 pixels and RAW12 3 bytes per
 * 2 pixels.  For the DMA stride we always program the padded bus width the
 * vendor code uses (RAW10 -> 8 bytes per 6 pixels is not used here, the SMI
 * pads each line to 32 bits).
 */
#define CAMSV_FMT_RAW8(fcc, code)	{ (fcc), (code), 8, CAMSV_CTRL_CFG_RAW8 }
#define CAMSV_FMT_RAW10(fcc, code)	{ (fcc), (code), 10, CAMSV_CTRL_CFG_RAW10 }
#define CAMSV_FMT_RAW12(fcc, code)	{ (fcc), (code), 12, CAMSV_CTRL_CFG_RAW12 }

static const struct mtk_camsv_format mtk_camsv_formats[] = {
	CAMSV_FMT_RAW8(V4L2_PIX_FMT_SBGGR8, MEDIA_BUS_FMT_SBGGR8_1X8),
	CAMSV_FMT_RAW8(V4L2_PIX_FMT_SGBRG8, MEDIA_BUS_FMT_SGBRG8_1X8),
	CAMSV_FMT_RAW8(V4L2_PIX_FMT_SGRBG8, MEDIA_BUS_FMT_SGRBG8_1X8),
	CAMSV_FMT_RAW8(V4L2_PIX_FMT_SRGGB8, MEDIA_BUS_FMT_SRGGB8_1X8),
	CAMSV_FMT_RAW10(V4L2_PIX_FMT_SBGGR10, MEDIA_BUS_FMT_SBGGR10_1X10),
	CAMSV_FMT_RAW10(V4L2_PIX_FMT_SGBRG10, MEDIA_BUS_FMT_SGBRG10_1X10),
	CAMSV_FMT_RAW10(V4L2_PIX_FMT_SGRBG10, MEDIA_BUS_FMT_SGRBG10_1X10),
	CAMSV_FMT_RAW10(V4L2_PIX_FMT_SRGGB10, MEDIA_BUS_FMT_SRGGB10_1X10),
	CAMSV_FMT_RAW12(V4L2_PIX_FMT_SBGGR12, MEDIA_BUS_FMT_SBGGR12_1X12),
	CAMSV_FMT_RAW12(V4L2_PIX_FMT_SGBRG12, MEDIA_BUS_FMT_SGBRG12_1X12),
	CAMSV_FMT_RAW12(V4L2_PIX_FMT_SGRBG12, MEDIA_BUS_FMT_SGRBG12_1X12),
	CAMSV_FMT_RAW12(V4L2_PIX_FMT_SRGGB12, MEDIA_BUS_FMT_SRGGB12_1X12),
};

#define mtk_camsv_num_formats	ARRAY_SIZE(mtk_camsv_formats)

static const struct mtk_camsv_format *
mtk_camsv_find_format(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < mtk_camsv_num_formats; i++)
		if (mtk_camsv_formats[i].fourcc == fourcc)
			return &mtk_camsv_formats[i];

	return NULL;
}

static const struct mtk_camsv_format *
mtk_camsv_find_mbus_format(u32 mbus_code)
{
	unsigned int i;

	for (i = 0; i < mtk_camsv_num_formats; i++)
		if (mtk_camsv_formats[i].mbus_code == mbus_code)
			return &mtk_camsv_formats[i];

	return NULL;
}

static unsigned int mtk_camsv_bpl(const struct mtk_camsv_format *fmt,
				  unsigned int width)
{
	/* One line of packed Bayer data, rounded up to a 32 bit boundary */
	return ALIGN(DIV_ROUND_UP(width * fmt->bpp, 8), 4);
}

static unsigned int mtk_camsv_sizeimage(const struct mtk_camsv_format *fmt,
					unsigned int width, unsigned int height)
{
	return PAGE_ALIGN(mtk_camsv_bpl(fmt, width) * height);
}

/* --------------------------------------------------------------------------
 * Hardware programming
 * ------------------------------------------------------------------------ */

static void mtk_camsv_hw_reset(struct mtk_camsv *camsv)
{
	writel(CAMSV_CTRL_CFG_SW_RST, camsv->base + CAMSV_CTRL_CFG);
	udelay(1);
	writel(0, camsv->base + CAMSV_CTRL_CFG);
}

static void mtk_camsv_hw_setup(struct mtk_camsv *camsv,
			       const struct mtk_camsv_format *fmt,
			       dma_addr_t addr)
{
	unsigned int bpl = mtk_camsv_bpl(fmt, camsv->width);
	u32 port = camsv->port_index;

	/* Stop everything, then program the pipeline from scratch */
	writel(CAMSV_CTRL_CFG_SW_RST, camsv->base + CAMSV_CTRL_CFG);
	writel(0, camsv->base + CAMSV_DMA_EN);
	writel(CAMSV_DMA_RESET, camsv->base + CAMSV_DMA_RESET);
	udelay(1);
	writel(0, camsv->base + CAMSV_DMA_RESET);

	/* IMGO: one plane, line stride in bytes, frame height in lines */
	writel(addr, camsv->base + CAMSV_IMGO_BASE_ADDR);
	writel(bpl, camsv->base + CAMSV_IMGO_XSIZE);
	writel(camsv->height, camsv->base + CAMSV_IMGO_YSIZE);
	writel(bpl, camsv->base + CAMSV_IMGO_STRIDE);

	/* FBC stays disabled, the consumer gets plain packed Bayer data */
	writel(0, camsv->base + CAMSV_IMGO_FBC);

	/* Timing generator follows the SENINF port of this engine */
	writel(port & CAMSV_TG_SENINF_PORT, camsv->base + CAMSV_TG_SENINF_CON);
	writel(CAMSV_TG_MODE_VSYNC, camsv->base + CAMSV_TG_MODE);
	writel(camsv->width, camsv->base + CAMSV_TG_HCNT);
	writel(camsv->height, camsv->base + CAMSV_TG_VCNT);

	writel(FIELD_PREP(CAMSV_CTRL_CFG_PIXEL_MODE, fmt->pixel_mode),
	       camsv->base + CAMSV_CTRL_CFG);

	/* Clear and arm the interrupt sources we care about */
	writel(0xffffffff, camsv->base + CAMSV_INT_STATUS);
	writel(0xffffffff, camsv->base + CAMSV_IPU_INT_STATUS);
	writel(CAMSV_INT_DONE, camsv->base + CAMSV_INT_EN);
	writel(CAMSV_IPU_INT_IMGO_NUM, camsv->base + CAMSV_IPU_INT_EN);

	writel(CAMSV_TOP_CFG_IMGO_EN, camsv->base + CAMSV_TOP_CFG);
	writel(CAMSV_MODULE_EN_TG | CAMSV_MODULE_EN_IMGO |
	       CAMSV_MODULE_EN_SENINF, camsv->base + CAMSV_MODULE_EN);
}

static void mtk_camsv_hw_start(struct mtk_camsv *camsv)
{
	u32 val;

	writel(CAMSV_DMA_EN_IMGO, camsv->base + CAMSV_DMA_EN);

	val = readl(camsv->base + CAMSV_TG_CTRL);
	writel(val | CAMSV_TG_CTRL_EN, camsv->base + CAMSV_TG_CTRL);

	val = readl(camsv->base + CAMSV_CTRL_CFG);
	writel(val | CAMSV_CTRL_CFG_STREAM, camsv->base + CAMSV_CTRL_CFG);
}

static void mtk_camsv_hw_stop(struct mtk_camsv *camsv)
{
	u32 val;

	val = readl(camsv->base + CAMSV_CTRL_CFG);
	writel(val & ~CAMSV_CTRL_CFG_STREAM, camsv->base + CAMSV_CTRL_CFG);

	val = readl(camsv->base + CAMSV_TG_CTRL);
	writel(val & ~CAMSV_TG_CTRL_EN, camsv->base + CAMSV_TG_CTRL);

	writel(0, camsv->base + CAMSV_DMA_EN);
	writel(0, camsv->base + CAMSV_INT_EN);
	writel(0, camsv->base + CAMSV_IPU_INT_EN);
	writel(0xffffffff, camsv->base + CAMSV_INT_STATUS);
	writel(0xffffffff, camsv->base + CAMSV_IPU_INT_STATUS);
}

/* Pick up the next queued buffer and hand it to the DMA engine */
static void mtk_camsv_start_next(struct mtk_camsv *camsv)
{
	struct mtk_camsv_buffer *buf;
	unsigned long flags;
	const struct mtk_camsv_format *fmt;

	spin_lock_irqsave(&camsv->slock, flags);

	if (!camsv->streaming)
		goto out;

	if (list_empty(&camsv->buffers)) {
		camsv->cur_buf = NULL;
		goto out;
	}

	buf = list_first_entry(&camsv->buffers, struct mtk_camsv_buffer, list);
	list_del_init(&buf->list);
	camsv->cur_buf = buf;
	spin_unlock_irqrestore(&camsv->slock, flags);

	fmt = mtk_camsv_find_mbus_format(camsv->mbus_code);
	if (!fmt)
		return;

	/*
	 * Re-arming the base address is enough for a contiguous buffer, the
	 * engine restarts on the next frame start.
	 */
	writel(vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0),
	       camsv->base + CAMSV_IMGO_BASE_ADDR);

	return;

out:
	spin_unlock_irqrestore(&camsv->slock, flags);
}

/* --------------------------------------------------------------------------
 * Interrupt handling
 * ------------------------------------------------------------------------ */

static irqreturn_t mtk_camsv_isr(int irq, void *data)
{
	struct mtk_camsv *camsv = data;
	struct mtk_camsv_buffer *buf;
	struct vb2_v4l2_buffer *vbuf;
	unsigned long flags;
	u32 status, ipu_status;

	status = readl(camsv->base + CAMSV_INT_STATUS);
	ipu_status = readl(camsv->base + CAMSV_IPU_INT_STATUS);

	if (!status && !ipu_status)
		return IRQ_NONE;

	writel(status, camsv->base + CAMSV_INT_STATUS);
	writel(ipu_status, camsv->base + CAMSV_IPU_INT_STATUS);

	if (status & CAMSV_INT_ERR)
		camsv->overruns++;

	if (!(status & CAMSV_INT_DONE))
		return IRQ_HANDLED;

	spin_lock_irqsave(&camsv->slock, flags);
	buf = camsv->cur_buf;
	camsv->cur_buf = NULL;
	spin_unlock_irqrestore(&camsv->slock, flags);

	if (!buf) {
		camsv->errors++;
		return IRQ_HANDLED;
	}

	vbuf = &buf->vb;
	vbuf->vb2_buf.timestamp = ktime_get_ns();
	vbuf->sequence = camsv->sequence++;
	vbuf->field = V4L2_FIELD_NONE;
	vb2_buffer_done(&vbuf->vb2_buf, VB2_BUF_STATE_DONE);

	mtk_camsv_start_next(camsv);

	return IRQ_HANDLED;
}

/* --------------------------------------------------------------------------
 * vb2 queue operations
 * ------------------------------------------------------------------------ */

static int mtk_camsv_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
				 unsigned int *num_planes, unsigned int sizes[],
				 struct device *alloc_devs[])
{
	struct mtk_camsv *camsv = q->drv_priv;
	const struct mtk_camsv_format *fmt;
	unsigned int size;

	fmt = mtk_camsv_find_mbus_format(camsv->mbus_code);
	if (!fmt)
		return -EINVAL;

	size = mtk_camsv_sizeimage(fmt, camsv->width, camsv->height);

	if (*num_planes) {
		if (*num_planes != 1 || sizes[0] < size)
			return -EINVAL;
		return 0;
	}

	*num_planes = 1;
	sizes[0] = size;

	return 0;
}

static int mtk_camsv_buf_prepare(struct vb2_buffer *vb)
{
	struct mtk_camsv *camsv = vb->vb2_queue->drv_priv;
	const struct mtk_camsv_format *fmt;
	unsigned int size;

	fmt = mtk_camsv_find_mbus_format(camsv->mbus_code);
	if (!fmt)
		return -EINVAL;

	size = mtk_camsv_sizeimage(fmt, camsv->width, camsv->height);

	if (vb2_plane_size(vb, 0) < size) {
		dev_err(camsv->cam->dev,
			"buffer too small (%lu < %u)\n",
			vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, size);

	return 0;
}

static void mtk_camsv_buf_queue(struct vb2_buffer *vb)
{
	struct mtk_camsv *camsv = vb->vb2_queue->drv_priv;
	struct mtk_camsv_buffer *buf = vb2_to_mtk_camsv_buf(vb);
	unsigned long flags;

	spin_lock_irqsave(&camsv->slock, flags);
	list_add_tail(&buf->list, &camsv->buffers);
	spin_unlock_irqrestore(&camsv->slock, flags);
}

static int mtk_camsv_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct mtk_camsv *camsv = q->drv_priv;
	const struct mtk_camsv_format *fmt;
	struct mtk_camsv_buffer *buf;
	dma_addr_t addr;
	unsigned long flags;
	int ret;

	if (!camsv->port)
		return -ENODEV;

	fmt = mtk_camsv_find_mbus_format(camsv->mbus_code);
	if (!fmt)
		return -EINVAL;

	/* First buffer arms the DMA, the rest are handed over by the ISR */
	spin_lock_irqsave(&camsv->slock, flags);
	if (list_empty(&camsv->buffers)) {
		spin_unlock_irqrestore(&camsv->slock, flags);
		return -ENODATA;
	}
	buf = list_first_entry(&camsv->buffers, struct mtk_camsv_buffer, list);
	list_del_init(&buf->list);
	camsv->cur_buf = buf;
	spin_unlock_irqrestore(&camsv->slock, flags);

	addr = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);

	camsv->sequence = 0;
	camsv->overruns = 0;
	camsv->errors = 0;

	mtk_camsv_hw_setup(camsv, fmt, addr);
	mtk_camsv_hw_start(camsv);
	camsv->streaming = true;

	ret = v4l2_subdev_call(camsv->port, video, s_stream, 1);
	if (ret) {
		mtk_camsv_hw_stop(camsv);
		camsv->streaming = false;
		goto err_buffers;
	}

	dev_dbg(camsv->cam->dev, "camsv%u: streaming %ux%u fourcc %08x\n",
		camsv->index, camsv->width, camsv->height, fmt->fourcc);

	return 0;

err_buffers:
	spin_lock_irqsave(&camsv->slock, flags);
	if (camsv->cur_buf) {
		list_add_tail(&camsv->cur_buf->list, &camsv->buffers);
		camsv->cur_buf = NULL;
	}
	spin_unlock_irqrestore(&camsv->slock, flags);
	return ret;
}

static void mtk_camsv_stop_streaming(struct vb2_queue *q)
{
	struct mtk_camsv *camsv = q->drv_priv;
	struct mtk_camsv_buffer *buf, *tmp;
	unsigned long flags;

	if (camsv->streaming) {
		v4l2_subdev_call(camsv->port, video, s_stream, 0);
		mtk_camsv_hw_stop(camsv);
		camsv->streaming = false;
	}

	spin_lock_irqsave(&camsv->slock, flags);
	if (camsv->cur_buf) {
		vb2_buffer_done(&camsv->cur_buf->vb.vb2_buf,
				VB2_BUF_STATE_ERROR);
		camsv->cur_buf = NULL;
	}
	list_for_each_entry_safe(buf, tmp, &camsv->buffers, list) {
		list_del_init(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&camsv->slock, flags);

	if (camsv->overruns || camsv->errors)
		dev_warn(camsv->cam->dev,
			 "camsv%u: %u overruns, %u lost frames\n",
			 camsv->index, camsv->overruns, camsv->errors);
}

static const struct vb2_ops mtk_camsv_vb2_ops = {
	.queue_setup = mtk_camsv_queue_setup,
	.buf_prepare = mtk_camsv_buf_prepare,
	.buf_queue = mtk_camsv_buf_queue,
	.start_streaming = mtk_camsv_start_streaming,
	.stop_streaming = mtk_camsv_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

/* --------------------------------------------------------------------------
 * Format propagation
 * ------------------------------------------------------------------------ */

static int mtk_camsv_init_state(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state)
{
	struct mtk_camsv *camsv = to_camsv(sd);
	struct v4l2_mbus_framefmt *fmt;
	unsigned int i;

	for (i = 0; i < 2; i++) {
		fmt = v4l2_subdev_state_get_format(state, i);
		fmt->code = camsv->mbus_code;
		fmt->width = camsv->width;
		fmt->height = camsv->height;
		fmt->field = V4L2_FIELD_NONE;
		fmt->colorspace = V4L2_COLORSPACE_RAW;
		fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
		fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
		fmt->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	}

	return 0;
}

static int mtk_camsv_get_fmt(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *fmt)
{
	fmt->format = *v4l2_subdev_state_get_format(state, fmt->pad);

	return 0;
}

static int mtk_camsv_set_fmt(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *fmt)
{
	struct mtk_camsv *camsv = to_camsv(sd);
	const struct mtk_camsv_format *format;
	struct v4l2_mbus_framefmt *mf;
	unsigned int i;

	format = mtk_camsv_find_mbus_format(fmt->format.code);
	if (!format)
		format = mtk_camsv_find_mbus_format(camsv->mbus_code);
	if (!format)
		return -EINVAL;

	fmt->format.code = format->mbus_code;
	fmt->format.width = clamp_val(fmt->format.width, CAMSV_MIN_WIDTH,
				      CAMSV_MAX_WIDTH);
	fmt->format.height = clamp_val(fmt->format.height, CAMSV_MIN_HEIGHT,
				       CAMSV_MAX_HEIGHT);

	/* Both pads carry the same stream: the engine has no scaler */
	for (i = 0; i < 2; i++) {
		mf = v4l2_subdev_state_get_format(state, i);
		*mf = fmt->format;
	}

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		camsv->mbus_code = format->mbus_code;
		camsv->width = fmt->format.width;
		camsv->height = fmt->format.height;
	}

	return 0;
}

static int mtk_camsv_enum_mbus_code(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= mtk_camsv_num_formats)
		return -EINVAL;

	code->code = mtk_camsv_formats[code->index].mbus_code;

	return 0;
}

static int mtk_camsv_enum_frame_size(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index)
		return -EINVAL;

	fse->min_width = CAMSV_MIN_WIDTH;
	fse->max_width = CAMSV_MAX_WIDTH;
	fse->min_height = CAMSV_MIN_HEIGHT;
	fse->max_height = CAMSV_MAX_HEIGHT;

	return 0;
}

/* Region of interest selection, mirrors the format */
static int mtk_camsv_get_selection(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_selection *sel)
{
	struct mtk_camsv *camsv = to_camsv(sd);

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = camsv->width;
		sel->r.height = camsv->height;
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_subdev_pad_ops mtk_camsv_pad_ops = {
	.enum_mbus_code = mtk_camsv_enum_mbus_code,
	.enum_frame_size = mtk_camsv_enum_frame_size,
	.get_fmt = mtk_camsv_get_fmt,
	.set_fmt = mtk_camsv_set_fmt,
	.get_selection = mtk_camsv_get_selection,
};

static const struct v4l2_subdev_ops mtk_camsv_ops = {
	.pad = &mtk_camsv_pad_ops,
};

static const struct v4l2_subdev_internal_ops mtk_camsv_internal_ops = {
	.init_state = mtk_camsv_init_state,
};

/* --------------------------------------------------------------------------
 * V4L2 video node
 * ------------------------------------------------------------------------ */

static int mtk_camsv_querycap(struct file *file, void *priv,
			      struct v4l2_capability *cap)
{
	struct mtk_camsv *camsv = video_drvdata(file);

	strscpy(cap->driver, MTK_CAM_DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "MTK CamSV", sizeof(cap->card));
	strscpy(cap->bus_info, dev_name(camsv->cam->dev),
		sizeof(cap->bus_info));

	return 0;
}

static int mtk_camsv_enum_fmt_vid_cap(struct file *file, void *priv,
				      struct v4l2_fmtdesc *f)
{
	if (f->index >= mtk_camsv_num_formats)
		return -EINVAL;

	f->pixelformat = mtk_camsv_formats[f->index].fourcc;
	f->flags = 0;

	return 0;
}

static int mtk_camsv_g_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct mtk_camsv *camsv = video_drvdata(file);
	const struct mtk_camsv_format *fmt;

	fmt = mtk_camsv_find_mbus_format(camsv->mbus_code);
	if (!fmt)
		return -EINVAL;

	f->fmt.pix.width = camsv->width;
	f->fmt.pix.height = camsv->height;
	f->fmt.pix.pixelformat = fmt->fourcc;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = mtk_camsv_bpl(fmt, camsv->width);
	f->fmt.pix.sizeimage = mtk_camsv_sizeimage(fmt, camsv->width,
						   camsv->height);
	f->fmt.pix.colorspace = V4L2_COLORSPACE_RAW;
	f->fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	f->fmt.pix.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	f->fmt.pix.xfer_func = V4L2_XFER_FUNC_DEFAULT;

	return 0;
}

static int mtk_camsv_s_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct mtk_camsv *camsv = video_drvdata(file);
	const struct mtk_camsv_format *fmt;
	struct v4l2_subdev_format sd_fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	struct media_pad *pad;
	int ret;

	if (vb2_is_busy(&camsv->queue))
		return -EBUSY;

	fmt = mtk_camsv_find_format(f->fmt.pix.pixelformat);
	if (!fmt)
		return -EINVAL;

	if (!camsv->port)
		return -ENODEV;

	sd_fmt.format.code = fmt->mbus_code;
	sd_fmt.format.width = clamp_val(f->fmt.pix.width, CAMSV_MIN_WIDTH,
					CAMSV_MAX_WIDTH);
	sd_fmt.format.height = clamp_val(f->fmt.pix.height, CAMSV_MIN_HEIGHT,
					 CAMSV_MAX_HEIGHT);
	sd_fmt.format.field = V4L2_FIELD_NONE;

	/* Walk up the graph: the engine requests, the sensor decides */
	pad = media_pad_remote_pad_first(&camsv->pads[0]);
	if (pad && pad->entity) {
		struct v4l2_subdev *upstream =
			media_entity_to_v4l2_subdev(pad->entity);

		sd_fmt.pad = pad->index;
		ret = v4l2_subdev_call(upstream, pad, set_fmt, NULL, &sd_fmt);
		if (ret && ret != -ENOIOCTLCMD)
			return ret;
	}

	mutex_lock(&camsv->lock);
	camsv->mbus_code = fmt->mbus_code;
	camsv->width = sd_fmt.format.width;
	camsv->height = sd_fmt.format.height;
	mutex_unlock(&camsv->lock);

	return mtk_camsv_g_fmt_vid_cap(file, priv, f);
}

static int mtk_camsv_try_fmt_vid_cap(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	const struct mtk_camsv_format *fmt;

	fmt = mtk_camsv_find_format(f->fmt.pix.pixelformat);
	if (!fmt)
		fmt = &mtk_camsv_formats[4];	/* SBGGR10 */

	f->fmt.pix.pixelformat = fmt->fourcc;
	f->fmt.pix.width = clamp_val(f->fmt.pix.width, CAMSV_MIN_WIDTH,
				     CAMSV_MAX_WIDTH);
	f->fmt.pix.height = clamp_val(f->fmt.pix.height, CAMSV_MIN_HEIGHT,
				      CAMSV_MAX_HEIGHT);
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = mtk_camsv_bpl(fmt, f->fmt.pix.width);
	f->fmt.pix.sizeimage = mtk_camsv_sizeimage(fmt, f->fmt.pix.width,
						   f->fmt.pix.height);
	f->fmt.pix.colorspace = V4L2_COLORSPACE_RAW;

	return 0;
}

static const struct v4l2_ioctl_ops mtk_camsv_ioctl_ops = {
	.vidioc_querycap = mtk_camsv_querycap,
	.vidioc_enum_fmt_vid_cap = mtk_camsv_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = mtk_camsv_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = mtk_camsv_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = mtk_camsv_try_fmt_vid_cap,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int mtk_camsv_open(struct file *file)
{
	return v4l2_fh_open(file);
}

static int mtk_camsv_close(struct file *file)
{
	struct mtk_camsv *camsv = video_drvdata(file);

	mutex_lock(&camsv->lock);
	vb2_queue_release(&camsv->queue);
	mutex_unlock(&camsv->lock);

	return v4l2_fh_release(file);
}

static const struct v4l2_file_operations mtk_camsv_fops = {
	.owner = THIS_MODULE,
	.open = mtk_camsv_open,
	.release = mtk_camsv_close,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
	.poll = vb2_fop_poll,
	.read = vb2_fop_read,
};

/* --------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------ */

static int mtk_camsv_link_port(struct mtk_camsv *camsv)
{
	struct media_pad *pad;
	int ret;

	pad = media_pad_remote_pad_first(&camsv->pads[0]);
	if (!pad || !pad->entity)
		return -ENOLINK;

	if (pad->entity->function != MEDIA_ENT_F_VID_IF_BRIDGE)
		return -ENOLINK;

	ret = media_create_pad_link(pad->entity, pad->index, &camsv->sd.entity,
				    0, MEDIA_LNK_FL_ENABLED |
				       MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		return ret;

	camsv->port = media_entity_to_v4l2_subdev(pad->entity);
	if (of_property_read_u32(pad->entity->of_node, "reg",
				 &camsv->port_index))
		camsv->port_index = 0;

	return 0;
}

/**
 * mtk_camsv_register - create the video device and sub-device of one engine
 * @cam: camera device
 * @node: device tree node of the CamSV engine
 *
 *	 camsv0: video@1a050000 {
 *		compatible = "mediatek,mt6797-camsv";
 *		reg = <0x1a050000 0x1000>;
 *		interrupts = ...;
 *		clocks = <&cam...>;
 *		port {
 *			camsv0_in: endpoint { remote-endpoint = <&seninf0_out>; };
 *		};
 *	};
 */
int mtk_camsv_register(struct mtk_cam *cam, struct device_node *node)
{
	struct platform_device *pdev;
	struct mtk_camsv *camsv;
	struct resource *res;
	int ret;

	if (cam->num_engines >= MTK_CAM_MAX_ENGINES)
		return -ENOSPC;

	pdev = of_find_device_by_node(node);
	if (!pdev)
		return -EPROBE_DEFER;

	camsv = devm_kzalloc(&pdev->dev, sizeof(*camsv), GFP_KERNEL);
	if (!camsv) {
		put_device(&pdev->dev);
		return -ENOMEM;
	}

	camsv->cam = cam;
	camsv->index = cam->num_engines;

	camsv->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(camsv->base)) {
		ret = PTR_ERR(camsv->base);
		goto err_put;
	}

	camsv->irq = platform_get_irq(pdev, 0);
	if (camsv->irq < 0) {
		ret = camsv->irq;
		goto err_put;
	}

	mutex_init(&camsv->lock);
	spin_lock_init(&camsv->slock);
	INIT_LIST_HEAD(&camsv->buffers);

	camsv->mbus_code = MEDIA_BUS_FMT_SBGGR10_1X10;
	camsv->width = 1600;
	camsv->height = 1200;

	/* Sub-device: sink pad from the CSI receiver, source pad to userspace */
	v4l2_subdev_init(&camsv->sd, &mtk_camsv_ops);
	camsv->sd.internal_ops = &mtk_camsv_internal_ops;
	camsv->sd.owner = THIS_MODULE;
	camsv->sd.dev = &pdev->dev;
	camsv->sd.fwnode = of_fwnode_handle(node);
	camsv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	camsv->sd.entity.function = MEDIA_ENT_F_IO_V4L;
	snprintf(camsv->sd.name, sizeof(camsv->sd.name), "camsv%u",
		 camsv->index);

	camsv->pads[0].flags = MEDIA_PAD_FL_SINK;
	camsv->pads[1].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&camsv->sd.entity, 2, camsv->pads);
	if (ret)
		goto err_put;

	ret = v4l2_subdev_init_finalize(&camsv->sd);
	if (ret)
		goto err_entity;

	/* Video node */
	strscpy(camsv->vdev.name, "mtk-camsv", sizeof(camsv->vdev.name));
	camsv->vdev.v4l2_dev = &cam->vdev;
	camsv->vdev.fops = &mtk_camsv_fops;
	camsv->vdev.ioctl_ops = &mtk_camsv_ioctl_ops;
	camsv->vdev.release = video_device_release_empty;
	camsv->vdev.lock = &camsv->lock;
	camsv->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE |
				  V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
	camsv->vdev.entity.function = MEDIA_ENT_F_IO_V4L;
	video_set_drvdata(&camsv->vdev, camsv);

	camsv->queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	camsv->queue.io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ;
	camsv->queue.dev = &pdev->dev;
	camsv->queue.ops = &mtk_camsv_vb2_ops;
	camsv->queue.mem_ops = &vb2_dma_contig_memops;
	camsv->queue.drv_priv = camsv;
	camsv->queue.buf_struct_size = sizeof(struct mtk_camsv_buffer);
	camsv->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	camsv->queue.min_queued_buffers = 1;

	ret = vb2_queue_init(&camsv->queue);
	if (ret)
		goto err_finalize;

	camsv->vdev.queue = &camsv->queue;
	camsv->vdev.vfl_dir = VFL_DIR_RX;

	ret = video_register_device(&camsv->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&pdev->dev, "failed to register video device: %d\n",
			ret);
		goto err_queue;
	}

	ret = request_irq(camsv->irq, mtk_camsv_isr, IRQF_SHARED,
			  dev_name(&pdev->dev), camsv);
	if (ret) {
		dev_err(&pdev->dev, "failed to request IRQ %d: %d\n",
			camsv->irq, ret);
		goto err_video;
	}

	/*
	 * The video node has no separate media entity: it is the sub-device's
	 * source pad that userspace opens.  Only the sink-side link to the
	 * CSI receiver needs to be created once both halves exist.
	 */
	/* The receiver end is resolved once both sub-devices are registered */
	ret = mtk_camsv_link_port(camsv);

	cam->engines[cam->num_engines++] = camsv;

	dev_info(&pdev->dev, "camsv%u: %s, IRQ %d\n", camsv->index,
		 video_device_node_name(&camsv->vdev), camsv->irq);

	if (ret && ret != -ENOLINK)
		dev_warn(&pdev->dev, "no CSI receiver linked yet\n");

	return 0;

err_irq:
	free_irq(camsv->irq, camsv);
err_video:
	video_unregister_device(&camsv->vdev);
err_queue:
	vb2_queue_release(&camsv->queue);
err_finalize:
	v4l2_subdev_cleanup(&camsv->sd);
err_entity:
	media_entity_cleanup(&camsv->sd.entity);
err_put:
	put_device(&pdev->dev);
	return ret;
}

int mtk_camsv_link_all(struct mtk_cam *cam)
{
	unsigned int i;
	int ret, err = 0;

	for (i = 0; i < cam->num_engines; i++) {
		if (cam->engines[i]->port)
			continue;

		ret = mtk_camsv_link_port(cam->engines[i]);
		if (ret && ret != -ENOLINK) {
			dev_err(cam->dev, "camsv%u: link failed: %d\n",
				cam->engines[i]->index, ret);
			err = ret;
		}
	}

	return err;
}

void mtk_camsv_unregister(struct mtk_cam *cam)
{
	unsigned int i;

	for (i = 0; i < cam->num_engines; i++) {
		struct mtk_camsv *camsv = cam->engines[i];

		/* Disallow userspace access first, then tear the pipeline */
		vb2_video_unregister_device(&camsv->vdev);
		free_irq(camsv->irq, camsv);
		vb2_queue_release(&camsv->queue);
		v4l2_subdev_cleanup(&camsv->sd);
		media_entity_cleanup(&camsv->sd.entity);
	}
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek legacy CamSV (Camera Simple) DMA capture engine
 *
 * Copyright (C) 2026 Meizu fleet kernel port
 *
 * A CamSV engine has no ISP behind it: it takes a CSI-2 port's pixel stream
 * from SENINF, runs a timing generator locked to the sensor frame boundaries
 * and writes every frame through a single DMA output (IMGO) into
 * DMA-contiguous memory.  One engine is exposed as one V4L2 capture node,
 * /dev/videoN, delivering raw Bayer frames.
 *
 * Media graph:
 *
 *   sensor ------> seninf port ------> camsv (/dev/videoN)
 *
 * The engine drives the whole stream: on .start_streaming() it programs the
 * CamSV timing generator and IMGO DMA, enables the SENINF receiver of the
 * port routed into it and only then starts the sensor.  The reverse order is
 * used when streaming stops, so that the receiver never sees an undriven
 * D-PHY clock.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_graph.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <media/media-device.h>
#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "mtk-cam-legacy.h"

static inline u32 camsv_read(struct mtk_camsv *camsv, u32 reg)
{
	return readl(camsv->base + reg);
}

static inline void camsv_write(struct mtk_camsv *camsv, u32 reg, u32 val)
{
	writel(val, camsv->base + reg);
}

/* --------------------------------------------------------------------------
 * Connection to the SENINF port
 * ------------------------------------------------------------------------ */

/*
 * Retrieve the seninf port whose output endpoint points at this engine.  The
 * engine node is a child of the camera node and its own "reg" is the engine
 * index used by the seninf output endpoints.
 */
static struct mtk_seninf_port *mtk_camsv_get_port(struct mtk_camsv *camsv)
{
	struct mtk_cam_legacy *cam = camsv->cam;
	unsigned int i;

	for (i = 0; i < cam->num_ports; i++) {
		struct mtk_seninf_port *port = &cam->ports[i];
		struct fwnode_handle *ep, *remote, *port_parent;
		u32 remote_idx = 0;
		bool match = false;

		ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(cam->dev),
						     port->index, 1,
						     FWNODE_GRAPH_DEVICE_DISABLED);
		if (!ep)
			continue;

		remote = fwnode_graph_get_remote_endpoint(ep);
		fwnode_handle_put(ep);
		if (!remote)
			continue;

		port_parent = fwnode_graph_get_port_parent(remote);
		fwnode_handle_put(remote);
		if (port_parent) {
			match = (!fwnode_property_read_u32(port_parent,
							   "cell-index",
							   &remote_idx) ||
				 !fwnode_property_read_u32(port_parent,
							   "reg",
							   &remote_idx)) &&
				remote_idx == camsv->index;
			fwnode_handle_put(port_parent);
		}

		if (match)
			return port;
	}

	return NULL;
}

/* --------------------------------------------------------------------------
 * Format helpers
 * ------------------------------------------------------------------------ */

static u32 mtk_camsv_bytes_per_pixel(u32 code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SBGGR8_1X8:
	case MEDIA_BUS_FMT_SGBRG8_1X8:
	case MEDIA_BUS_FMT_SGRBG8_1X8:
	case MEDIA_BUS_FMT_SRGGB8_1X8:
		return 1;
	case MEDIA_BUS_FMT_SBGGR10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
	case MEDIA_BUS_FMT_SBGGR12_1X12:
	case MEDIA_BUS_FMT_SGBRG12_1X12:
	case MEDIA_BUS_FMT_SGRBG12_1X12:
	case MEDIA_BUS_FMT_SRGGB12_1X12:
		/* 10/12 bit samples are stored into 16 bit words */
		return 2;
	default:
		return 0;
	}
}

static u32 mtk_camsv_pixel_mode(u32 code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SBGGR10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
		return CAMSV_CRTL_CFG_RAW10;
	case MEDIA_BUS_FMT_SBGGR12_1X12:
	case MEDIA_BUS_FMT_SGBRG12_1X12:
	case MEDIA_BUS_FMT_SGRBG12_1X12:
	case MEDIA_BUS_FMT_SRGGB12_1X12:
		return CAMSV_CRTL_CFG_RAW12;
	default:
		return CAMSV_CRTL_CFG_RAW8;
	}
}

static u32 mtk_camsv_code_to_pixfmt(u32 code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SBGGR8_1X8:
	case MEDIA_BUS_FMT_SGBRG8_1X8:
	case MEDIA_BUS_FMT_SGRBG8_1X8:
	case MEDIA_BUS_FMT_SRGGB8_1X8:
		return V4L2_PIX_FMT_SBGGR8;
	case MEDIA_BUS_FMT_SBGGR12_1X12:
	case MEDIA_BUS_FMT_SGBRG12_1X12:
	case MEDIA_BUS_FMT_SGRBG12_1X12:
	case MEDIA_BUS_FMT_SRGGB12_1X12:
		return V4L2_PIX_FMT_SBGGR12;
	default:
		return V4L2_PIX_FMT_SBGGR10;
	}
}

static void mtk_camsv_update_sizes(struct mtk_camsv *camsv)
{
	u32 bpp = mtk_camsv_bytes_per_pixel(camsv->format.code);
	u32 align = camsv->cam->soc->data_alignment;

	if (!bpp)
		bpp = 2;

	camsv->bytesperline = ALIGN(camsv->width * bpp, align);
	camsv->sizeimage = camsv->bytesperline * camsv->height;
}

/* --------------------------------------------------------------------------
 * Hardware programming
 * ------------------------------------------------------------------------ */

static void mtk_camsv_hw_stop(struct mtk_camsv *camsv)
{
	u32 val;

	/* Stop the timing generator first, then the DMA path */
	val = camsv_read(camsv, CAMSV_CRTL_CFG);
	val &= ~CAMSV_CRTL_CFG_STREAM_EN;
	camsv_write(camsv, CAMSV_CRTL_CFG, val);

	camsv_write(camsv, CAMSV_DMA_EN, 0);
	camsv_write(camsv, CAMSV_DMA_RESET, CAMSV_DMA_RESET_IMGO);
	camsv_write(camsv, CAMSV_INT_EN, 0);
	camsv_write(camsv, CAMSV_INT_STATUS,
		    CAMSV_INT_IMGO_DONE | CAMSV_INT_ERRORS);
}

static void mtk_camsv_hw_start(struct mtk_camsv *camsv, dma_addr_t addr)
{
	u32 val;

	/* Reset the engine before programming it */
	camsv_write(camsv, CAMSV_CRTL_CFG, CAMSV_CRTL_CFG_SW_RST);

	val = FIELD_PREP(CAMSV_CRTL_CFG_PIXEL_MODE,
			 mtk_camsv_pixel_mode(camsv->format.code)) |
	      CAMSV_CRTL_CFG_FRAME_MODE;
	camsv_write(camsv, CAMSV_CRTL_CFG, val);

	camsv_write(camsv, CAMSV_TG_SENINF_CON,
		    FIELD_PREP(CAMSV_TG_SENINF_CON_PORT, camsv->port->index));
	camsv_write(camsv, CAMSV_TG_HCNT, camsv->width);
	camsv_write(camsv, CAMSV_TG_VCNT, camsv->height);

	camsv_write(camsv, CAMSV_IMGO_XSIZE, camsv->width);
	camsv_write(camsv, CAMSV_IMGO_YSIZE, camsv->height);
	camsv_write(camsv, CAMSV_IMGO_STRIDE, camsv->bytesperline);
	camsv_write(camsv, CAMSV_IMGO_BASE_ADDR, lower_32_bits(addr));
	camsv_write(camsv, CAMSV_IMGO_FBC, 0);

	camsv_write(camsv, CAMSV_MODULE_EN, CAMSV_MODULE_EN_IMGO);
	camsv_write(camsv, CAMSV_TOP_CFG, CAMSV_TOP_CFG_IMGO_EN);

	camsv_write(camsv, CAMSV_INT_STATUS,
		    CAMSV_INT_IMGO_DONE | CAMSV_INT_ERRORS);
	camsv_write(camsv, CAMSV_INT_EN,
		    CAMSV_INT_IMGO_DONE | CAMSV_INT_ERRORS);

	camsv_write(camsv, CAMSV_DMA_RESET, CAMSV_DMA_RESET_IMGO);
	camsv_write(camsv, CAMSV_DMA_EN, CAMSV_DMA_EN_IMGO);

	val = camsv_read(camsv, CAMSV_CRTL_CFG);
	camsv_write(camsv, CAMSV_CRTL_CFG, val | CAMSV_CRTL_CFG_STREAM_EN);
}

/* --------------------------------------------------------------------------
 * Media-graph sub-device
 * ------------------------------------------------------------------------ */

static int mtk_camsv_sd_enum_mbus_code(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state,
				       struct v4l2_subdev_mbus_code_enum *code)
{
	struct mtk_camsv *camsv = container_of(sd, struct mtk_camsv, sd);

	if (code->index)
		return -EINVAL;

	code->code = camsv->format.code;

	return 0;
}

static int mtk_camsv_sd_get_fmt(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_format *fmt)
{
	struct mtk_camsv *camsv = container_of(sd, struct mtk_camsv, sd);

	if (fmt->pad)
		return -EINVAL;

	fmt->format = camsv->format;

	return 0;
}

static const struct v4l2_subdev_pad_ops mtk_camsv_sd_pad_ops = {
	.enum_mbus_code = mtk_camsv_sd_enum_mbus_code,
	.get_fmt = mtk_camsv_sd_get_fmt,
};

const struct v4l2_subdev_ops mtk_camsv_subdev_ops = {
	.pad = &mtk_camsv_sd_pad_ops,
};

/* --------------------------------------------------------------------------
 * vb2 queue operations
 * ------------------------------------------------------------------------ */

static int mtk_camsv_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
				 unsigned int *nplanes, unsigned int sizes[],
				 struct device *alloc_devs[])
{
	struct mtk_camsv *camsv = vb2_get_drv_priv(vq);
	unsigned int size = camsv->sizeimage;

	if (*nplanes) {
		if (sizes[0] < size)
			return -EINVAL;
		size = sizes[0];
	}

	if (*nbuffers < 2)
		*nbuffers = 2;

	*nplanes = 1;
	sizes[0] = size;

	return vb2_dma_contig_set_max_seg_size(camsv->dev, DMA_BIT_MASK(32));
}

static int mtk_camsv_buf_prepare(struct vb2_buffer *vb)
{
	struct mtk_camsv *camsv = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *v4l2_buf = to_vb2_v4l2_buffer(vb);

	if (vb2_plane_size(vb, 0) < camsv->sizeimage)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, camsv->sizeimage);
	v4l2_buf->field = V4L2_FIELD_NONE;

	return 0;
}

static void mtk_camsv_buf_queue(struct vb2_buffer *vb)
{
	struct mtk_camsv *camsv = vb2_get_drv_priv(vb->vb2_queue);
	struct mtk_camsv_buffer *buf = container_of(vb,
						    struct mtk_camsv_buffer,
						    vb.vb2_buf);
	unsigned long flags;

	spin_lock_irqsave(&camsv->qlock, flags);
	list_add_tail(&buf->list, &camsv->bufs);
	spin_unlock_irqrestore(&camsv->qlock, flags);
}

static void mtk_camsv_return_all_buffers(struct mtk_camsv *camsv,
					 enum vb2_buffer_state state)
{
	struct mtk_camsv_buffer *buf, *tmp;

	lockdep_assert_held(&camsv->lock);

	list_for_each_entry_safe(buf, tmp, &camsv->bufs, list) {
		list_del_init(&buf->list);
		buf->vb.sequence = camsv->sequence;
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}

	if (camsv->active) {
		buf = camsv->active;
		camsv->active = NULL;
		buf->vb.sequence = camsv->sequence;
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
}

static int mtk_camsv_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct mtk_camsv *camsv = vb2_get_drv_priv(vq);
	struct mtk_cam_legacy *cam = camsv->cam;
	struct mtk_camsv_buffer *buf;
	dma_addr_t addr;
	int ret;

	lockdep_assert_held(&camsv->lock);

	/* Pick up the sensor format the daemon latched in the seninf port */
	if (camsv->port)
		mtk_seninf_apply_fmt(camsv->port, &camsv->format);

	if (!camsv->port || !camsv->port->sensor_sd) {
		dev_err(cam->dev, "camsv-%u has no sensor attached\n",
			camsv->index);
		ret = -ENODEV;
		goto err_buffers;
	}

	mtk_camsv_update_sizes(camsv);
	camsv->sequence = 0;

	spin_lock_irq(&camsv->qlock);
	buf = list_first_entry_or_null(&camsv->bufs,
				       struct mtk_camsv_buffer, list);
	if (buf)
		list_del_init(&buf->list);
	camsv->active = buf;
	spin_unlock_irq(&camsv->qlock);

	if (!buf) {
		ret = -ENOMEM;
		goto err_buffers;
	}

	addr = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);

	pm_runtime_get_sync(cam->dev);

	mtk_camsv_hw_start(camsv, addr);
	mtk_seninf_port_enable(camsv->port);

	camsv->streaming = true;

	ret = v4l2_subdev_call(camsv->port->sensor_sd, video, s_stream, 1);
	if (ret && ret != -ENOIOCTLCMD) {
		camsv->streaming = false;
		mtk_seninf_port_disable(camsv->port);
		mtk_camsv_hw_stop(camsv);
		pm_runtime_put(cam->dev);
		goto err_buffers;
	}

	dev_dbg(cam->dev, "camsv-%u: streaming %ux%u at %u bytes/line\n",
		camsv->index, camsv->width, camsv->height,
		camsv->bytesperline);

	return 0;

err_buffers:
	mtk_camsv_return_all_buffers(camsv, VB2_BUF_STATE_QUEUED);

	return ret;
}

static void mtk_camsv_stop_streaming(struct vb2_queue *vq)
{
	struct mtk_camsv *camsv = vb2_get_drv_priv(vq);
	struct mtk_cam_legacy *cam = camsv->cam;

	lockdep_assert_held(&camsv->lock);

	if (camsv->streaming) {
		v4l2_subdev_call(camsv->port->sensor_sd, video, s_stream, 0);
		mtk_seninf_port_disable(camsv->port);
		mtk_camsv_hw_stop(camsv);
		camsv->streaming = false;

		pm_runtime_put(cam->dev);
	}

	mtk_camsv_return_all_buffers(camsv, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops mtk_camsv_qops = {
	.queue_setup = mtk_camsv_queue_setup,
	.buf_prepare = mtk_camsv_buf_prepare,
	.buf_queue = mtk_camsv_buf_queue,
	.start_streaming = mtk_camsv_start_streaming,
	.stop_streaming = mtk_camsv_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static int mtk_camsv_queue_init(struct mtk_camsv *camsv)
{
	struct vb2_queue *q = &camsv->queue;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->dev = camsv->dev;
	q->ops = &mtk_camsv_qops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->buf_struct_size = sizeof(struct mtk_camsv_buffer);
	q->drv_priv = camsv;
	q->lock = &camsv->lock;
	q->min_queued_buffers = 1;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->allow_cache_hints = 1;

	return vb2_queue_init(q);
}

/* --------------------------------------------------------------------------
 * Interrupt handler
 * ------------------------------------------------------------------------ */

static irqreturn_t mtk_camsv_irq(int irq, void *data)
{
	struct mtk_camsv *camsv = data;
	struct mtk_camsv_buffer *done, *next;
	unsigned long flags;
	dma_addr_t addr;
	u32 status;

	status = camsv_read(camsv, CAMSV_INT_STATUS);
	if (!status)
		return IRQ_NONE;

	camsv_write(camsv, CAMSV_INT_STATUS,
		    CAMSV_INT_IMGO_DONE | CAMSV_INT_ERRORS);

	if (status & CAMSV_INT_ERRORS)
		camsv->errors++;

	if (!(status & CAMSV_INT_IMGO_DONE))
		return IRQ_HANDLED;

	spin_lock_irqsave(&camsv->qlock, flags);

	done = camsv->active;
	next = list_first_entry_or_null(&camsv->bufs,
					struct mtk_camsv_buffer, list);
	if (next)
		list_del_init(&next->list);
	camsv->active = next;

	if (next) {
		addr = vb2_dma_contig_plane_dma_addr(&next->vb.vb2_buf, 0);
		camsv_write(camsv, CAMSV_IMGO_BASE_ADDR, lower_32_bits(addr));
		camsv_write(camsv, CAMSV_IMGO_FBC, 0);
	} else {
		/* No buffer to write into: drop frames until one arrives */
		camsv_write(camsv, CAMSV_IMGO_FBC, CAMSV_IMGO_FBC_DROP_FRAME);
	}

	spin_unlock_irqrestore(&camsv->qlock, flags);

	if (done) {
		done->vb.vb2_buf.timestamp = ktime_get_ns();
		done->vb.sequence = camsv->sequence++;
		done->vb.field = V4L2_FIELD_NONE;
		vb2_buffer_done(&done->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}

	return IRQ_HANDLED;
}

/* --------------------------------------------------------------------------
 * Video node
 * ------------------------------------------------------------------------ */

static int mtk_camsv_querycap(struct file *file, void *priv,
			      struct v4l2_capability *cap)
{
	struct mtk_camsv *camsv = video_drvdata(file);

	strscpy(cap->driver, MTK_CAM_DRV_NAME, sizeof(cap->driver));
	snprintf(cap->card, sizeof(cap->card), "MTK CamSV%u", camsv->index);
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(camsv->cam->dev));

	return 0;
}

static int mtk_camsv_enum_fmt(struct file *file, void *priv,
			      struct v4l2_fmtdesc *f)
{
	struct mtk_camsv *camsv = video_drvdata(file);

	if (f->index)
		return -EINVAL;

	f->pixelformat = mtk_camsv_code_to_pixfmt(camsv->format.code);
	snprintf(f->description, sizeof(f->description), "%ux%u raw Bayer",
		 camsv->width, camsv->height);

	return 0;
}

static void mtk_camsv_fill_pixfmt(struct mtk_camsv *camsv,
				  struct v4l2_pix_format_mplane *pix)
{
	pix->width = camsv->width;
	pix->height = camsv->height;
	pix->pixelformat = mtk_camsv_code_to_pixfmt(camsv->format.code);
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_RAW;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_NONE;
	pix->num_planes = 1;
	pix->plane_fmt[0].bytesperline = camsv->bytesperline;
	pix->plane_fmt[0].sizeimage = camsv->sizeimage;
}

static int mtk_camsv_g_fmt(struct file *file, void *priv,
			   struct v4l2_format *f)
{
	struct mtk_camsv *camsv = video_drvdata(file);

	memset(&f->fmt.pix_mp, 0, sizeof(f->fmt.pix_mp));
	mtk_camsv_fill_pixfmt(camsv, &f->fmt.pix_mp);

	return 0;
}

static int mtk_camsv_try_fmt(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	struct mtk_camsv *camsv = video_drvdata(file);
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
	u32 bpp = mtk_camsv_bytes_per_pixel(camsv->format.code);
	u32 align = camsv->cam->soc->data_alignment;

	if (!bpp)
		bpp = 2;

	/*
	 * The engine has no scaler and no format converter: the frame size is
	 * the sensor readout size and the pixel format follows the Bayer code
	 * the sensor produces.
	 */
	pix->width = camsv->width;
	pix->height = camsv->height;
	pix->pixelformat = mtk_camsv_code_to_pixfmt(camsv->format.code);
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_RAW;
	pix->num_planes = 1;
	pix->plane_fmt[0].bytesperline = ALIGN(pix->width * bpp, align);
	pix->plane_fmt[0].sizeimage = pix->plane_fmt[0].bytesperline *
				      pix->height;

	return 0;
}

static int mtk_camsv_s_fmt(struct file *file, void *priv,
			   struct v4l2_format *f)
{
	struct mtk_camsv *camsv = video_drvdata(file);
	int ret;

	ret = mtk_camsv_try_fmt(file, priv, f);
	if (ret)
		return ret;

	if (vb2_is_busy(&camsv->queue))
		return -EBUSY;

	mtk_camsv_update_sizes(camsv);

	return 0;
}

static const struct v4l2_ioctl_ops mtk_camsv_ioctl_ops = {
	.vidioc_querycap = mtk_camsv_querycap,
	.vidioc_enum_fmt_vid_cap = mtk_camsv_enum_fmt,
	.vidioc_g_fmt_vid_cap_mplane = mtk_camsv_g_fmt,
	.vidioc_try_fmt_vid_cap_mplane = mtk_camsv_try_fmt,
	.vidioc_s_fmt_vid_cap_mplane = mtk_camsv_s_fmt,

	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,

	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations mtk_camsv_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/* --------------------------------------------------------------------------
 * Per-engine platform device
 * ------------------------------------------------------------------------ */

static int mtk_camsv_engine_probe(struct platform_device *pdev)
{
	struct mtk_camsv **camsvp = dev_get_platdata(&pdev->dev);
	struct mtk_camsv *camsv = camsvp ? *camsvp : NULL;
	struct device *dev = &pdev->dev;
	int ret;

	if (!camsv)
		return -ENODEV;

	camsv->dev = dev;
	camsv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(camsv->base))
		return PTR_ERR(camsv->base);

	ret = device_property_read_u32(dev, "cell-index", &camsv->index);
	if (ret)
		camsv->index = pdev->id;

	camsv->irq = platform_get_irq(pdev, 0);
	if (camsv->irq < 0)
		return camsv->irq;

	mutex_init(&camsv->lock);
	spin_lock_init(&camsv->qlock);
	INIT_LIST_HEAD(&camsv->bufs);

	camsv->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	camsv->format.field = V4L2_FIELD_NONE;
	camsv->format.colorspace = V4L2_COLORSPACE_RAW;
	camsv->width = 640;
	camsv->height = 480;
	mtk_camsv_update_sizes(camsv);

	ret = devm_request_irq(dev, camsv->irq, mtk_camsv_irq, IRQF_SHARED,
			       dev_name(dev), camsv);
	if (ret)
		return ret;

	camsv->port = mtk_camsv_get_port(camsv);
	if (camsv->port)
		camsv->port->camsv = camsv;

	/* The sub-device only describes the engine entity in the graph */
	v4l2_subdev_init(&camsv->sd, &mtk_camsv_subdev_ops);
	camsv->sd.owner = THIS_MODULE;
	camsv->sd.dev = dev;
	snprintf(camsv->sd.name, sizeof(camsv->sd.name), "camsv-%u",
		 camsv->index);
	camsv->pads[0].flags = MEDIA_PAD_FL_SINK;
	camsv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;

	ret = media_entity_pads_init(&camsv->sd.entity, 1, camsv->pads);
	if (ret)
		return ret;

	return v4l2_subdev_init_finalize(&camsv->sd);
}

static const struct platform_device_id mtk_camsv_engine_ids[] = {
	{ .name = "mtk-camsv-legacy" },
	{ }
};
MODULE_DEVICE_TABLE(platform, mtk_camsv_engine_ids);

static struct platform_driver mtk_camsv_engine_driver = {
	.probe = mtk_camsv_engine_probe,
	.driver = {
		.name = "mtk-camsv-legacy",
	},
	.id_table = mtk_camsv_engine_ids,
};

int mtk_camsv_init(struct mtk_cam_legacy *cam)
{
	unsigned int n = 0;
	int ret;

	fwnode_for_each_available_child_node_scoped(dev_fwnode(cam->dev), child)
		if (fwnode_device_is_compatible(child,
						"mediatek,cam-simple-engine"))
			n++;

	if (!n)
		return 0;

	cam->camsv = devm_kcalloc(cam->dev, n, sizeof(*cam->camsv),
				  GFP_KERNEL);
	if (!cam->camsv)
		return -ENOMEM;

	/*
	 * Register the engine driver before the engine devices so that each
	 * platform_device_register_full() binds immediately and the engines
	 * exist in the media graph before the graph is built.
	 */
	ret = platform_driver_register(&mtk_camsv_engine_driver);
	if (ret)
		return ret;

	n = 0;
	fwnode_for_each_available_child_node_scoped(dev_fwnode(cam->dev), child) {
		struct resource res[2];
		unsigned int num_res = 0;
		struct device_node *np = to_of_node(child);
		struct platform_device_info pdevinfo = {
			.parent = cam->dev,
			.fwnode = child,
			.name = "mtk-camsv-legacy",
			.id = n,
		};
		struct mtk_camsv *camsv = &cam->camsv[n];
		struct platform_device *pdev;

		if (!fwnode_device_is_compatible(child,
						 "mediatek,cam-simple-engine"))
			continue;

		if (np) {
			if (!of_address_to_resource(np, 0, &res[num_res]))
				num_res++;
			if (of_irq_to_resource(np, 0, &res[num_res]) > 0)
				num_res++;
			pdevinfo.res = res;
			pdevinfo.num_res = num_res;
		}

		camsv->cam = cam;

		/* Bound through platform_data, before the driver probes it */
		pdevinfo.data = &camsv;
		pdevinfo.size_data = sizeof(camsv);

		pdev = platform_device_register_full(&pdevinfo);
		if (IS_ERR(pdev)) {
			ret = PTR_ERR(pdev);
			goto err_driver;
		}

		n++;
	}

	cam->num_camsv = n;

	return 0;

err_driver:
	platform_driver_unregister(&mtk_camsv_engine_driver);

	while (n--)
		if (cam->camsv[n].dev)
			platform_device_unregister(to_platform_device(cam->camsv[n].dev));

	return ret;
}

/* --------------------------------------------------------------------------
 * Video node registration
 * ------------------------------------------------------------------------ */

static int mtk_camsv_add_video(struct mtk_camsv *camsv)
{
	int ret;

	snprintf(camsv->vdev.name, sizeof(camsv->vdev.name), "camsv%u",
		 camsv->index);
	camsv->vdev.v4l2_dev = &camsv->cam->v4l2_dev;
	camsv->vdev.fops = &mtk_camsv_fops;
	camsv->vdev.ioctl_ops = &mtk_camsv_ioctl_ops;
	camsv->vdev.release = video_device_release_empty;
	camsv->vdev.lock = &camsv->lock;
	camsv->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE |
				  V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
	camsv->vdev.vfl_dir = VFL_DIR_RX;
	camsv->vdev.entity.function = MEDIA_ENT_F_IO_V4L;
	video_set_drvdata(&camsv->vdev, camsv);

	ret = mtk_camsv_queue_init(camsv);
	if (ret)
		return ret;

	camsv->vdev.queue = &camsv->queue;

	ret = video_register_device(&camsv->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		vb2_queue_release(&camsv->queue);
		return ret;
	}

	return 0;
}

/**
 * mtk_camsv_add_nodes - create the video nodes of all engines
 * @cam: camera subsystem instance
 *
 * Called once the media graph is complete, so that the video nodes appear
 * with their links already in place.
 */
int mtk_camsv_add_nodes(struct mtk_cam_legacy *cam)
{
	unsigned int i;
	int ret;

	for (i = 0; i < cam->num_camsv; i++) {
		ret = mtk_camsv_add_video(&cam->camsv[i]);
		if (ret)
			return ret;
	}

	return 0;
}

void mtk_camsv_remove_nodes(struct mtk_cam_legacy *cam)
{
	unsigned int i;

	for (i = 0; i < cam->num_camsv; i++) {
		struct mtk_camsv *camsv = &cam->camsv[i];

		if (!video_is_registered(&camsv->vdev))
			continue;

		video_unregister_device(&camsv->vdev);
		vb2_queue_release(&camsv->queue);
	}
}

void mtk_camsv_cleanup(struct mtk_cam_legacy *cam)
{
	unsigned int i;

	if (!cam->num_camsv)
		return;

	platform_driver_unregister(&mtk_camsv_engine_driver);

	for (i = 0; i < cam->num_camsv; i++) {
		struct mtk_camsv *camsv = &cam->camsv[i];

		v4l2_subdev_cleanup(&camsv->sd);
		media_entity_cleanup(&camsv->sd.entity);

		if (camsv->dev)
			platform_device_unregister(to_platform_device(camsv->dev));
	}
}

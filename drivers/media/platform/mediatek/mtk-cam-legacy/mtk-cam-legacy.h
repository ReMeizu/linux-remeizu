/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Register and device tree definitions for the MediaTek legacy camera path
 * (MT6735/MT6755/MT6797 families).
 *
 * Copyright (C) 2026 Meizu fleet kernel port
 *
 * All offsets are relative to the block base address given in the device
 * tree.  The register layout is shared between the SoCs of this generation:
 * they all use the same "seninf"/"camsv" hardware generation driven by the
 * vendor 3.18/4.9 kernels (drivers/misc/mediatek/camera_isp, camsv).
 */

#ifndef __MTK_CAM_LEGACY_H__
#define __MTK_CAM_LEGACY_H__

#include <linux/bits.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <linux/clk.h>

#include <media/media-device.h>
#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-v4l2.h>

/* --------------------------------------------------------------------------
 * CSI-2 receiver (SENINF)
 * ------------------------------------------------------------------------ */

#define SENINF_CSI2_RX_CON(n)		(0x0000 + 0x40 * (n))
#define SENINF_CSI2_RX_CON_LANES	GENMASK(1, 0)
#define SENINF_CSI2_RX_CON_LANES_SHIFT	0
#define SENINF_CSI2_RX_CON_CSI2_MODE	GENMASK(6, 4)
#define SENINF_CSI2_RX_CON_CSI2_DPHY	0x4
#define SENINF_CSI2_RX_CON_HS_SETTLE	GENMASK(23, 16)
#define SENINF_CSI2_RX_CON_HS_SHIFT	16

#define SENINF_CSI2_RX_EN(n)		(0x0010 + 0x40 * (n))
#define SENINF_CSI2_RX_EN_ON		BIT(0)
#define SENINF_CSI2_RX_EN_TG1		BIT(1)

#define SENINF_CSI2_IRQ_STATUS(n)	(0x0028 + 0x40 * (n))
#define SENINF_CSI2_IRQ_OVERRUN		BIT(0)
#define SENINF_CSI2_IRQ_CRC		BIT(1)
#define SENINF_CSI2_IRQ_FSM		BIT(2)
#define SENINF_CSI2_IRQ_VSIZE		BIT(3)
#define SENINF_CSI2_IRQ_HSIZE		BIT(4)
#define SENINF_CSI2_IRQ_SENSOR_VSIZE	BIT(5)
#define SENINF_CSI2_IRQ_SENSOR_HSIZE	BIT(6)

/* Input path: sensor N feeds seninf port (pad) N, POL_SWAP inverts polarity */
#define SENINF_PAD_CON(n)		(0x0200 + 0x10 * (n))
#define SENINF_PAD_CON_POL_SWAP		BIT(15)

/* Shared D-PHY settle delay, in units of 100 DDR clock cycles */
#define SENINF_SETTLE_CTRL		0x0300
#define SENINF_SETTLE_DELAY		GENMASK(7, 0)

#define SENINF_TOP_CTRL			0x0400
#define SENINF_TOP_SW_RST		BIT(0)

/* --------------------------------------------------------------------------
 * Camera video front-end (CamSV): one register block and one IRQ per engine
 * ------------------------------------------------------------------------ */

#define CAMSV_TOP_CFG			0x004
#define CAMSV_TOP_CFG_IMGO_EN		BIT(3)

#define CAMSV_INT_EN			0x010
#define CAMSV_INT_STATUS		0x018
#define CAMSV_INT_DONE			BIT(0)
#define CAMSV_INT_ERR			GENMASK(7, 4)

#define CAMSV_DMA_EN			0x050
#define CAMSV_DMA_EN_IMGO		BIT(0)
#define CAMSV_DMA_RESET			0x054

#define CAMSV_IPU_INT_EN		0x400
#define CAMSV_IPU_INT_STATUS		0x408
#define CAMSV_IPU_INT_IMGO_NUM		GENMASK(7, 0)

/* Image output (IMGO) DMA configuration */
#define CAMSV_IMGO_BASE_ADDR		0x300
#define CAMSV_IMGO_XSIZE			0x308
#define CAMSV_IMGO_YSIZE			0x30c
#define CAMSV_IMGO_STRIDE		0x310
#define CAMSV_IMGO_FBC			0x330

/* Timing generator: driven by the seninf port selected in TG_SENINF_CON */
#define CAMSV_TG_CTRL			0x600
#define CAMSV_TG_CTRL_EN		BIT(0)
#define CAMSV_TG_MODE			0x604
#define CAMSV_TG_MODE_VSYNC		BIT(0)
#define CAMSV_TG_VCNT			0x608
#define CAMSV_TG_HCNT			0x60c
#define CAMSV_TG_PCNT			0x610
#define CAMSV_TG_SENINF_CON		0x614
#define CAMSV_TG_SENINF_PORT		GENMASK(3, 0)

#define CAMSV_CTRL_CFG			0x700
#define CAMSV_CTRL_CFG_PIXEL_MODE	GENMASK(1, 0)
#define CAMSV_CTRL_CFG_RAW8		0
#define CAMSV_CTRL_CFG_RAW10		1
#define CAMSV_CTRL_CFG_RAW12		2
#define CAMSV_CTRL_CFG_STREAM		BIT(4)
#define CAMSV_CTRL_CFG_SW_RST		BIT(8)

#define CAMSV_MODULE_EN			0x720
#define CAMSV_MODULE_EN_TG		BIT(0)
#define CAMSV_MODULE_EN_IMGO		BIT(1)
#define CAMSV_MODULE_EN_SENINF		BIT(2)

#define CAMSV_FMT_SEL			0x730
#define CAMSV_SW_CTL			0x734

#define CAMSV_MAX_WIDTH			4224
#define CAMSV_MAX_HEIGHT		3200
#define CAMSV_MIN_WIDTH			32
#define CAMSV_MIN_HEIGHT		32

#define MTK_CAM_DRV_NAME		"mtk-cam-legacy"

struct mtk_cam_legacy;
struct mtk_camsv;

/**
 * struct mtk_seninf_port - one CSI-2 receive port exported as a sub-device
 * @cam: backpointer to the SoC instance
 * @index: port index, selects the SENINF register bank
 * @sd: V4L2 sub-device representing the port
 * @pads: sink (from the sensor) and source (to the CamSV engine)
 * @lock: protects @format against stream toggling
 * @format: active media bus format
 * @lanes: number of D-PHY data lanes, from the sensor endpoint
 * @link_freq_hz: sensor D-PHY link frequency in Hz
 * @settle_ns: D-PHY data lane settle time, from device tree
 * @sensor_sd: remote sensor sub-device, NULL until discovered
 * @camsv: CamSV engine consuming this port, NULL if unused
 */
struct mtk_seninf_port {
	struct mtk_cam_legacy *cam;
	unsigned int index;

	struct v4l2_subdev sd;
	struct media_pad pads[2];
	struct mutex lock;

	struct v4l2_mbus_framefmt format;
	unsigned int lanes;
	u64 link_freq_hz;
	u32 settle_ns;

	struct v4l2_subdev *sensor_sd;
	struct mtk_camsv *camsv;
};

/**
 * struct mtk_camsv_buffer - one capture buffer queued on a CamSV engine
 * @vb: videobuf2 buffer
 * @list: node in the engine's pending list
 */
struct mtk_camsv_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

/**
 * struct mtk_camsv - one CamSV DMA engine and its video node
 * @cam: backpointer to the SoC instance
 * @dev: platform device of the engine, owner of the DMA memory
 * @index: engine index within the CamSV block
 * @base: register window
 * @irq: DMA done interrupt
 * @vdev: capture video node (/dev/videoN), the media graph entity
 * @sd: sub-device describing the engine in the graph
 * @pads: sink pad of the engine entity
 * @port: seninf port routed into this engine
 * @format: media bus format delivered by the engine
 * @width: capture width in pixels
 * @height: capture height in pixels
 * @bytesperline: line stride in bytes
 * @sizeimage: frame size in bytes
 * @queue: vb2 queue
 * @lock: serialises the ioctls and the buffer list
 * @qlock: protects @bufs and @active against the interrupt
 * @bufs: queued buffers waiting for a frame
 * @active: buffer the DMA is currently writing into
 * @streaming: true between .streamon and .streamoff
 * @sequence: frame counter reported to userspace
 * @errors: capture error counter
 */
struct mtk_camsv {
	struct mtk_cam_legacy *cam;
	struct device *dev;
	unsigned int index;
	void __iomem *base;
	int irq;

	struct video_device vdev;
	struct v4l2_subdev sd;
	struct media_pad pads[1];

	struct mtk_seninf_port *port;

	struct v4l2_mbus_framefmt format;
	u32 width;
	u32 height;
	u32 bytesperline;
	u32 sizeimage;

	struct vb2_queue queue;
	struct mutex lock;
	spinlock_t qlock;
	struct list_head bufs;
	struct mtk_camsv_buffer *active;
	bool streaming;

	u32 sequence;
	u32 errors;
};

/**
 * struct mtk_cam_legacy_soc - per-SoC parameters
 * @name: SoC name, for the media device model string
 * @seninf_ports: number of CSI-2 receive ports of the SENINF block
 * @rx_clk_rate: MIPI RX clock frequency in Hz
 * @data_alignment: required DMA line alignment in bytes
 */
struct mtk_cam_legacy_soc {
	const char *name;
	unsigned int seninf_ports;
	unsigned long rx_clk_rate;
	unsigned int data_alignment;
};

/**
 * struct mtk_cam_legacy - one camera capture subsystem instance
 * @dev: platform device
 * @seninf_base: SENINF register window
 * @soc: SoC descriptor
 * @notifier: async notifier collecting the sensors
 * @v4l2_dev: V4L2 device
 * @media_dev: media device
 * @pads: total number of pads in the graph
 * @num_ports: number of seninf ports described in device tree
 * @ports: seninf ports
 * @num_camsv: number of CamSV engines described in device tree
 * @camsv: CamSV engines
 * @sensors: sub-devices of the sensors found by the notifier
 * @num_sensors: number of entries in @sensors
 * @graph_built: true once the media graph has been registered
 * @lock: serialises the graph setup
 */
struct mtk_cam_legacy {
	struct device *dev;
	void __iomem *seninf_base;
	const struct mtk_cam_legacy_soc *soc;

	struct v4l2_async_notifier notifier;
	struct v4l2_device v4l2_dev;
	struct media_device media_dev;
	unsigned int pads;

	unsigned int num_ports;
	struct mtk_seninf_port *ports;

	unsigned int num_camsv;
	struct mtk_camsv *camsv;

	struct v4l2_subdev **sensors;
	unsigned int num_sensors;
	bool graph_built;

	struct mutex lock;
};

int mtk_seninf_init(struct mtk_cam_legacy *cam);
void mtk_seninf_cleanup(struct mtk_cam_legacy *cam);
void mtk_seninf_port_enable(struct mtk_seninf_port *port);
void mtk_seninf_port_disable(struct mtk_seninf_port *port);
void mtk_seninf_apply_fmt(struct mtk_seninf_port *port,
			  const struct v4l2_mbus_framefmt *fmt);

int mtk_camsv_init(struct mtk_cam_legacy *cam);
void mtk_camsv_cleanup(struct mtk_cam_legacy *cam);
int mtk_camsv_add_nodes(struct mtk_cam_legacy *cam);
void mtk_camsv_remove_nodes(struct mtk_cam_legacy *cam);

u8 mtk_mbus_code_to_csi2_dt(u32 code);

extern const struct v4l2_subdev_ops mtk_camsv_subdev_ops;

#endif /* __MTK_CAM_LEGACY_H__ */

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Register, device tree and state definitions for the MediaTek legacy camera
 * capture path (MT6735/MT6755/MT6797 and their -M derivatives).
 *
 * Copyright (C) 2026 Meizu fleet kernel port
 *
 * Hardware model
 * --------------
 * On these SoCs the camera capture path is split in two blocks:
 *
 *   SENINF  - the CSI-2 receiver.  It deserialises the D-PHY lanes of one or
 *             more sensor ports into a parallel pixel stream and hands that
 *             stream to one or more CamSV engines through the test mux.
 *
 *   CamSV   - "Camera Simple": a stream engine with no ISP behind it, only a
 *             single DMA output (IMGO) and a timing generator (TG).  Each
 *             CamSV engine appears as one /dev/videoN V4L2 capture node
 *             delivering raw Bayer frames into DMA-contiguous memory.
 *
 * Media graph:
 *
 *   sensor subdev ------> seninf port subdev ------> camsv video node
 *     (I2C sensor)         (/dev/v4l-subdevN)         (/dev/videoN)
 *
 * The sensor is an unrelated V4L2 sub-device registered by its own I2C
 * driver (drivers/media/i2c/s5k4h8.c and friends); it is matched through the
 * "remote-endpoint" of the seninf port's sink endpoint.
 */

#ifndef __MTK_CAM_LEGACY_H__
#define __MTK_CAM_LEGACY_H__

#include <linux/bits.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/videodev2.h>

#include <media/media-device.h>
#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-v4l2.h>

/* --------------------------------------------------------------------------
 * SENINF (CSI-2 receiver), one 4 KiB window per SoC
 * ------------------------------------------------------------------------ */

/* Per-port receive configuration */
#define SENINF_RX_CON(n)		(0x0000 + 0x40 * (n))
#define SENINF_RX_CON_LANE_NUM		GENMASK(3, 0)
#define SENINF_RX_CON_MIPI_MODE		GENMASK(6, 4)
#define SENINF_RX_CON_MIPI_MODE_CSI2	0x4
#define SENINF_RX_CON_PHY_SEL		GENMASK(9, 8)
#define SENINF_RX_CON_LONG_PKT_EN	BIT(10)
#define SENINF_RX_CON_HS_SETTLE		GENMASK(23, 16)

/* Shared receive enable: one bit per port */
#define SENINF_RX_EN			0x0010
#define SENINF_RX_EN_MIPI_RX_EN(n)	BIT(n)

/* Per-port pad control; POL_SWAP inverts the D-PHY polarity */
#define SENINF_PAD_CON(n)		(0x0200 + 0x10 * (n))
#define SENINF_PAD_CON_POL_SWAP		BIT(15)

/* Test/routing mux: which seninf port feeds which CamSV TG input */
#define SENINF_TEST_MUX_PORT(n)		(0x0400 + 0x4 * (n))
#define SENINF_TEST_MUX_SEL		GENMASK(3, 0)

#define SENINF_NUM_PORTS		8
#define SENINF_MAX_LANES		4
#define SENINF_DEFAULT_SETTLE_NS	100
#define SENINF_HS_SETTLE_STEP_NS	4

/* --------------------------------------------------------------------------
 * CamSV (DMA capture engine), one 4 KiB window per engine
 * ------------------------------------------------------------------------ */

#define CAMSV_TOP_CFG			0x0004
#define CAMSV_TOP_CFG_IMGO_EN		BIT(3)

#define CAMSV_INT_EN			0x0010
#define CAMSV_INT_STATUS		0x0018
#define CAMSV_INT_IMGO_DONE		BIT(0)
#define CAMSV_INT_OVERFLOW		BIT(1)
#define CAMSV_INT_TG_ERR		BIT(2)
#define CAMSV_INT_ERRORS		(CAMSV_INT_OVERFLOW | CAMSV_INT_TG_ERR)

/* Frame buffer control: capture or drop the incoming frame */
#define CAMSV_IMGO_FBC			0x0030
#define CAMSV_IMGO_FBC_DROP_FRAME	BIT(0)

#define CAMSV_DMA_EN			0x0050
#define CAMSV_DMA_EN_IMGO		BIT(0)
#define CAMSV_DMA_RESET			0x0054
#define CAMSV_DMA_RESET_IMGO		BIT(0)

/* Timing generator */
#define CAMSV_TG_VCNT			0x0608
#define CAMSV_TG_HCNT			0x060c

/* Input selection: seninf port feeding this engine */
#define CAMSV_TG_SENINF_CON		0x0614
#define CAMSV_TG_SENINF_CON_PORT	GENMASK(3, 0)

/* Configuration */
#define CAMSV_CRTL_CFG			0x0700
#define CAMSV_CRTL_CFG_RAW8		0
#define CAMSV_CRTL_CFG_RAW10		1
#define CAMSV_CRTL_CFG_RAW12		2
#define CAMSV_CRTL_CFG_PIXEL_MODE	GENMASK(1, 0)
#define CAMSV_CRTL_CFG_FRAME_MODE	BIT(3)
#define CAMSV_CRTL_CFG_STREAM_EN	BIT(4)
#define CAMSV_CRTL_CFG_SW_RST		BIT(8)

#define CAMSV_MODULE_EN			0x0720
#define CAMSV_MODULE_EN_IMGO		BIT(0)
#define CAMSV_FMT_SEL			0x0730
#define CAMSV_SW_CTL			0x0734

/* IMGO output window, stride and destination address */
#define CAMSV_IMGO_XSIZE		0x0a00
#define CAMSV_IMGO_YSIZE		0x0a04
#define CAMSV_IMGO_STRIDE		0x0a08
#define CAMSV_IMGO_BASE_ADDR		0x0a0c

#define CAMSV_IMGO_MAX_WIDTH		4224
#define CAMSV_IMGO_MAX_HEIGHT		3200
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

/* mtk_seninf_legacy.c */
int mtk_seninf_init(struct mtk_cam_legacy *cam);
void mtk_seninf_cleanup(struct mtk_cam_legacy *cam);
void mtk_seninf_port_enable(struct mtk_seninf_port *port);
void mtk_seninf_port_disable(struct mtk_seninf_port *port);
void mtk_seninf_apply_fmt(struct mtk_seninf_port *port,
			  const struct v4l2_mbus_framefmt *fmt);
u8 mtk_mbus_code_to_csi2_dt(u32 code);

/* mtk_camsv_legacy.c */
int mtk_camsv_init(struct mtk_cam_legacy *cam);
void mtk_camsv_cleanup(struct mtk_cam_legacy *cam);
int mtk_camsv_add_nodes(struct mtk_cam_legacy *cam);
void mtk_camsv_remove_nodes(struct mtk_cam_legacy *cam);

extern const struct v4l2_subdev_ops mtk_camsv_subdev_ops;

#endif /* __MTK_CAM_LEGACY_H__ */

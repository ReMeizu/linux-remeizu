// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek legacy Camera Serial Interface (CSI-2 receiver) V4L2 sub-device
 *
 * Copyright (C) 2026 Meizu fleet kernel port
 *
 * The SENINF block is the MIPI CSI-2 receive side of the MT6735/MT6755/MT6797
 * camera subsystem.  It accepts several CSI-2 ports, each with its own set of
 * D-PHY lanes, and forwards one port's pixel stream to a CamSV timing
 * generator input through the test mux.
 *
 * Each port is represented here as a V4L2 sub-device with a sink pad fed by a
 * sensor and a source pad consumed by a CamSV engine:
 *
 *   sensor -----------> seninf port -----------> camsv (/dev/videoN)
 *          sink pad 0                source pad 1
 *
 * The sub-device does not program hardware on its own: the CamSV engine that
 * owns the port enables the receiver when streaming starts, so that lane
 * count, CSI-2 mode and routing are applied exactly once, in the right order
 * relative to the CamSV timing generator.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <media/media-device.h>
#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "mtk-cam-legacy.h"

static inline struct mtk_seninf_port *to_seninf_port(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mtk_seninf_port, sd);
}

static inline void seninf_write(struct mtk_cam_legacy *cam, u32 reg, u32 val)
{
	writel(val, cam->seninf_base + reg);
}

static inline u32 seninf_read(struct mtk_cam_legacy *cam, u32 reg)
{
	return readl(cam->seninf_base + reg);
}

/*
 * The D-PHY settle time is expressed in 4 ns steps of the RX clock; the
 * vendor tables use a nominal 100 ns for every SoC of this generation.
 */
static u32 mtk_seninf_settle_code(u32 settle_ns)
{
	return DIV_ROUND_UP(settle_ns, SENINF_HS_SETTLE_STEP_NS);
}

static u32 mtk_seninf_rx_con(struct mtk_seninf_port *port)
{
	return FIELD_PREP(SENINF_RX_CON_LANE_NUM, port->lanes - 1) |
	       FIELD_PREP(SENINF_RX_CON_MIPI_MODE, SENINF_RX_CON_MIPI_MODE_CSI2) |
	       FIELD_PREP(SENINF_RX_CON_PHY_SEL, 0) |
	       FIELD_PREP(SENINF_RX_CON_HS_SETTLE,
			  mtk_seninf_settle_code(port->settle_ns)) |
	       SENINF_RX_CON_LONG_PKT_EN;
}

/**
 * mtk_seninf_port_enable - program the receiver for a port and route it
 * @port: seninf port to enable
 *
 * Programs the number of data lanes, the CSI-2 mode and the D-PHY settle time
 * of @port, routes it to the CamSV engine that consumes it and raises the
 * receive enable bit.  The caller must hold @port->cam->lock and must have
 * configured the CamSV timing generator first.
 */
void mtk_seninf_port_enable(struct mtk_seninf_port *port)
{
	struct mtk_cam_legacy *cam = port->cam;
	u32 val;

	seninf_write(cam, SENINF_RX_CON(port->index), mtk_seninf_rx_con(port));
	seninf_write(cam, SENINF_PAD_CON(port->index), 0);

	if (port->camsv)
		seninf_write(cam, SENINF_TEST_MUX_PORT(port->camsv->index),
			     FIELD_PREP(SENINF_TEST_MUX_SEL, port->index));

	val = seninf_read(cam, SENINF_RX_EN);
	val |= SENINF_RX_EN_MIPI_RX_EN(port->index);
	seninf_write(cam, SENINF_RX_EN, val);
}
EXPORT_SYMBOL_GPL(mtk_seninf_port_enable);

/**
 * mtk_seninf_port_disable - stop the receiver of a port
 * @port: seninf port to disable
 */
void mtk_seninf_port_disable(struct mtk_seninf_port *port)
{
	struct mtk_cam_legacy *cam = port->cam;
	u32 val;

	val = seninf_read(cam, SENINF_RX_EN);
	val &= ~SENINF_RX_EN_MIPI_RX_EN(port->index);
	seninf_write(cam, SENINF_RX_EN, val);

	seninf_write(cam, SENINF_RX_CON(port->index), 0);
}
EXPORT_SYMBOL_GPL(mtk_seninf_port_disable);

/**
 * mtk_seninf_apply_fmt - latch the sensor format in the port
 * @port: seninf port
 * @fmt: format produced by the connected sensor
 *
 * The CSI-2 receiver has no scaler and no format converter, so the port
 * simply mirrors what the sensor produces.
 */
void mtk_seninf_apply_fmt(struct mtk_seninf_port *port,
			  const struct v4l2_mbus_framefmt *fmt)
{
	port->format = *fmt;
}

/**
 * mtk_mbus_code_to_csi2_dt - MIPI CSI-2 data type of a media bus code
 * @code: media bus pixel code
 *
 * Returns the MIPI CSI-2 data type the sensor emits for @code, or 0 when the
 * code has no direct CSI-2 mapping.
 */
u8 mtk_mbus_code_to_csi2_dt(u32 code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SBGGR8_1X8:
	case MEDIA_BUS_FMT_SGBRG8_1X8:
	case MEDIA_BUS_FMT_SGRBG8_1X8:
	case MEDIA_BUS_FMT_SRGGB8_1X8:
	case MEDIA_BUS_FMT_Y8_1X8:
	case MEDIA_BUS_FMT_YUYV8_1X16:
	case MEDIA_BUS_FMT_UYVY8_1X16:
	case MEDIA_BUS_FMT_VYUY8_1X16:
	case MEDIA_BUS_FMT_YVYU8_1X16:
		return 0x2a;	/* RAW8 */
	case MEDIA_BUS_FMT_SBGGR10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
	case MEDIA_BUS_FMT_Y10_1X10:
		return 0x2b;	/* RAW10 */
	case MEDIA_BUS_FMT_SBGGR12_1X12:
	case MEDIA_BUS_FMT_SGBRG12_1X12:
	case MEDIA_BUS_FMT_SGRBG12_1X12:
	case MEDIA_BUS_FMT_SRGGB12_1X12:
	case MEDIA_BUS_FMT_Y12_1X12:
		return 0x2c;	/* RAW12 */
	default:
		return 0;
	}
}

/* --------------------------------------------------------------------------
 * V4L2 sub-device operations
 * ------------------------------------------------------------------------ */

static int mtk_seninf_init_state(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state)
{
	struct mtk_seninf_port *port = to_seninf_port(sd);
	struct v4l2_mbus_framefmt *fmt;

	fmt = v4l2_subdev_state_get_format(state, 0);
	*fmt = port->format;

	fmt = v4l2_subdev_state_get_format(state, 1);
	*fmt = port->format;

	return 0;
}

static int mtk_seninf_enum_mbus_code(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_mbus_code_enum *code)
{
	struct mtk_seninf_port *port = to_seninf_port(sd);

	/*
	 * The receiver is transparent: it forwards the sensor's Bayer code
	 * unchanged, so the only code it enumerates is the current one.
	 */
	if (code->index)
		return -EINVAL;

	code->code = port->format.code;

	return 0;
}

static int mtk_seninf_enum_frame_size(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_frame_size_enum *fse)
{
	struct mtk_seninf_port *port = to_seninf_port(sd);

	if (fse->index)
		return -EINVAL;

	fse->code = port->format.code;
	fse->min_width = CAMSV_MIN_WIDTH;
	fse->max_width = CAMSV_IMGO_MAX_WIDTH;
	fse->min_height = CAMSV_MIN_HEIGHT;
	fse->max_height = CAMSV_IMGO_MAX_HEIGHT;

	return 0;
}

static int mtk_seninf_get_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *fmt)
{
	if (fmt->pad >= 2)
		return -EINVAL;

	fmt->format = *v4l2_subdev_state_get_format(state, fmt->pad);

	return 0;
}

static int mtk_seninf_set_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *fmt)
{
	struct mtk_seninf_port *port = to_seninf_port(sd);
	struct v4l2_mbus_framefmt *mf;

	if (fmt->pad >= 2)
		return -EINVAL;

	/* The receiver cannot rescale: only the sensor readout size is valid */
	mtk_seninf_apply_fmt(port, &fmt->format);
	fmt->format = port->format;

	mf = v4l2_subdev_state_get_format(state, fmt->pad);
	*mf = fmt->format;

	return 0;
}

static int mtk_seninf_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				     struct v4l2_mbus_frame_desc *fd)
{
	struct mtk_seninf_port *port = to_seninf_port(sd);

	if (pad >= 2)
		return -EINVAL;

	memset(fd, 0, sizeof(*fd));
	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;

	/* One virtual channel carrying the sensor's Bayer stream */
	if (pad == 0) {
		fd->entry[0].pixelcode = port->format.code;
		fd->entry[0].bus.csi2.vc = 0;
		fd->entry[0].bus.csi2.dt =
			mtk_mbus_code_to_csi2_dt(port->format.code);
		fd->num_entries = 1;
	}

	return 0;
}

static const struct v4l2_subdev_pad_ops mtk_seninf_pad_ops = {
	.enum_mbus_code = mtk_seninf_enum_mbus_code,
	.enum_frame_size = mtk_seninf_enum_frame_size,
	.get_fmt = mtk_seninf_get_fmt,
	.set_fmt = mtk_seninf_set_fmt,
	.get_frame_desc = mtk_seninf_get_frame_desc,
};

static const struct v4l2_subdev_ops mtk_seninf_subdev_ops = {
	.pad = &mtk_seninf_pad_ops,
};

static const struct v4l2_subdev_internal_ops mtk_seninf_internal_ops = {
	.init_state = mtk_seninf_init_state,
};

/* --------------------------------------------------------------------------
 * Device tree parsing and registration
 * ------------------------------------------------------------------------ */

static int mtk_seninf_parse_port(struct mtk_cam_legacy *cam,
				 struct mtk_seninf_port *port,
				 struct fwnode_handle *child)
{
	struct v4l2_fwnode_endpoint vep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *ep;
	u32 reg;
	int ret;

	ret = fwnode_property_read_u32(child, "reg", &reg);
	if (ret)
		return -EINVAL;

	if (reg >= cam->soc->seninf_ports)
		return -EINVAL;

	port->index = reg;

	ep = fwnode_graph_get_next_endpoint(child, NULL);
	if (ep) {
		ret = v4l2_fwnode_endpoint_parse(ep, &vep);
		fwnode_handle_put(ep);
		if (ret)
			return ret;

		port->lanes = vep.bus.mipi_csi2.num_data_lanes;
		if (!port->lanes || port->lanes > SENINF_MAX_LANES)
			return -EINVAL;

		if (vep.nr_of_link_frequencies)
			port->link_freq_hz = vep.link_frequencies[0];
	}

	if (!port->lanes)
		port->lanes = 2;

	/* Optional per-board D-PHY settle time tweak, in nanoseconds */
	if (fwnode_property_read_u32(child, "mediatek,settle-ns",
				     &port->settle_ns))
		port->settle_ns = SENINF_DEFAULT_SETTLE_NS;

	return 0;
}

static int mtk_seninf_register_port(struct mtk_cam_legacy *cam,
				    struct mtk_seninf_port *port)
{
	int ret;

	port->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	port->format.width = 640;
	port->format.height = 480;
	port->format.field = V4L2_FIELD_NONE;
	port->format.colorspace = V4L2_COLORSPACE_RAW;
	port->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	port->format.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	port->format.xfer_func = V4L2_XFER_FUNC_NONE;

	v4l2_subdev_init(&port->sd, &mtk_seninf_subdev_ops);
	port->sd.internal_ops = &mtk_seninf_internal_ops;
	port->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	port->sd.owner = THIS_MODULE;
	port->sd.dev = cam->dev;
	snprintf(port->sd.name, sizeof(port->sd.name), "seninf-%u",
		 port->index);
	v4l2_set_subdevdata(&port->sd, port);

	port->pads[0].flags = MEDIA_PAD_FL_SINK;
	port->pads[1].flags = MEDIA_PAD_FL_SOURCE;
	port->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;

	ret = media_entity_pads_init(&port->sd.entity, 2, port->pads);
	if (ret)
		return ret;

	ret = v4l2_subdev_init_finalize(&port->sd);
	if (ret) {
		media_entity_cleanup(&port->sd.entity);
		return ret;
	}

	ret = v4l2_device_register_subdev(&cam->v4l2_dev, &port->sd);
	if (ret) {
		v4l2_subdev_cleanup(&port->sd);
		media_entity_cleanup(&port->sd.entity);
		return ret;
	}

	return 0;
}

/**
 * mtk_seninf_init - register one sub-device per CSI-2 port
 * @cam: camera subsystem instance
 *
 * The seninf node carries one child per CSI-2 port:
 *
 *   seninf@... {
 *           compatible = "mediatek,mt6735-seninf";
 *           #address-cells = <1>;
 *           #size-cells = <0>;
 *
 *           port@0 {
 *                   reg = <0>;
 *                   #address-cells = <1>;
 *                   #size-cells = <0>;
 *
 *                   seninf_in0: endpoint@0 {
 *                           reg = <0>;
 *                           data-lanes = <1 2 3 4>;
 *                           remote-endpoint = <&s5k4h8_out>;
 *                   };
 *                   seninf_out0: endpoint@1 {
 *                           reg = <1>;
 *                           remote-endpoint = <&camsv0_in>;
 *                   };
 *           };
 *   };
 */
int mtk_seninf_init(struct mtk_cam_legacy *cam)
{
	struct fwnode_handle *ports_node;
	unsigned int n = 0;

	ports_node = fwnode_get_named_child_node(dev_fwnode(cam->dev), "ports");
	if (!ports_node)
		ports_node = fwnode_handle_get(dev_fwnode(cam->dev));

	fwnode_for_each_child_node_scoped(ports_node, child) {
		u32 reg;

		if (!fwnode_name_eq(child, "port"))
			continue;

		if (fwnode_property_read_u32(child, "reg", &reg))
			continue;

		if (reg >= cam->soc->seninf_ports)
			continue;

		n++;
	}

	if (!n) {
		fwnode_handle_put(ports_node);
		return 0;
	}

	cam->ports = devm_kcalloc(cam->dev, n, sizeof(*cam->ports), GFP_KERNEL);
	if (!cam->ports) {
		fwnode_handle_put(ports_node);
		return -ENOMEM;
	}

	n = 0;
	fwnode_for_each_child_node_scoped(ports_node, child) {
		struct mtk_seninf_port *port = &cam->ports[n];
		int ret;

		if (!fwnode_name_eq(child, "port"))
			continue;

		port->cam = cam;
		mutex_init(&port->lock);

		ret = mtk_seninf_parse_port(cam, port, child);
		if (ret) {
			fwnode_handle_put(ports_node);
			return ret;
		}

		ret = mtk_seninf_register_port(cam, port);
		if (ret) {
			fwnode_handle_put(ports_node);
			return ret;
		}

		cam->pads += ARRAY_SIZE(port->pads);
		n++;
	}

	fwnode_handle_put(ports_node);
	cam->num_ports = n;

	return 0;
}

void mtk_seninf_cleanup(struct mtk_cam_legacy *cam)
{
	unsigned int i;

	for (i = 0; i < cam->num_ports; i++) {
		struct mtk_seninf_port *port = &cam->ports[i];

		v4l2_device_unregister_subdev(&port->sd);
		v4l2_subdev_cleanup(&port->sd);
		media_entity_cleanup(&port->sd.entity);
	}
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek legacy (MT6735/MT6755/MT6797) camera capture driver
 *
 * Copyright (C) 2026 Meizu fleet kernel port
 *
 * This is the top-level driver of the camera capture path: it maps the SENINF
 * (CSI-2 receiver) and CamSV (DMA capture) register windows described by the
 * device tree, registers the V4L2 and media devices, collects the sensors
 * through a V4L2 async notifier and builds the media graph
 *
 *   sensor ------> seninf port ------> camsv (/dev/videoN)
 *
 * See mtk-cam-legacy.h for the hardware model.
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>

#include "mtk-cam-legacy.h"

static const struct mtk_cam_legacy_soc mt6735_cam = {
	.name = "MT6735",
	.seninf_ports = 2,
	.rx_clk_rate = 384000000,
	.data_alignment = 16,
};

static const struct mtk_cam_legacy_soc mt6755_cam = {
	.name = "MT6755",
	.seninf_ports = 3,
	.rx_clk_rate = 384000000,
	.data_alignment = 16,
};

static const struct mtk_cam_legacy_soc mt6797_cam = {
	.name = "MT6797",
	.seninf_ports = 3,
	.rx_clk_rate = 546000000,
	.data_alignment = 16,
};

/* --------------------------------------------------------------------------
 * Media graph construction
 * ------------------------------------------------------------------------ */

/*
 * By the time the last sensor probes, the seninf ports and the CamSV video
 * nodes are registered, so the whole graph can be created and (if all
 * required sensors are present) registered with userspace.
 */
static bool mtk_cam_legacy_graph_is_complete(struct mtk_cam_legacy *cam)
{
	unsigned int i;

	if (cam->num_sensors < cam->num_ports)
		return false;

	for (i = 0; i < cam->num_ports; i++) {
		struct mtk_seninf_port *port = &cam->ports[i];

		if (!port->sensor_sd)
			return false;
		if (!port->camsv || !video_is_registered(&port->camsv->vdev))
			return false;
	}

	return cam->num_ports > 0;
}

static int mtk_cam_legacy_build_graph(struct mtk_cam_legacy *cam)
{
	unsigned int i;
	int ret;

	if (cam->graph_built)
		return 0;

	/*
	 * The video nodes must exist before the links are created, otherwise
	 * media_create_pad_link() has no entity to point at.
	 */
	ret = mtk_camsv_add_nodes(cam);
	if (ret)
		return ret;

	ret = media_device_register(&cam->media_dev);
	if (ret)
		goto err_nodes;

	ret = v4l2_device_register_subdev_nodes(&cam->v4l2_dev);
	if (ret)
		goto err_media;

	for (i = 0; i < cam->num_ports; i++) {
		struct mtk_seninf_port *port = &cam->ports[i];

		if (!port->sensor_sd) {
			dev_warn(cam->dev, "seninf-%u has no sensor\n",
				 port->index);
			continue;
		}

		ret = media_create_pad_link(&port->sensor_sd->entity, 0,
					    &port->sd.entity, 0,
					    MEDIA_LNK_FL_ENABLED |
					    MEDIA_LNK_FL_IMMUTABLE);
		if (ret) {
			dev_err(cam->dev, "failed to link seninf-%u: %d\n",
				port->index, ret);
			return ret;
		}

		if (!port->camsv) {
			dev_warn(cam->dev, "seninf-%u has no CamSV engine\n",
				 port->index);
			continue;
		}

		ret = media_create_pad_link(&port->sd.entity, 1,
					    &port->camsv->vdev.entity, 0,
					    MEDIA_LNK_FL_ENABLED |
					    MEDIA_LNK_FL_IMMUTABLE);
		if (ret) {
			dev_err(cam->dev, "failed to link camsv: %d\n", ret);
			return ret;
		}
	}

	dev_info(cam->dev,
		 "%s camera graph registered: %u seninf ports, %u CamSV engines\n",
		 cam->soc->name, cam->num_ports, cam->num_camsv);

	return 0;

err_media:
	media_device_unregister(&cam->media_dev);
err_nodes:
	mtk_camsv_remove_nodes(cam);
	return ret;
}

/*
 * Map a sensor sub-device to the seninf port it is wired to.  The port is
 * given by the parent port node's "reg" of the sensor's endpoint.
 */
static struct mtk_seninf_port *
mtk_cam_legacy_port_for_sensor(struct mtk_cam_legacy *cam,
			       struct v4l2_subdev *sd)
{
	struct fwnode_handle *ep;
	unsigned int i;

	if (!sd->fwnode)
		return NULL;

	fwnode_graph_for_each_endpoint(sd->fwnode, ep) {
		struct fwnode_handle *port_node, *remote;
		struct mtk_seninf_port *port = NULL;
		u32 reg = 0;

		remote = fwnode_graph_get_remote_endpoint(ep);
		if (!remote) {
			fwnode_handle_put(ep);
			continue;
		}

		port_node = fwnode_graph_get_port_parent(remote);
		fwnode_handle_put(remote);
		if (!port_node) {
			fwnode_handle_put(ep);
			continue;
		}

		fwnode_property_read_u32(port_node, "reg", &reg);
		fwnode_handle_put(port_node);

		for (i = 0; i < cam->num_ports; i++) {
			if (cam->ports[i].index == reg) {
				port = &cam->ports[i];
				break;
			}
		}

		if (port && !port->sensor_sd) {
			fwnode_handle_put(ep);
			return port;
		}

		fwnode_handle_put(ep);
	}

	return NULL;
}

static int mtk_cam_legacy_notifier_bound(struct v4l2_async_notifier *notifier,
					 struct v4l2_subdev *sd,
					 struct v4l2_async_connection *asc)
{
	struct mtk_cam_legacy *cam = container_of(notifier,
						  struct mtk_cam_legacy,
						  notifier);
	struct mtk_seninf_port *port = mtk_cam_legacy_port_for_sensor(cam, sd);

	if (!port) {
		dev_warn(cam->dev, "sensor %s is not connected to a seninf port\n",
			 sd->name);
		return -ENODEV;
	}

	port->sensor_sd = sd;
	cam->sensors[cam->num_sensors++] = sd;

	dev_dbg(cam->dev, "sensor %s bound to seninf-%u\n", sd->name,
		port->index);

	return 0;
}

static int mtk_cam_legacy_notifier_complete(struct v4l2_async_notifier *notifier)
{
	struct mtk_cam_legacy *cam = container_of(notifier,
						  struct mtk_cam_legacy,
						  notifier);

	guard(mutex)(&cam->lock);

	if (!mtk_cam_legacy_graph_is_complete(cam))
		return 0;

	return mtk_cam_legacy_build_graph(cam);
}

static const struct v4l2_async_notifier_operations mtk_cam_legacy_notifier_ops = {
	.bound = mtk_cam_legacy_notifier_bound,
	.complete = mtk_cam_legacy_notifier_complete,
};

/*
 * Only the sensor side of a seninf port carries a link frequency list in the
 * conventional way; use the CamSV/port links to find the sensors through our
 * own graph instead of walking the endpoints blindly.
 */
static int mtk_cam_legacy_register_sensors(struct mtk_cam_legacy *cam)
{
	unsigned int i;
	int ret;

	cam->notifier.ops = &mtk_cam_legacy_notifier_ops;
	v4l2_async_nf_init(&cam->notifier, &cam->v4l2_dev);

	for (i = 0; i < cam->num_ports; i++) {
		struct mtk_seninf_port *port = &cam->ports[i];
		struct v4l2_async_connection *asc;
		struct fwnode_handle *ep;

		ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(cam->dev),
						     port->index, 0,
						     FWNODE_GRAPH_DEVICE_DISABLED);
		if (!ep)
			continue;

		asc = v4l2_async_nf_add_fwnode_remote(&cam->notifier, ep,
						      struct v4l2_async_connection);
		fwnode_handle_put(ep);
		if (IS_ERR(asc)) {
			ret = PTR_ERR(asc);
			goto err_cleanup;
		}
	}

	ret = v4l2_async_nf_register(&cam->notifier);
	if (ret)
		goto err_cleanup;

	return 0;

err_cleanup:
	v4l2_async_nf_cleanup(&cam->notifier);
	return ret;
}

/* --------------------------------------------------------------------------
 * Probe / remove
 * ------------------------------------------------------------------------ */

static int mtk_cam_legacy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_cam_legacy *cam;
	int ret;

	cam = devm_kzalloc(dev, sizeof(*cam), GFP_KERNEL);
	if (!cam)
		return -ENOMEM;

	cam->dev = dev;
	cam->soc = of_device_get_match_data(dev);
	if (!cam->soc)
		return -ENODEV;

	mutex_init(&cam->lock);

	cam->seninf_base = devm_platform_ioremap_resource_byname(pdev,
								 "seninf");
	if (IS_ERR(cam->seninf_base))
		return PTR_ERR(cam->seninf_base);

	strscpy(cam->v4l2_dev.name, "mtk-cam-legacy",
		sizeof(cam->v4l2_dev.name));
	ret = v4l2_device_register(dev, &cam->v4l2_dev);
	if (ret)
		return ret;

	cam->media_dev.dev = dev;
	strscpy(cam->media_dev.model, "MediaTek camera",
		sizeof(cam->media_dev.model));
	snprintf(cam->media_dev.bus_info, sizeof(cam->media_dev.bus_info),
		 "platform:%s", dev_name(dev));
	cam->media_dev.hw_revision = 0;
	media_device_init(&cam->media_dev);
	cam->v4l2_dev.mdev = &cam->media_dev;

	ret = mtk_seninf_init(cam);
	if (ret)
		goto err_media;

	ret = mtk_camsv_init(cam);
	if (ret)
		goto err_seninf;

	ret = mtk_cam_legacy_register_sensors(cam);
	if (ret)
		goto err_camsv;

	platform_set_drvdata(pdev, cam);

	cam->sensors = devm_kcalloc(dev, cam->num_ports,
				    sizeof(*cam->sensors), GFP_KERNEL);
	if (!cam->sensors) {
		ret = -ENOMEM;
		goto err_nf;
	}

	dev_info(dev, "%s camera subsystem: %u seninf ports, %u CamSV engines\n",
		 cam->soc->name, cam->num_ports, cam->num_camsv);

	return 0;

err_nf:
	v4l2_async_nf_unregister(&cam->notifier);
	v4l2_async_nf_cleanup(&cam->notifier);
err_camsv:
	mtk_camsv_cleanup(cam);
err_seninf:
	mtk_seninf_cleanup(cam);
err_media:
	media_device_unregister(&cam->media_dev);
	media_device_cleanup(&cam->media_dev);
	v4l2_device_unregister(&cam->v4l2_dev);
	return ret;
}

static void mtk_cam_legacy_remove(struct platform_device *pdev)
{
	struct mtk_cam_legacy *cam = platform_get_drvdata(pdev);

	v4l2_async_nf_unregister(&cam->notifier);
	v4l2_async_nf_cleanup(&cam->notifier);

	mtk_camsv_remove_nodes(cam);
	mtk_camsv_cleanup(cam);
	mtk_seninf_cleanup(cam);

	media_device_unregister(&cam->media_dev);
	media_device_cleanup(&cam->media_dev);
	v4l2_device_unregister(&cam->v4l2_dev);
}

static const struct of_device_id mtk_cam_legacy_of_match[] = {
	{ .compatible = "mediatek,mt6735-camera", .data = &mt6735_cam },
	{ .compatible = "mediatek,mt6755-camera", .data = &mt6755_cam },
	{ .compatible = "mediatek,mt6797-camera", .data = &mt6797_cam },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_cam_legacy_of_match);

static struct platform_driver mtk_cam_legacy_driver = {
	.probe = mtk_cam_legacy_probe,
	.remove = mtk_cam_legacy_remove,
	.driver = {
		.name = "mtk-cam-legacy",
		.of_match_table = mtk_cam_legacy_of_match,
	},
};
module_platform_driver(mtk_cam_legacy_driver);

MODULE_DESCRIPTION("MediaTek legacy (MT6735/MT6755/MT6797) camera capture driver");
MODULE_AUTHOR("Meizu fleet kernel port");
MODULE_LICENSE("GPL");

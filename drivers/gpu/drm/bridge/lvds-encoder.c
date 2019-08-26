// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2016 Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 */

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>

#include <drm/drm_bridge.h>
#include <drm/drm_panel.h>

struct lvds_encoder {
	struct drm_bridge bridge;
	struct drm_bridge *panel_bridge;
	struct gpio_desc *powerdown_gpio;
	u32 output_fmt;
	u32 input_fmt;
};

static int lvds_encoder_attach(struct drm_bridge *bridge)
{
	struct lvds_encoder *lvds_encoder = container_of(bridge,
							 struct lvds_encoder,
							 bridge);

	return drm_bridge_attach(bridge->encoder, lvds_encoder->panel_bridge,
				 bridge);
}

static void lvds_encoder_enable(struct drm_bridge *bridge)
{
	struct lvds_encoder *lvds_encoder = container_of(bridge,
							 struct lvds_encoder,
							 bridge);

	if (lvds_encoder->powerdown_gpio)
		gpiod_set_value_cansleep(lvds_encoder->powerdown_gpio, 0);
}

static void lvds_encoder_disable(struct drm_bridge *bridge)
{
	struct lvds_encoder *lvds_encoder = container_of(bridge,
							 struct lvds_encoder,
							 bridge);

	if (lvds_encoder->powerdown_gpio)
		gpiod_set_value_cansleep(lvds_encoder->powerdown_gpio, 1);
}

static void lvds_atomic_get_input_bus_fmts(struct drm_bridge *bridge,
					   struct drm_bridge_state *bridge_state,
					   struct drm_crtc_state *crtc_state,
					   struct drm_connector_state *conn_state,
					   u32 output_fmt,
					   unsigned int *num_input_fmts,
					   u32 *input_fmts)
{
	struct lvds_encoder *lvds_encoder = container_of(bridge,
							 struct lvds_encoder,
							 bridge);

	if (output_fmt == MEDIA_BUS_FMT_FIXED ||
	    output_fmt == lvds_encoder->output_fmt)
		*num_input_fmts = 1;
	else
		*num_input_fmts = 0;

	if (*num_input_fmts && input_fmts)
		input_fmts[0] = lvds_encoder->input_fmt;
}

static int lvds_encoder_atomic_check(struct drm_bridge *bridge,
				     struct drm_bridge_state *bridge_state,
				     struct drm_crtc_state *crtc_state,
				     struct drm_connector_state *conn_state)
{
	/* Propagate the bus_flags. */
	bridge_state->input_bus_cfg.flags = bridge_state->output_bus_cfg.flags;
	return 0;
}

static struct drm_bridge_funcs funcs = {
	.attach = lvds_encoder_attach,
	.enable = lvds_encoder_enable,
	.disable = lvds_encoder_disable,
	.atomic_get_input_bus_fmts = lvds_atomic_get_input_bus_fmts,
	.atomic_check = lvds_encoder_atomic_check,
};

struct of_data_mapping {
	const char *name;
	u32 id;
};

static const struct of_data_mapping output_data_mappings[] = {
	{ "jeida-18", MEDIA_BUS_FMT_RGB666_1X7X3_SPWG },
	{ "jeida-24", MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA },
	{ "vesa-24", MEDIA_BUS_FMT_RGB888_1X7X4_SPWG },
};

static const struct of_data_mapping input_data_mappings[] = {
	{ "rgb-24", MEDIA_BUS_FMT_RGB888_1X24 },
	{ "rgb-18", MEDIA_BUS_FMT_RGB666_1X18 },
};

static int of_get_data_mapping(struct device_node *port,
			       const struct of_data_mapping *mappings,
			       unsigned int num_mappings,
			       u32 *fmt)
{
	const char *name = NULL;
	unsigned int i;

	of_property_read_string(port, "data-mapping", &name);
	if (!name) {
		*fmt = MEDIA_BUS_FMT_FIXED;
		return 0;
	}

	for (i = 0; i < num_mappings; i++) {
		if (!strcmp(mappings[i].name, name)) {
			*fmt = mappings[i].id;
			return 0;
		}
	}

	return -ENOTSUPP;
}

static int lvds_encoder_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *port;
	struct device_node *endpoint;
	struct device_node *panel_node;
	struct drm_panel *panel;
	struct lvds_encoder *lvds_encoder;
	int ret;

	lvds_encoder = devm_kzalloc(dev, sizeof(*lvds_encoder), GFP_KERNEL);
	if (!lvds_encoder)
		return -ENOMEM;

	lvds_encoder->input_fmt = MEDIA_BUS_FMT_FIXED;
	lvds_encoder->output_fmt = MEDIA_BUS_FMT_FIXED;

	lvds_encoder->powerdown_gpio = devm_gpiod_get_optional(dev, "powerdown",
							       GPIOD_OUT_HIGH);
	if (IS_ERR(lvds_encoder->powerdown_gpio)) {
		int err = PTR_ERR(lvds_encoder->powerdown_gpio);

		if (err != -EPROBE_DEFER)
			dev_err(dev, "powerdown GPIO failure: %d\n", err);
		return err;
	}

	port = of_graph_get_port_by_id(dev->of_node, 0);
	if (!port) {
		dev_dbg(dev, "port 0 not found\n");
		return -ENXIO;
	}

	ret = of_get_data_mapping(port, input_data_mappings,
				  ARRAY_SIZE(input_data_mappings),
				  &lvds_encoder->input_fmt);
	of_node_put(port);
	if (ret) {
		dev_dbg(dev, "unsupported input data-mapping\n");
		return ret;
	}

	/* Locate the panel DT node. */
	port = of_graph_get_port_by_id(dev->of_node, 1);
	if (!port) {
		dev_dbg(dev, "port 1 not found\n");
		return -ENXIO;
	}

	ret = of_get_data_mapping(port, output_data_mappings,
				  ARRAY_SIZE(output_data_mappings),
				  &lvds_encoder->output_fmt);
	if (ret) {
		of_node_put(port);
		dev_dbg(dev, "unsupported output data-mapping\n");
		return ret;
	}

	endpoint = of_get_child_by_name(port, "endpoint");
	of_node_put(port);
	if (!endpoint) {
		dev_dbg(dev, "no endpoint for port 1\n");
		return -ENXIO;
	}

	panel_node = of_graph_get_remote_port_parent(endpoint);
	of_node_put(endpoint);
	if (!panel_node) {
		dev_dbg(dev, "no remote endpoint for port 1\n");
		return -ENXIO;
	}

	panel = of_drm_find_panel(panel_node);
	of_node_put(panel_node);
	if (IS_ERR(panel)) {
		dev_dbg(dev, "panel not found, deferring probe\n");
		return PTR_ERR(panel);
	}

	lvds_encoder->panel_bridge =
		devm_drm_panel_bridge_add(dev, panel, DRM_MODE_CONNECTOR_LVDS);
	if (IS_ERR(lvds_encoder->panel_bridge))
		return PTR_ERR(lvds_encoder->panel_bridge);

	/* The panel_bridge bridge is attached to the panel's of_node,
	 * but we need a bridge attached to our of_node for our user
	 * to look up.
	 */
	lvds_encoder->bridge.of_node = dev->of_node;
	lvds_encoder->bridge.funcs = &funcs;
	drm_bridge_add(&lvds_encoder->bridge);

	platform_set_drvdata(pdev, lvds_encoder);

	return 0;
}

static int lvds_encoder_remove(struct platform_device *pdev)
{
	struct lvds_encoder *lvds_encoder = platform_get_drvdata(pdev);

	drm_bridge_remove(&lvds_encoder->bridge);

	return 0;
}

static const struct of_device_id lvds_encoder_match[] = {
	{ .compatible = "lvds-encoder" },
	{ .compatible = "thine,thc63lvdm83d" },
	{},
};
MODULE_DEVICE_TABLE(of, lvds_encoder_match);

static struct platform_driver lvds_encoder_driver = {
	.probe	= lvds_encoder_probe,
	.remove	= lvds_encoder_remove,
	.driver		= {
		.name		= "lvds-encoder",
		.of_match_table	= lvds_encoder_match,
	},
};
module_platform_driver(lvds_encoder_driver);

MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_DESCRIPTION("Transparent parallel to LVDS encoder");
MODULE_LICENSE("GPL");

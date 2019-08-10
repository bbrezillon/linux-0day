// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * based in parts on udlfb.c:
 * Copyright (C) 2009 Roberto De Ioris <roberto@unbit.it>
 * Copyright (C) 2009 Jaya Kumar <jayakumar.lkml@gmail.com>
 * Copyright (C) 2009 Bernie Thompson <bernie@plugable.com>
 */

#include <drm/drm_encoder.h>
#include <drm/drm_modeset_helper_vtables.h>

#include "udl_drv.h"

/* dummy encoder */
static void udl_enc_destroy(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
	kfree(encoder);
}

static const struct drm_encoder_funcs udl_enc_funcs = {
	.destroy = udl_enc_destroy,
};

struct drm_encoder *udl_encoder_init(struct drm_device *dev)
{
	struct drm_encoder *encoder;

	encoder = kzalloc(sizeof(struct drm_encoder), GFP_KERNEL);
	if (!encoder)
		return NULL;

	drm_encoder_init(dev, encoder, &udl_enc_funcs, DRM_MODE_ENCODER_TMDS,
			 NULL);
	encoder->possible_crtcs = 1;
	return encoder;
}

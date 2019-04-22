// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Collabora, Ltd.
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *	Alpha Lin <Alpha.Lin@rock-chips.com>
 *	Jeffy Chen <jeffy.chen@rock-chips.com>
 *
 * Copyright 2018 Google LLC.
 *	Tomasz Figa <tfiga@chromium.org>
 *
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2010-2011 Samsung Electronics Co., Ltd.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-sg.h>

#include "rockchip_vpu.h"
#include "rockchip_vpu_hw.h"
#include "rockchip_vpu_v4l2.h"

static const struct rockchip_vpu_fmt *
rockchip_vpu_get_formats(const struct rockchip_vpu_ctx *ctx,
			 unsigned int *num_fmts)
{
	const struct rockchip_vpu_fmt *formats;

	formats = ctx->dev->variant->enc_fmts;
	*num_fmts = ctx->dev->variant->num_enc_fmts;

	return formats;
}

static const struct rockchip_vpu_fmt *
rockchip_vpu_find_format(const struct rockchip_vpu_fmt *formats,
			 unsigned int num_fmts, u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < num_fmts; i++)
		if (formats[i].fourcc == fourcc)
			return &formats[i];
	return NULL;
}

static const struct rockchip_vpu_fmt *
rockchip_vpu_get_default_fmt(const struct rockchip_vpu_fmt *formats,
			     unsigned int num_fmts, bool bitstream)
{
	unsigned int i;

	for (i = 0; i < num_fmts; i++) {
		if (bitstream == (formats[i].codec_mode != RK_VPU_MODE_NONE))
			return &formats[i];
	}
	return NULL;
}

static int vidioc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct video_device *vfd = video_devdata(file);
	struct rockchip_vpu_dev *vpu = video_drvdata(file);

	strscpy(cap->driver, vpu->dev->driver->name, sizeof(cap->driver));
	strscpy(cap->card, vfd->name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform: %s",
		 vpu->dev->driver->name);
	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *priv,
				  struct v4l2_frmsizeenum *fsize)
{
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	const struct rockchip_vpu_fmt *formats, *fmt;
	unsigned int num_fmts;

	if (fsize->index != 0) {
		vpu_debug(0, "invalid frame size index (expected 0, got %d)\n",
			  fsize->index);
		return -EINVAL;
	}

	formats = rockchip_vpu_get_formats(ctx, &num_fmts);
	fmt = rockchip_vpu_find_format(formats, num_fmts, fsize->pixel_format);
	if (!fmt) {
		vpu_debug(0, "unsupported bitstream format (%08x)\n",
			  fsize->pixel_format);
		return -EINVAL;
	}

	/* This only makes sense for coded formats */
	if (fmt->codec_mode == RK_VPU_MODE_NONE)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise = fmt->frmsize;

	return 0;
}

static int vidioc_enum_fmt(struct file *file, void *priv,
			   struct v4l2_fmtdesc *f, bool capture)

{
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	const struct rockchip_vpu_fmt *fmt, *formats;
	unsigned int num_fmts, i, j = 0;
	bool skip_mode_none;

	/*
	 * When dealing with an encoder:
	 *  - on the capture side we want to filter out all MODE_NONE formats.
	 *  - on the output side we want to filter out all formats that are
	 *    not MODE_NONE.
	 * When dealing with a decoder:
	 *  - on the capture side we want to filter out all formats that are
	 *    not MODE_NONE.
	 *  - on the output side we want to filter out all MODE_NONE formats.
	 */
	skip_mode_none = capture == rockchip_vpu_is_encoder_ctx(ctx);

	formats = rockchip_vpu_get_formats(ctx, &num_fmts);
	for (i = 0; i < num_fmts; i++) {
		bool mode_none = formats[i].codec_mode == RK_VPU_MODE_NONE;

		if (skip_mode_none == mode_none)
			continue;
		if (j == f->index) {
			fmt = &formats[i];
			f->pixelformat = fmt->fourcc;
			return 0;
		}
		++j;
	}
	return -EINVAL;
}

static int vidioc_enum_fmt_vid_cap_mplane(struct file *file, void *priv,
					  struct v4l2_fmtdesc *f)
{
	return vidioc_enum_fmt(file, priv, f, true);
}

static int vidioc_enum_fmt_vid_out_mplane(struct file *file, void *priv,
					  struct v4l2_fmtdesc *f)
{
	return vidioc_enum_fmt(file, priv, f, false);
}

static int vidioc_g_fmt_out_mplane(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);

	vpu_debug(4, "f->type = %d\n", f->type);

	*pix_mp = ctx->src_fmt;

	return 0;
}

static int vidioc_g_fmt_cap_mplane(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);

	vpu_debug(4, "f->type = %d\n", f->type);

	*pix_mp = ctx->dst_fmt;

	return 0;
}

static int vidioc_try_fmt(struct file *file, void *priv, struct v4l2_format *f,
			  bool capture)
{
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct rockchip_vpu_fmt *formats, *fmt, *vpu_fmt;
	unsigned int num_fmts;
	bool coded;

	coded = capture == rockchip_vpu_is_encoder_ctx(ctx);

	vpu_debug(4, "trying format %c%c%c%c\n",
		  (pix_mp->pixelformat & 0x7f),
		  (pix_mp->pixelformat >> 8) & 0x7f,
		  (pix_mp->pixelformat >> 16) & 0x7f,
		  (pix_mp->pixelformat >> 24) & 0x7f);

	formats = rockchip_vpu_get_formats(ctx, &num_fmts);
	fmt = rockchip_vpu_find_format(formats, num_fmts, pix_mp->pixelformat);
	if (!fmt) {
		fmt = rockchip_vpu_get_default_fmt(formats, num_fmts, coded);
		f->fmt.pix.pixelformat = fmt->fourcc;
	}

	if (coded) {
		pix_mp->num_planes = 1;
		vpu_fmt = fmt;
	} else if (rockchip_vpu_is_encoder_ctx(ctx)) {
		vpu_fmt = ctx->vpu_dst_fmt;
	} else {
		vpu_fmt = ctx->vpu_src_fmt;
	}

	pix_mp->field = V4L2_FIELD_NONE;
	pix_mp->width = clamp(pix_mp->width,
			      vpu_fmt->frmsize.min_width,
			      vpu_fmt->frmsize.max_width);
	pix_mp->height = clamp(pix_mp->height,
			       vpu_fmt->frmsize.min_height,
			       vpu_fmt->frmsize.max_height);

	/* Round up to macroblocks. */
	pix_mp->width = round_up(pix_mp->width, vpu_fmt->frmsize.step_width);
	pix_mp->height = round_up(pix_mp->height, vpu_fmt->frmsize.step_height);

	if (!coded) {
		/* Fill remaining fields */
		v4l2_fill_pixfmt_mp(pix_mp, fmt->fourcc, pix_mp->width,
				    pix_mp->height);
	} else if (!pix_mp->plane_fmt[0].sizeimage) {
		/*
		 * For coded formats the application can specify
		 * sizeimage. If the application passes a zero sizeimage,
		 * let's default to the maximum frame size.
		 */
		pix_mp->plane_fmt[0].sizeimage = fmt->header_size +
			pix_mp->width * pix_mp->height * fmt->max_depth;
	}

	return 0;
}

static int vidioc_try_fmt_cap_mplane(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	return vidioc_try_fmt(file, priv, f, true);
}

static int vidioc_try_fmt_out_mplane(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	return vidioc_try_fmt(file, priv, f, false);
}

static void
rockchip_vpu_reset_fmt(struct v4l2_pix_format_mplane *fmt,
		       const struct rockchip_vpu_fmt *vpu_fmt)
{
	memset(fmt, 0, sizeof(*fmt));

	fmt->pixelformat = vpu_fmt->fourcc;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_JPEG,
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static void
rockchip_vpu_reset_encoded_fmt(struct rockchip_vpu_ctx *ctx)
{
	const struct rockchip_vpu_fmt *vpu_fmt, *formats;
	struct v4l2_pix_format_mplane *fmt;
	unsigned int num_fmts;

	formats = rockchip_vpu_get_formats(ctx, &num_fmts);
	vpu_fmt = rockchip_vpu_get_default_fmt(formats, num_fmts, true);

	if (rockchip_vpu_is_encoder_ctx(ctx)) {
		ctx->vpu_dst_fmt = vpu_fmt;
		fmt = &ctx->dst_fmt;
	} else {
		ctx->vpu_src_fmt = vpu_fmt;
		fmt = &ctx->src_fmt;
	}

	rockchip_vpu_reset_fmt(fmt, vpu_fmt);
	fmt->num_planes = 1;
	fmt->width = vpu_fmt->frmsize.min_width;
	fmt->height = vpu_fmt->frmsize.min_height;
	fmt->plane_fmt[0].sizeimage = vpu_fmt->header_size +
				fmt->width * fmt->height * vpu_fmt->max_depth;
}

static void
rockchip_vpu_reset_raw_fmt(struct rockchip_vpu_ctx *ctx)
{
	const struct rockchip_vpu_fmt *raw_vpu_fmt, *encoded_vpu_fmt, *formats;
	struct v4l2_pix_format_mplane *fmt;
	unsigned int num_fmts;

	formats = rockchip_vpu_get_formats(ctx, &num_fmts);
	raw_vpu_fmt = rockchip_vpu_get_default_fmt(formats, num_fmts, false);

	if (rockchip_vpu_is_encoder_ctx(ctx)) {
		ctx->vpu_src_fmt = raw_vpu_fmt;
		fmt = &ctx->src_fmt;
		encoded_vpu_fmt = ctx->vpu_dst_fmt;
	} else {
		ctx->vpu_dst_fmt = raw_vpu_fmt;
		fmt = &ctx->dst_fmt;
		encoded_vpu_fmt = ctx->vpu_src_fmt;
	}

	rockchip_vpu_reset_fmt(fmt, raw_vpu_fmt);
	v4l2_fill_pixfmt_mp(fmt, raw_vpu_fmt->fourcc,
			    encoded_vpu_fmt->frmsize.min_width,
			    encoded_vpu_fmt->frmsize.min_height);
}

void rockchip_vpu_reset_fmts(struct rockchip_vpu_ctx *ctx)
{
	rockchip_vpu_reset_encoded_fmt(ctx);
	rockchip_vpu_reset_raw_fmt(ctx);
}

static int
vidioc_s_fmt_out_mplane(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	const struct rockchip_vpu_fmt *formats;
	unsigned int num_fmts;
	struct vb2_queue *vq;
	int ret;

	/* Change not allowed if queue is streaming. */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_streaming(vq))
		return -EBUSY;

	if (!rockchip_vpu_is_encoder_ctx(ctx)) {
		struct vb2_queue *peer_vq;

		/*
		 * Since format change on the CAPTURE queue will reset
		 * the OUTPUT queue, we can't allow doing so
		 * when the OUTPUT queue has buffers allocated.
		 */
		peer_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
					  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		if (vb2_is_busy(peer_vq))
			return -EBUSY;
	}

	ret = vidioc_try_fmt_out_mplane(file, priv, f);
	if (ret)
		return ret;

	formats = rockchip_vpu_get_formats(ctx, &num_fmts);
	ctx->vpu_src_fmt = rockchip_vpu_find_format(formats, num_fmts,
						    pix_mp->pixelformat);
	ctx->src_fmt = *pix_mp;

	if (rockchip_vpu_is_encoder_ctx(ctx)) {
		/* Propagate to the CAPTURE format */
		ctx->dst_fmt.colorspace = pix_mp->colorspace;
		ctx->dst_fmt.ycbcr_enc = pix_mp->ycbcr_enc;
		ctx->dst_fmt.xfer_func = pix_mp->xfer_func;
		ctx->dst_fmt.quantization = pix_mp->quantization;
		ctx->dst_fmt.width = pix_mp->width;
		ctx->dst_fmt.height = pix_mp->height;
	} else {
		rockchip_vpu_reset_raw_fmt(ctx);
	}

	vpu_debug(0, "OUTPUT codec mode: %d\n", ctx->vpu_src_fmt->codec_mode);
	vpu_debug(0, "fmt - w: %d, h: %d\n",
		  pix_mp->width, pix_mp->height);
	return 0;
}

static int vidioc_s_fmt_cap_mplane(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	const struct rockchip_vpu_fmt *formats;
	struct vb2_queue *vq;
	unsigned int num_fmts;
	int ret;

	/* Change not allowed if queue is streaming. */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_streaming(vq))
		return -EBUSY;

	if (rockchip_vpu_is_encoder_ctx(ctx)) {
		struct vb2_queue *peer_vq;

		/*
		 * Since format change on the CAPTURE queue will reset
		 * the OUTPUT queue, we can't allow doing so
		 * when the OUTPUT queue has buffers allocated.
		 */
		peer_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
					  V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		if (vb2_is_busy(peer_vq) &&
		    (pix_mp->pixelformat != ctx->dst_fmt.pixelformat ||
		     pix_mp->height != ctx->dst_fmt.height ||
		     pix_mp->width != ctx->dst_fmt.width))
			return -EBUSY;
	}

	ret = vidioc_try_fmt_cap_mplane(file, priv, f);
	if (ret)
		return ret;

	formats = rockchip_vpu_get_formats(ctx, &num_fmts);
	ctx->vpu_dst_fmt = rockchip_vpu_find_format(formats, num_fmts,
						    pix_mp->pixelformat);
	ctx->dst_fmt = *pix_mp;

	vpu_debug(0, "CAPTURE codec mode: %d\n", ctx->vpu_dst_fmt->codec_mode);
	vpu_debug(0, "fmt - w: %d, h: %d\n",
		  pix_mp->width, pix_mp->height);

	/*
	 * Current raw format might have become invalid with newly
	 * selected codec, so reset it to default just to be safe and
	 * keep internal driver state sane. User is mandated to set
	 * the raw format again after we return, so we don't need
	 * anything smarter.
	 */
	if (rockchip_vpu_is_encoder_ctx(ctx)) {
		rockchip_vpu_reset_raw_fmt(ctx);
	} else {
		/* Propagate to the OUTPUT format */
		ctx->src_fmt.colorspace = pix_mp->colorspace;
		ctx->src_fmt.ycbcr_enc = pix_mp->ycbcr_enc;
		ctx->src_fmt.xfer_func = pix_mp->xfer_func;
		ctx->src_fmt.quantization = pix_mp->quantization;
		ctx->src_fmt.width = pix_mp->width;
		ctx->src_fmt.height = pix_mp->height;
	}

	return 0;
}

const struct v4l2_ioctl_ops rockchip_vpu_ioctl_ops = {
	.vidioc_querycap = vidioc_querycap,
	.vidioc_enum_framesizes = vidioc_enum_framesizes,

	.vidioc_try_fmt_vid_cap_mplane = vidioc_try_fmt_cap_mplane,
	.vidioc_try_fmt_vid_out_mplane = vidioc_try_fmt_out_mplane,
	.vidioc_s_fmt_vid_out_mplane = vidioc_s_fmt_out_mplane,
	.vidioc_s_fmt_vid_cap_mplane = vidioc_s_fmt_cap_mplane,
	.vidioc_g_fmt_vid_out_mplane = vidioc_g_fmt_out_mplane,
	.vidioc_g_fmt_vid_cap_mplane = vidioc_g_fmt_cap_mplane,
	.vidioc_enum_fmt_vid_out_mplane = vidioc_enum_fmt_vid_out_mplane,
	.vidioc_enum_fmt_vid_cap_mplane = vidioc_enum_fmt_vid_cap_mplane,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
};

static int
rockchip_vpu_queue_setup(struct vb2_queue *vq,
			 unsigned int *num_buffers,
			 unsigned int *num_planes,
			 unsigned int sizes[],
			 struct device *alloc_devs[])
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *pixfmt;
	int i;

	switch (vq->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		pixfmt = &ctx->dst_fmt;
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		pixfmt = &ctx->src_fmt;
		break;
	default:
		vpu_err("invalid queue type: %d\n", vq->type);
		return -EINVAL;
	}

	if (*num_planes) {
		if (*num_planes != pixfmt->num_planes)
			return -EINVAL;
		for (i = 0; i < pixfmt->num_planes; ++i)
			if (sizes[i] < pixfmt->plane_fmt[i].sizeimage)
				return -EINVAL;
		return 0;
	}

	*num_planes = pixfmt->num_planes;
	for (i = 0; i < pixfmt->num_planes; ++i)
		sizes[i] = pixfmt->plane_fmt[i].sizeimage;
	return 0;
}

static int
rockchip_vpu_buf_plane_check(struct vb2_buffer *vb,
			     const struct rockchip_vpu_fmt *vpu_fmt,
			     struct v4l2_pix_format_mplane *pixfmt)
{
	unsigned int sz;
	int i;

	for (i = 0; i < pixfmt->num_planes; ++i) {
		sz = pixfmt->plane_fmt[i].sizeimage;
		vpu_debug(4, "plane %d size: %ld, sizeimage: %u\n",
			  i, vb2_plane_size(vb, i), sz);
		if (vb2_plane_size(vb, i) < sz) {
			vpu_err("plane %d is too small for output\n", i);
			return -EINVAL;
		}
	}
	return 0;
}

static int rockchip_vpu_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vq);

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		return rockchip_vpu_buf_plane_check(vb, ctx->vpu_src_fmt,
						    &ctx->src_fmt);

	return rockchip_vpu_buf_plane_check(vb, ctx->vpu_dst_fmt,
					    &ctx->dst_fmt);
}

static void rockchip_vpu_buf_queue(struct vb2_buffer *vb)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static bool rockchip_vpu_vq_is_coded(struct vb2_queue *q)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(q);

	return rockchip_vpu_is_encoder_ctx(ctx) != V4L2_TYPE_IS_OUTPUT(q->type);
}

static int rockchip_vpu_start_streaming(struct vb2_queue *q,
					unsigned int count)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(q);
	int ret = 0;

	if (V4L2_TYPE_IS_OUTPUT(q->type))
		ctx->sequence_out = 0;
	else
		ctx->sequence_cap = 0;

	if (rockchip_vpu_vq_is_coded(q)) {
		enum rockchip_vpu_codec_mode codec_mode;

		if (V4L2_TYPE_IS_OUTPUT(q->type))
			codec_mode = ctx->vpu_src_fmt->codec_mode;
		else
			codec_mode = ctx->vpu_dst_fmt->codec_mode;

		vpu_debug(4, "Codec mode = %d\n", codec_mode);
		ctx->codec_ops = &ctx->dev->variant->codec_ops[codec_mode];
		if (ctx->codec_ops && ctx->codec_ops->init)
			ret = ctx->codec_ops->init(ctx);
	}

	return ret;
}

static void
rockchip_vpu_return_bufs(struct vb2_queue *q,
			 struct vb2_v4l2_buffer *(*buf_remove)(struct v4l2_m2m_ctx *))
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(q);

	for (;;) {
		struct vb2_v4l2_buffer *vbuf;

		vbuf = buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_ctrl_request_complete(vbuf->vb2_buf.req_obj.req,
					   &ctx->ctrl_handler);
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

static void rockchip_vpu_stop_streaming(struct vb2_queue *q)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(q);

	if (rockchip_vpu_vq_is_coded(q)) {
		if (ctx->codec_ops && ctx->codec_ops->exit)
			ctx->codec_ops->exit(ctx);
	}

	/*
	 * The mem2mem framework calls v4l2_m2m_cancel_job before
	 * .stop_streaming, so there isn't any job running and
	 * it is safe to return all the buffers.
	 */
	if (V4L2_TYPE_IS_OUTPUT(q->type))
		rockchip_vpu_return_bufs(q, v4l2_m2m_src_buf_remove);
	else
		rockchip_vpu_return_bufs(q, v4l2_m2m_dst_buf_remove);
}

static void rockchip_vpu_buf_request_complete(struct vb2_buffer *vb)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->ctrl_handler);
}

static int rockchip_vpu_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	vbuf->field = V4L2_FIELD_NONE;
	return 0;
}

const struct vb2_ops rockchip_vpu_queue_ops = {
	.queue_setup = rockchip_vpu_queue_setup,
	.buf_prepare = rockchip_vpu_buf_prepare,
	.buf_queue = rockchip_vpu_buf_queue,
	.buf_out_validate = rockchip_vpu_buf_out_validate,
	.buf_request_complete = rockchip_vpu_buf_request_complete,
	.start_streaming = rockchip_vpu_start_streaming,
	.stop_streaming = rockchip_vpu_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

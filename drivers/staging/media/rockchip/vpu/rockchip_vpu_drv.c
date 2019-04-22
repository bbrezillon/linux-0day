// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Collabora, Ltd.
 * Copyright 2018 Google LLC.
 *	Tomasz Figa <tfiga@chromium.org>
 *
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-vmalloc.h>

#include "rockchip_vpu_v4l2.h"
#include "rockchip_vpu.h"
#include "rockchip_vpu_hw.h"

#define DRIVER_NAME "rockchip-vpu"

int rockchip_vpu_debug;
module_param_named(debug, rockchip_vpu_debug, int, 0644);
MODULE_PARM_DESC(debug,
		 "Debug level - higher value produces more verbose messages");

static int
rockchip_vpu_enc_buf_finish(struct rockchip_vpu_ctx *ctx,
			    struct vb2_buffer *buf,
			    unsigned int bytesused)
{
	size_t avail_size;

	avail_size = vb2_plane_size(buf, 0) - ctx->vpu_dst_fmt->header_size;
	if (bytesused > avail_size)
		return -EINVAL;
	/*
	 * The bounce buffer is only for the JPEG encoder.
	 * TODO: Rework the JPEG encoder to eliminate the need
	 * for a bounce buffer.
	 */
	if (ctx->jpeg_enc.bounce_buffer.cpu) {
		memcpy(vb2_plane_vaddr(buf, 0) +
		       ctx->vpu_dst_fmt->header_size,
		       ctx->jpeg_enc.bounce_buffer.cpu, bytesused);
	}
	buf->planes[0].bytesused =
		ctx->vpu_dst_fmt->header_size + bytesused;
	return 0;
}

static int
rockchip_vpu_dec_buf_finish(struct rockchip_vpu_ctx *ctx,
			    struct vb2_buffer *buf,
			    unsigned int bytesused)
{
	/* For decoders set bytesused as per the output picture. */
	buf->planes[0].bytesused = ctx->dst_fmt.plane_fmt[0].sizeimage;
	return 0;
}

static void rockchip_vpu_job_finish(struct rockchip_vpu_dev *vpu,
				    struct rockchip_vpu_ctx *ctx,
				    unsigned int bytesused,
				    enum vb2_buffer_state result)
{
	struct vb2_v4l2_buffer *src, *dst;
	int ret;

	pm_runtime_mark_last_busy(vpu->dev);
	pm_runtime_put_autosuspend(vpu->dev);
	clk_bulk_disable(vpu->variant->num_clocks, vpu->clocks);

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

	if (WARN_ON(!src))
		return;
	if (WARN_ON(!dst))
		return;

	src->sequence = ctx->sequence_out++;
	dst->sequence = ctx->sequence_cap++;

	v4l2_m2m_buf_copy_metadata(src, dst, true);

	ret = ctx->buf_finish(ctx, &dst->vb2_buf, bytesused);
	if (ret)
		result = VB2_BUF_STATE_ERROR;

	v4l2_m2m_buf_done(src, result);
	v4l2_m2m_buf_done(dst, result);

	v4l2_m2m_job_finish(vpu->m2m_dev, ctx->fh.m2m_ctx);
}

void rockchip_vpu_irq_done(struct rockchip_vpu_dev *vpu,
			   unsigned int bytesused,
			   enum vb2_buffer_state result)
{
	struct rockchip_vpu_ctx *ctx =
		v4l2_m2m_get_curr_priv(vpu->m2m_dev);

	/*
	 * If cancel_delayed_work returns false
	 * the timeout expired. The watchdog is running,
	 * and will take care of finishing the job.
	 */
	if (cancel_delayed_work(&vpu->watchdog_work))
		rockchip_vpu_job_finish(vpu, ctx, bytesused, result);
}

void rockchip_vpu_watchdog(struct work_struct *work)
{
	struct rockchip_vpu_dev *vpu;
	struct rockchip_vpu_ctx *ctx;

	vpu = container_of(to_delayed_work(work),
			   struct rockchip_vpu_dev, watchdog_work);
	ctx = v4l2_m2m_get_curr_priv(vpu->m2m_dev);
	if (ctx) {
		vpu_err("frame processing timed out!\n");
		ctx->codec_ops->reset(ctx);
		rockchip_vpu_job_finish(vpu, ctx, 0, VB2_BUF_STATE_ERROR);
	}
}

static void device_run(void *priv)
{
	struct rockchip_vpu_ctx *ctx = priv;
	int ret;

	ret = clk_bulk_enable(ctx->dev->variant->num_clocks, ctx->dev->clocks);
	if (ret)
		goto err_cancel_job;
	ret = pm_runtime_get_sync(ctx->dev->dev);
	if (ret < 0)
		goto err_cancel_job;

	ctx->codec_ops->run(ctx);
	return;

err_cancel_job:
	rockchip_vpu_job_finish(ctx->dev, ctx, 0, VB2_BUF_STATE_ERROR);
}

bool rockchip_vpu_is_encoder_ctx(const struct rockchip_vpu_ctx *ctx)
{
	return ctx->buf_finish == rockchip_vpu_enc_buf_finish;
}

static struct v4l2_m2m_ops vpu_m2m_ops = {
	.device_run = device_run,
};

static int
queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	struct rockchip_vpu_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->ops = &rockchip_vpu_queue_ops;
	src_vq->mem_ops = &vb2_dma_contig_memops;

	/*
	 * Driver does mostly sequential access, so sacrifice TLB efficiency
	 * for faster allocation. Also, no CPU access on the source queue,
	 * so no kernel mapping needed.
	 */
	src_vq->dma_attrs = DMA_ATTR_ALLOC_SINGLE_PAGES |
			    DMA_ATTR_NO_KERNEL_MAPPING;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->vpu_mutex;
	src_vq->dev = ctx->dev->v4l2_dev.dev;
	src_vq->supports_requests = true;

	if (!rockchip_vpu_is_encoder_ctx(ctx))
		src_vq->requires_requests = true;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	/*
	 * When encoding, the CAPTURE queue doesn't need dma memory,
	 * as the CPU needs to create the JPEG frames, from the
	 * hardware-produced JPEG payload.
	 *
	 * For the DMA destination buffer, we use a bounce buffer.
	 */
	if (rockchip_vpu_is_encoder_ctx(ctx)) {
		dst_vq->mem_ops = &vb2_vmalloc_memops;
	} else {
		dst_vq->bidirectional = true;
		dst_vq->mem_ops = &vb2_dma_contig_memops;
		dst_vq->dma_attrs = DMA_ATTR_ALLOC_SINGLE_PAGES |
				    DMA_ATTR_NO_KERNEL_MAPPING;
	}

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &rockchip_vpu_queue_ops;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->vpu_mutex;
	dst_vq->dev = ctx->dev->v4l2_dev.dev;

	return vb2_queue_init(dst_vq);
}

static int rockchip_vpu_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct rockchip_vpu_ctx *ctx;

	ctx = container_of(ctrl->handler,
			   struct rockchip_vpu_ctx, ctrl_handler);

	vpu_debug(1, "s_ctrl: id = %d, val = %d\n", ctrl->id, ctrl->val);

	switch (ctrl->id) {
	case V4L2_CID_JPEG_COMPRESSION_QUALITY:
		ctx->jpeg_quality = ctrl->val;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops rockchip_vpu_ctrl_ops = {
	.s_ctrl = rockchip_vpu_s_ctrl,
};

static int rockchip_vpu_ctrls_setup(struct rockchip_vpu_dev *vpu,
				    struct rockchip_vpu_ctx *ctx)
{
	v4l2_ctrl_handler_init(&ctx->ctrl_handler, 1);
	if (vpu->variant->codec & RK_VPU_CODEC_JPEG) {
		v4l2_ctrl_new_std(&ctx->ctrl_handler, &rockchip_vpu_ctrl_ops,
				  V4L2_CID_JPEG_COMPRESSION_QUALITY,
				  5, 100, 1, 50);
		if (ctx->ctrl_handler.error) {
			vpu_err("Adding JPEG control failed %d\n",
				ctx->ctrl_handler.error);
			v4l2_ctrl_handler_free(&ctx->ctrl_handler);
			return ctx->ctrl_handler.error;
		}
	}

	return v4l2_ctrl_handler_setup(&ctx->ctrl_handler);
}

/*
 * V4L2 file operations.
 */

static int rockchip_vpu_open(struct file *filp)
{
	struct rockchip_vpu_dev *vpu = video_drvdata(filp);
	struct video_device *vdev = video_devdata(filp);
	struct rockchip_vpu_ctx *ctx;
	int ret;

	/*
	 * We do not need any extra locking here, because we operate only
	 * on local data here, except reading few fields from dev, which
	 * do not change through device's lifetime (which is guaranteed by
	 * reference on module from open()) and V4L2 internal objects (such
	 * as vdev and ctx->fh), which have proper locking done in respective
	 * helper functions used here.
	 */

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = vpu;
	if (vdev == vpu->vfd_enc) {
		ctx->buf_finish = rockchip_vpu_enc_buf_finish;
		ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(vpu->m2m_dev, ctx,
						    queue_init);
	} else if (vdev == vpu->vfd_dec) {
		ctx->buf_finish = rockchip_vpu_dec_buf_finish;
		ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(vpu->m2m_dev, ctx,
						    queue_init);
	} else {
		ctx->fh.m2m_ctx = ERR_PTR(-ENODEV);
	}
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		kfree(ctx);
		return ret;
	}

	v4l2_fh_init(&ctx->fh, vdev);
	filp->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);

	rockchip_vpu_reset_fmts(ctx);

	ret = rockchip_vpu_ctrls_setup(vpu, ctx);
	if (ret) {
		vpu_err("Failed to set up controls\n");
		goto err_fh_free;
	}
	ctx->fh.ctrl_handler = &ctx->ctrl_handler;

	return 0;

err_fh_free:
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int rockchip_vpu_release(struct file *filp)
{
	struct rockchip_vpu_ctx *ctx =
		container_of(filp->private_data, struct rockchip_vpu_ctx, fh);

	/*
	 * No need for extra locking because this was the last reference
	 * to this file.
	 */
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations rockchip_vpu_fops = {
	.owner = THIS_MODULE,
	.open = rockchip_vpu_open,
	.release = rockchip_vpu_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static const struct of_device_id of_rockchip_vpu_match[] = {
	{ .compatible = "rockchip,rk3399-vpu", .data = &rk3399_vpu_variant, },
	{ .compatible = "rockchip,rk3288-vpu", .data = &rk3288_vpu_variant, },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_rockchip_vpu_match);

static const struct media_device_ops rockchip_m2m_media_ops = {
	.req_validate = vb2_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

static int rockchip_vpu_video_device_register(struct rockchip_vpu_dev *vpu,
					      enum rockchip_vpu_type type)
{
	const struct of_device_id *match;
	struct video_device *vfd;
	int ret;

	match = of_match_node(of_rockchip_vpu_match, vpu->dev->of_node);
	vfd = video_device_alloc();
	if (!vfd) {
		v4l2_err(&vpu->v4l2_dev, "Failed to allocate video device\n");
		return -ENOMEM;
	}

	vfd->fops = &rockchip_vpu_fops;
	vfd->release = video_device_release;
	vfd->lock = &vpu->vpu_mutex;
	vfd->v4l2_dev = &vpu->v4l2_dev;
	vfd->vfl_dir = VFL_DIR_M2M;
	vfd->device_caps = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_M2M_MPLANE;
	vfd->ioctl_ops = &rockchip_vpu_ioctl_ops;
	snprintf(vfd->name, sizeof(vfd->name), "%s-%s", match->compatible,
		 type == RK_VPU_ENCODER ? "enc" : "dec");

	if (type == RK_VPU_ENCODER)
		vpu->vfd_enc = vfd;
	else
		vpu->vfd_dec = vfd;
	video_set_drvdata(vfd, vpu);

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, 0);
	if (ret) {
		v4l2_err(&vpu->v4l2_dev, "Failed to register video device\n");
		goto err_free_dev;
	}
	v4l2_info(&vpu->v4l2_dev, "registered as /dev/video%d\n", vfd->num);

	return 0;
err_free_dev:
	video_device_release(vfd);
	return ret;
}

static int rockchip_vpu_register_entity(struct media_device *mdev,
					struct media_entity *entity,
					const char *entity_name,
					struct media_pad *pads, int num_pads,
					int function,
	struct video_device *vdev)
{
	char *name;
	int ret;

	entity->obj_type = MEDIA_ENTITY_TYPE_BASE;
	if (function == MEDIA_ENT_F_IO_V4L) {
		entity->info.dev.major = VIDEO_MAJOR;
		entity->info.dev.minor = vdev->minor;
	}
	name = kasprintf(GFP_KERNEL, "%s-%s", vdev->name, entity_name);
	if (!name)
		return -ENOMEM;
	entity->name = name;
	entity->function = function;

	ret = media_entity_pads_init(entity, num_pads, pads);
	if (ret)
		goto err_free_name;
	ret = media_device_register_entity(mdev, entity);
	if (ret)
		goto err_free_name;

	return 0;

err_free_name:
	kfree(name);
	return ret;
}

static int rockchip_register_mc(struct media_device *mdev,
				struct rockchip_vpu_mc *mc,
				struct video_device *vdev,
				int function)
{
	struct media_link *link;
	int ret;

	/* Create the three encoder entities with their pads */
	mc->source = &vdev->entity;
	mc->source_pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = rockchip_vpu_register_entity(mdev, mc->source, "source",
					   &mc->source_pad, 1,
					   MEDIA_ENT_F_IO_V4L, vdev);
	if (ret)
		return ret;

	mc->proc_pads[0].flags = MEDIA_PAD_FL_SINK;
	mc->proc_pads[1].flags = MEDIA_PAD_FL_SOURCE;
	ret = rockchip_vpu_register_entity(mdev, &mc->proc, "proc",
					   mc->proc_pads, 2, function, vdev);
	if (ret)
		goto err_rel_entity0;

	mc->sink_pad.flags = MEDIA_PAD_FL_SINK;
	ret = rockchip_vpu_register_entity(mdev, &mc->sink, "sink",
					   &mc->sink_pad, 1, MEDIA_ENT_F_IO_V4L,
					   vdev);
	if (ret)
		goto err_rel_entity1;

	/* Connect the three entities */
	ret = media_create_pad_link(mc->source, 0, &mc->proc, 1,
				    MEDIA_LNK_FL_IMMUTABLE |
				    MEDIA_LNK_FL_ENABLED);
	if (ret)
		goto err_rel_entity2;

	ret = media_create_pad_link(&mc->proc, 0, &mc->sink, 0,
				    MEDIA_LNK_FL_IMMUTABLE |
				    MEDIA_LNK_FL_ENABLED);
	if (ret)
		goto err_rm_links0;

	/* Create video interface */
	mc->intf_devnode = media_devnode_create(mdev, MEDIA_INTF_T_V4L_VIDEO,
						0, VIDEO_MAJOR, vdev->minor);
	if (!mc->intf_devnode) {
		ret = -ENOMEM;
		goto err_rm_links1;
	}

	/* Connect the two DMA engines to the interface */
	link = media_create_intf_link(mc->source, &mc->intf_devnode->intf,
				      MEDIA_LNK_FL_IMMUTABLE |
				      MEDIA_LNK_FL_ENABLED);
	if (!link) {
		ret = -ENOMEM;
		goto err_rm_devnode;
	}

	link = media_create_intf_link(&mc->sink, &mc->intf_devnode->intf,
				      MEDIA_LNK_FL_IMMUTABLE |
				      MEDIA_LNK_FL_ENABLED);
	if (!link) {
		ret = -ENOMEM;
		goto err_rm_devnode;
	}
	return 0;

err_rm_devnode:
	media_devnode_remove(mc->intf_devnode);
err_rm_links1:
	media_entity_remove_links(&mc->sink);
err_rm_links0:
	media_entity_remove_links(&mc->proc);
	media_entity_remove_links(mc->source);
err_rel_entity2:
	media_device_unregister_entity(&mc->proc);
	kfree(mc->proc.name);
err_rel_entity1:
	media_device_unregister_entity(&mc->sink);
	kfree(mc->sink.name);
err_rel_entity0:
	media_device_unregister_entity(mc->source);
	kfree(mc->source->name);
	return ret;
}

static void rockchip_unregister_mc(struct rockchip_vpu_mc *mc)
{
	media_devnode_remove(mc->intf_devnode);
	media_entity_remove_links(mc->source);
	media_entity_remove_links(&mc->sink);
	media_entity_remove_links(&mc->proc);
	media_device_unregister_entity(mc->source);
	media_device_unregister_entity(&mc->sink);
	media_device_unregister_entity(&mc->proc);
	kfree(mc->source->name);
	kfree(mc->sink.name);
	kfree(mc->proc.name);
}

static int rockchip_register_media_controller(struct rockchip_vpu_dev *vpu)
{
	int ret;

	/*
	 * We have one memory-to-memory device, to hold a single queue
	 * of memory-to-memory serialized jobs.
	 * There is a set of pads and processing entities for the encoder,
	 * and another set for the decoder.
	 * Also, there are two V4L interface, one for each set of entities.
	 */

	if (vpu->vfd_enc) {
		ret = rockchip_register_mc(&vpu->mdev, &vpu->mc[0],
					   vpu->vfd_enc,
					   MEDIA_ENT_F_PROC_VIDEO_ENCODER);
		if (ret)
			return ret;
	}

	if (vpu->vfd_dec) {
		ret = rockchip_register_mc(&vpu->mdev, &vpu->mc[1],
					   vpu->vfd_dec,
					   MEDIA_ENT_F_PROC_VIDEO_DECODER);
		if (ret) {
			rockchip_unregister_mc(&vpu->mc[0]);
			return ret;
		}
	}

	return 0;
}

static int rockchip_vpu_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct rockchip_vpu_dev *vpu;
	struct resource *res;
	int i, ret;

	vpu = devm_kzalloc(&pdev->dev, sizeof(*vpu), GFP_KERNEL);
	if (!vpu)
		return -ENOMEM;

	vpu->dev = &pdev->dev;
	vpu->pdev = pdev;
	mutex_init(&vpu->vpu_mutex);
	spin_lock_init(&vpu->irqlock);

	match = of_match_node(of_rockchip_vpu_match, pdev->dev.of_node);
	vpu->variant = match->data;

	INIT_DELAYED_WORK(&vpu->watchdog_work, rockchip_vpu_watchdog);

	for (i = 0; i < vpu->variant->num_clocks; i++)
		vpu->clocks[i].id = vpu->variant->clk_names[i];
	ret = devm_clk_bulk_get(&pdev->dev, vpu->variant->num_clocks,
				vpu->clocks);
	if (ret)
		return ret;

	res = platform_get_resource(vpu->pdev, IORESOURCE_MEM, 0);
	vpu->base = devm_ioremap_resource(vpu->dev, res);
	if (IS_ERR(vpu->base))
		return PTR_ERR(vpu->base);
	vpu->enc_base = vpu->base + vpu->variant->enc_offset;
	vpu->dec_base = vpu->base + vpu->variant->dec_offset;

	ret = dma_set_coherent_mask(vpu->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(vpu->dev, "Could not set DMA coherent mask.\n");
		return ret;
	}

	if (vpu->variant->vdpu_irq) {
		int irq;

		irq = platform_get_irq_byname(vpu->pdev, "vdpu");
		if (irq <= 0) {
			dev_err(vpu->dev, "Could not get vdpu IRQ.\n");
			return -ENXIO;
		}

		ret = devm_request_irq(vpu->dev, irq, vpu->variant->vdpu_irq,
				       0, dev_name(vpu->dev), vpu);
		if (ret) {
			dev_err(vpu->dev, "Could not request vdpu IRQ.\n");
			return ret;
		}
	}

	if (vpu->variant->vepu_irq) {
		int irq;

		irq = platform_get_irq_byname(vpu->pdev, "vepu");
		if (irq <= 0) {
			dev_err(vpu->dev, "Could not get vepu IRQ.\n");
			return -ENXIO;
		}

		ret = devm_request_irq(vpu->dev, irq, vpu->variant->vepu_irq,
				       0, dev_name(vpu->dev), vpu);
		if (ret) {
			dev_err(vpu->dev, "Could not request vepu IRQ.\n");
			return ret;
		}
	}

	ret = vpu->variant->init(vpu);
	if (ret) {
		dev_err(&pdev->dev, "Failed to init VPU hardware\n");
		return ret;
	}

	pm_runtime_set_autosuspend_delay(vpu->dev, 100);
	pm_runtime_use_autosuspend(vpu->dev);
	pm_runtime_enable(vpu->dev);

	ret = clk_bulk_prepare(vpu->variant->num_clocks, vpu->clocks);
	if (ret) {
		dev_err(&pdev->dev, "Failed to prepare clocks\n");
		return ret;
	}

	ret = v4l2_device_register(&pdev->dev, &vpu->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register v4l2 device\n");
		goto err_clk_unprepare;
	}
	platform_set_drvdata(pdev, vpu);

	vpu->m2m_dev = v4l2_m2m_init(&vpu_m2m_ops);
	if (IS_ERR(vpu->m2m_dev)) {
		v4l2_err(&vpu->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(vpu->m2m_dev);
		goto err_v4l2_unreg;
	}

	vpu->mdev.dev = vpu->dev;
	strscpy(vpu->mdev.model, DRIVER_NAME, sizeof(vpu->mdev.model));
	media_device_init(&vpu->mdev);
	vpu->mdev.ops = &rockchip_m2m_media_ops;
	vpu->v4l2_dev.mdev = &vpu->mdev;

	if (vpu->variant->enc_fmts) {
		ret = rockchip_vpu_video_device_register(vpu, RK_VPU_ENCODER);
		if (ret) {
			dev_err(&pdev->dev, "Failed to register encoder\n");
			goto err_m2m_enc_rel;
		}
	}

	if (vpu->variant->dec_fmts) {
		ret = rockchip_vpu_video_device_register(vpu, RK_VPU_DECODER);
		if (ret) {
			dev_err(&pdev->dev, "Failed to register decoder\n");
			goto err_video_dev_unreg;
		}
	}

	ret = rockchip_register_media_controller(vpu);
	if (ret) {
		v4l2_err(&vpu->v4l2_dev, "Failed to register media controller\n");
		goto err_video_dev_unreg;
	}

	ret = media_device_register(&vpu->mdev);
	if (ret) {
		v4l2_err(&vpu->v4l2_dev, "Failed to register mem2mem media device\n");
		goto err_mc_unreg;
	}
	return 0;
err_mc_unreg:
	if (vpu->vfd_dec)
		rockchip_unregister_mc(&vpu->mc[1]);
	if (vpu->vfd_enc)
		rockchip_unregister_mc(&vpu->mc[0]);
err_video_dev_unreg:
	if (vpu->vfd_dec) {
		video_unregister_device(vpu->vfd_dec);
		video_device_release(vpu->vfd_dec);
	}
	if (vpu->vfd_enc) {
		video_unregister_device(vpu->vfd_enc);
		video_device_release(vpu->vfd_enc);
	}
err_m2m_enc_rel:
	v4l2_m2m_release(vpu->m2m_dev);
err_v4l2_unreg:
	v4l2_device_unregister(&vpu->v4l2_dev);
err_clk_unprepare:
	clk_bulk_unprepare(vpu->variant->num_clocks, vpu->clocks);
	pm_runtime_disable(vpu->dev);
	return ret;
}

static int rockchip_vpu_remove(struct platform_device *pdev)
{
	struct rockchip_vpu_dev *vpu = platform_get_drvdata(pdev);

	v4l2_info(&vpu->v4l2_dev, "Removing %s\n", pdev->name);

	media_device_unregister(&vpu->mdev);
	v4l2_m2m_release(vpu->m2m_dev);
	media_device_cleanup(&vpu->mdev);
	if (vpu->vfd_enc) {
		rockchip_unregister_mc(&vpu->mc[0]);
		video_unregister_device(vpu->vfd_enc);
		video_device_release(vpu->vfd_enc);
	}
	if (vpu->vfd_dec) {
		rockchip_unregister_mc(&vpu->mc[1]);
		video_unregister_device(vpu->vfd_dec);
		video_device_release(vpu->vfd_dec);
	}
	v4l2_device_unregister(&vpu->v4l2_dev);
	clk_bulk_unprepare(vpu->variant->num_clocks, vpu->clocks);
	pm_runtime_disable(vpu->dev);
	return 0;
}

static const struct dev_pm_ops rockchip_vpu_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

static struct platform_driver rockchip_vpu_driver = {
	.probe = rockchip_vpu_probe,
	.remove = rockchip_vpu_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .of_match_table = of_match_ptr(of_rockchip_vpu_match),
		   .pm = &rockchip_vpu_pm_ops,
	},
};
module_platform_driver(rockchip_vpu_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Alpha Lin <Alpha.Lin@Rock-Chips.com>");
MODULE_AUTHOR("Tomasz Figa <tfiga@chromium.org>");
MODULE_AUTHOR("Ezequiel Garcia <ezequiel@collabora.com>");
MODULE_DESCRIPTION("Rockchip VPU codec driver");

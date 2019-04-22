/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip VPU codec driver
 *
 * Copyright 2018 Google LLC.
 *	Tomasz Figa <tfiga@chromium.org>
 *
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 */

#ifndef ROCKCHIP_VPU_H_
#define ROCKCHIP_VPU_H_

#include <linux/platform_device.h>
#include <linux/videodev2.h>
#include <linux/wait.h>
#include <linux/clk.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#include "rockchip_vpu_hw.h"

#define ROCKCHIP_VPU_MAX_CLOCKS		4

#define JPEG_MB_DIM			16
#define JPEG_MB_WIDTH(w)		DIV_ROUND_UP(w, JPEG_MB_DIM)
#define JPEG_MB_HEIGHT(h)		DIV_ROUND_UP(h, JPEG_MB_DIM)

struct rockchip_vpu_ctx;
struct rockchip_vpu_codec_ops;

#define RK_VPU_JPEG_ENCODER	BIT(0)
#define RK_VPU_ENCODERS		0x0000ffff

#define RK_VPU_DECODERS		0xffff0000

/**
 * struct rockchip_vpu_variant - information about VPU hardware variant
 *
 * @enc_offset:			Offset from VPU base to encoder registers.
 * @dec_offset:			Offset from VPU base to decoder registers.
 * @enc_fmts:			Encoder formats.
 * @num_enc_fmts:		Number of encoder formats.
 * @dec_fmts:			Decoder formats.
 * @num_dec_fmts:		Number of decoder formats.
 * @codec:			Supported codecs
 * @codec_ops:			Codec ops.
 * @init:			Initialize hardware.
 * @vepu_irq:			encoder interrupt handler
 * @vdpu_irq:			decoder interrupt handler
 * @clk_names:			array of clock names
 * @num_clocks:			number of clocks in the array
 */
struct rockchip_vpu_variant {
	unsigned int enc_offset;
	unsigned int dec_offset;
	const struct rockchip_vpu_fmt *enc_fmts;
	unsigned int num_enc_fmts;
	const struct rockchip_vpu_fmt *dec_fmts;
	unsigned int num_dec_fmts;
	unsigned int codec;
	const struct rockchip_vpu_codec_ops *codec_ops;
	int (*init)(struct rockchip_vpu_dev *vpu);
	irqreturn_t (*vepu_irq)(int irq, void *priv);
	irqreturn_t (*vdpu_irq)(int irq, void *priv);
	const char *clk_names[ROCKCHIP_VPU_MAX_CLOCKS];
	int num_clocks;
};

/**
 * enum rockchip_vpu_codec_mode - codec operating mode.
 * @RK_VPU_MODE_NONE:  No operating mode. Used for RAW video formats.
 * @RK_VPU_MODE_JPEG_ENC: JPEG encoder.
 */
enum rockchip_vpu_codec_mode {
	RK_VPU_MODE_NONE = -1,
	RK_VPU_MODE_JPEG_ENC,
};

/**
 * enum rockchip_vpu_type - device type, encoder or decoder
 * @RK_VPU_ENCODER: The device is an encoder.
 * @RK_VPU_DECODER: The device is a decoder.
 */
enum rockchip_vpu_type {
	RK_VPU_ENCODER,
	RK_VPU_DECODER,
};

/*
 * struct rockchip_vpu_ctrl - helper type to declare supported controls
 * @id:		V4L2 control ID (V4L2_CID_xxx)
 * @codec:	codec id this control belong to (RK_VPU_JPEG_ENCODER, etc.)
 * @cfg:	control configuration
 */
struct rockchip_vpu_ctrl {
	unsigned int id;
	unsigned int codec;
	struct v4l2_ctrl_config cfg;
};

/*
 * struct rockchip_vpu_mc - media controller data
 *
 * @source:		&struct media_entity pointer with the source entity
 * @source_pad:		&struct media_pad with the source pad.
 * @sink:		&struct media_entity pointer with the sink entity
 * @sink_pad:		&struct media_pad with the sink pad.
 * @proc:		&struct media_entity pointer with the M2M device itself.
 * @proc_pads:		&struct media_pad with the @proc pads.
 * @intf_devnode:	&struct media_intf devnode pointer with the interface
 *			with controls the M2M device.
 */
struct rockchip_vpu_mc {
	struct media_entity	*source;
	struct media_pad	source_pad;
	struct media_entity	sink;
	struct media_pad	sink_pad;
	struct media_entity	proc;
	struct media_pad	proc_pads[2];
	struct media_intf_devnode *intf_devnode;
};

/**
 * struct rockchip_vpu_dev - driver data
 * @v4l2_dev:		V4L2 device to register video devices for.
 * @m2m_dev:		mem2mem device associated to this device.
 * @mdev:		media device associated to this device.
 * @vfd_enc:		Video device for encoder.
 * @vfd_dec:		Video device for decoder.
 * @pdev:		Pointer to VPU platform device.
 * @mc:			Array of media controller topology structs
 *			for encoder and decoder.
 * @dev:		Pointer to device for convenient logging using
 *			dev_ macros.
 * @clocks:		Array of clock handles.
 * @base:		Mapped address of VPU registers.
 * @enc_base:		Mapped address of VPU encoder register for convenience.
 * @dec_base:		Mapped address of VPU decoder register for convenience.
 * @vpu_mutex:		Mutex to synchronize V4L2 calls.
 * @irqlock:		Spinlock to synchronize access to data structures
 *			shared with interrupt handlers.
 * @variant:		Hardware variant-specific parameters.
 * @watchdog_work:	Delayed work for hardware timeout handling.
 */
struct rockchip_vpu_dev {
	struct v4l2_device v4l2_dev;
	struct v4l2_m2m_dev *m2m_dev;
	struct media_device mdev;
	struct video_device *vfd_enc;
	struct video_device *vfd_dec;
	struct platform_device *pdev;
	struct rockchip_vpu_mc mc[2];
	struct device *dev;
	struct clk_bulk_data clocks[ROCKCHIP_VPU_MAX_CLOCKS];
	void __iomem *base;
	void __iomem *enc_base;
	void __iomem *dec_base;

	struct mutex vpu_mutex;	/* video_device lock */
	spinlock_t irqlock;
	const struct rockchip_vpu_variant *variant;
	struct delayed_work watchdog_work;
};

/**
 * struct rockchip_vpu_ctx - Context (instance) private data.
 *
 * @dev:		VPU driver data to which the context belongs.
 * @fh:			V4L2 file handler.
 *
 * @sequence_cap:       Sequence counter for capture queue
 * @sequence_out:       Sequence counter for output queue
 *
 * @vpu_src_fmt:	Descriptor of active source format.
 * @src_fmt:		V4L2 pixel format of active source format.
 * @vpu_dst_fmt:	Descriptor of active destination format.
 * @dst_fmt:		V4L2 pixel format of active destination format.
 *
 * @ctrl_handler:	Control handler used to register controls.
 * @jpeg_quality:	User-specified JPEG compression quality.
 *
 * @buf_finish:		Buffer finish. This depends on encoder or decoder
 *			context, and it's called right before
 *			calling v4l2_m2m_job_finish.
 * @codec_ops:		Set of operations related to codec mode.
 * @jpeg_enc:		JPEG-encoding context.
 */
struct rockchip_vpu_ctx {
	struct rockchip_vpu_dev *dev;
	struct v4l2_fh fh;

	u32 sequence_cap;
	u32 sequence_out;

	const struct rockchip_vpu_fmt *vpu_src_fmt;
	struct v4l2_pix_format_mplane src_fmt;
	const struct rockchip_vpu_fmt *vpu_dst_fmt;
	struct v4l2_pix_format_mplane dst_fmt;

	struct v4l2_ctrl_handler ctrl_handler;
	int jpeg_quality;

	int (*buf_finish)(struct rockchip_vpu_ctx *ctx,
			  struct vb2_buffer *buf,
			  unsigned int bytesused);

	const struct rockchip_vpu_codec_ops *codec_ops;

	/* Specific for particular codec modes. */
	union {
		struct rockchip_vpu_jpeg_enc_hw_ctx jpeg_enc;
	};
};

/**
 * struct rockchip_vpu_fmt - information about supported video formats.
 * @name:	Human readable name of the format.
 * @fourcc:	FourCC code of the format. See V4L2_PIX_FMT_*.
 * @codec_mode:	Codec mode related to this format. See
 *		enum rockchip_vpu_codec_mode.
 * @header_size: Optional header size. Currently used by JPEG encoder.
 * @max_depth:	Maximum depth, for bitstream formats
 * @enc_fmt:	Format identifier for encoder registers.
 * @frmsize:	Supported range of frame sizes (only for bitstream formats).
 */
struct rockchip_vpu_fmt {
	char *name;
	u32 fourcc;
	enum rockchip_vpu_codec_mode codec_mode;
	int header_size;
	int max_depth;
	enum rockchip_vpu_enc_fmt enc_fmt;
	struct v4l2_frmsize_stepwise frmsize;
};

/* Logging helpers */

/**
 * debug - Module parameter to control level of debugging messages.
 *
 * Level of debugging messages can be controlled by bits of
 * module parameter called "debug". Meaning of particular
 * bits is as follows:
 *
 * bit 0 - global information: mode, size, init, release
 * bit 1 - each run start/result information
 * bit 2 - contents of small controls from userspace
 * bit 3 - contents of big controls from userspace
 * bit 4 - detail fmt, ctrl, buffer q/dq information
 * bit 5 - detail function enter/leave trace information
 * bit 6 - register write/read information
 */
extern int rockchip_vpu_debug;

#define vpu_debug(level, fmt, args...)				\
	do {							\
		if (rockchip_vpu_debug & BIT(level))		\
			pr_info("%s:%d: " fmt,	                \
				 __func__, __LINE__, ##args);	\
	} while (0)

#define vpu_err(fmt, args...)					\
	pr_err("%s:%d: " fmt, __func__, __LINE__, ##args)

/* Structure access helpers. */
static inline struct rockchip_vpu_ctx *fh_to_ctx(struct v4l2_fh *fh)
{
	return container_of(fh, struct rockchip_vpu_ctx, fh);
}

/* Register accessors. */
static inline void vepu_write_relaxed(struct rockchip_vpu_dev *vpu,
				      u32 val, u32 reg)
{
	vpu_debug(6, "0x%04x = 0x%08x\n", reg / 4, val);
	writel_relaxed(val, vpu->enc_base + reg);
}

static inline void vepu_write(struct rockchip_vpu_dev *vpu, u32 val, u32 reg)
{
	vpu_debug(6, "0x%04x = 0x%08x\n", reg / 4, val);
	writel(val, vpu->enc_base + reg);
}

static inline u32 vepu_read(struct rockchip_vpu_dev *vpu, u32 reg)
{
	u32 val = readl(vpu->enc_base + reg);

	vpu_debug(6, "0x%04x = 0x%08x\n", reg / 4, val);
	return val;
}

static inline void vdpu_write_relaxed(struct rockchip_vpu_dev *vpu,
				      u32 val, u32 reg)
{
	vpu_debug(6, "0x%04x = 0x%08x\n", reg / 4, val);
	writel_relaxed(val, vpu->dec_base + reg);
}

static inline void vdpu_write(struct rockchip_vpu_dev *vpu, u32 val, u32 reg)
{
	vpu_debug(6, "0x%04x = 0x%08x\n", reg / 4, val);
	writel(val, vpu->dec_base + reg);
}

static inline u32 vdpu_read(struct rockchip_vpu_dev *vpu, u32 reg)
{
	u32 val = readl(vpu->dec_base + reg);

	vpu_debug(6, "0x%04x = 0x%08x\n", reg / 4, val);
	return val;
}

bool rockchip_vpu_is_encoder_ctx(const struct rockchip_vpu_ctx *ctx);

#endif /* ROCKCHIP_VPU_H_ */

// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018 NXP
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/busfreq-imx.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include <media/v4l2-async.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fwnode.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#include "m4_rpmsg.h"

#define IMX_RPMSG_DRV_NAME	"imx-rpmsg-v4l2"
#define IMX_RPMSG_MAX_SUBDEV_NUM	4

#define notifier_to_rpmsg_dev(n) container_of(n, struct imx_rpmsg_device, notifier)
#define v4l2_dev_to_rpmsg_dev(d) container_of(d, struct imx_rpmsg_device, v4l2_dev)

/* reg bit manipulation */
#define REG_MASK(e, s) (((1 << ((e) - (s) + 1)) - 1) << (s))
#define REG_PUT(x, e, s) (((x) << (s)) & REG_MASK(e, s))
#define REG_GET(x, e, s) (((x) & REG_MASK(e, s)) >> (s))

struct imx_rpmsg_input {
	unsigned int index;
	const char *name;
	unsigned int type;
	unsigned int caps;
};

struct imx_rpmsg_fh {
	struct v4l2_fh fh;

	/* TODO other fields added later */
};

struct imx_rpmsg_fmt {
	char name[32];
	u32  pix_fmt;
	u32  fourcc;
	u32  mbus_code;
	int  bpp;
};

struct rpmsg_buf_internal {
	struct list_head queue;
};

struct imx_rpmsg_buffer {
	struct vb2_v4l2_buffer vb;
	struct rpmsg_buf_internal internal;
	uint32_t field;
};

struct imx_rpmsg_device {
	struct device *dev;
	struct v4l2_device v4l2_dev;
	struct video_device *vdev;
	struct vb2_queue queue;
	struct v4l2_subdev *v4l2_sd;

	spinlock_t slock;
	struct list_head active_queue;
	uint32_t frame_count;
	bool streaming;

	struct mutex lock;
	atomic_t use_count;
	unsigned int curr_input;

	struct imx_rpmsg_fmt rpmsg_fmt;
	struct v4l2_pix_format v4l2_pix_fmt;
	u32 buf_type;
	u32 mbus_code;

	/* async subdevs */
	struct v4l2_async_subdev subdevs[IMX_RPMSG_MAX_SUBDEV_NUM];
	struct v4l2_async_subdev *async_subdevs[IMX_RPMSG_MAX_SUBDEV_NUM];
	struct v4l2_async_notifier notifier;
};

static const struct imx_rpmsg_fmt rpmsg_fmts[] = {
	{
		.name		= "YUYV",
		.fourcc		= V4L2_PIX_FMT_YUYV,
		.pix_fmt	= V4L2_PIX_FMT_YUYV,
		.mbus_code	= MEDIA_BUS_FMT_YUYV8_1X16,
		.bpp		= 16,
	},
	{
		.name		= "Y16",
		.fourcc		= V4L2_PIX_FMT_Y16,
		.pix_fmt	= V4L2_PIX_FMT_Y16,
		.mbus_code	= MEDIA_BUS_FMT_Y16_1X16,
		.bpp		= 16,
	},
};

static const struct imx_rpmsg_input inputs[] = {
	{
		.index = 0,
		.name  = "Camera M4",
		.type  = V4L2_INPUT_TYPE_CAMERA,
		.caps  = 0,
	},
};

static const struct imx_rpmsg_fmt *rpmsg_fmt_by_mbus(u32 code)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(rpmsg_fmts); i++) {
		if (rpmsg_fmts[i].mbus_code == code)
			return rpmsg_fmts + i;
	}

	pr_err("unknown mbus:0x%x\n", code);

	return NULL;
}

static const struct imx_rpmsg_fmt *rpmsg_fmt_by_pix_fmt(u32 pixelformat)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(rpmsg_fmts); i++) {
		if (rpmsg_fmts[i].pix_fmt == pixelformat)
			return rpmsg_fmts + i;
	}

	pr_err("unknown pix_fmt:0x%x\n", pixelformat);

	return NULL;
}

static int imx_rpmsg_set_fmt(struct imx_rpmsg_device *rpmsg_dev)
{
	struct imx_rpmsg_fmt *rpmsg_fmt = &rpmsg_dev->rpmsg_fmt;

	switch (rpmsg_fmt->mbus_code) {
	case MEDIA_BUS_FMT_YUYV8_1X16:
	case MEDIA_BUS_FMT_Y16_1X16:
		/* YUYV */
		break;
	default:
		dev_err(rpmsg_dev->dev,
			"%s: unsupported mbus fmt: %#x\n",
			__func__, rpmsg_fmt->mbus_code);
		return -EINVAL;
	}

	return 0;
}

static int imx_rpmsg_config_dma(struct imx_rpmsg_device *rpmsg_dev,
			      dma_addr_t dma_addr,
			      unsigned int field)
{
	if (field > 1)
		return -EINVAL;

	rpmsg_send_buffer(dma_addr);

	return 0;
}

static int imx_rpmsg_vidioc_querycap(struct file *file, void *fh,
				   struct v4l2_capability *cap)
{
	struct imx_rpmsg_device *rpmsg_dev = video_drvdata(file);

	strlcpy(cap->driver, IMX_RPMSG_DRV_NAME, sizeof(cap->driver));
	strlcpy(cap->card, "V4L2 i.MX RPMSG", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info),
		 "platform:%s", dev_name(rpmsg_dev->dev));

	return 0;
}

static int imx_rpmsg_vidioc_enum_input(struct file *filp, void *fh,
				     struct v4l2_input *inp)
{
	const struct imx_rpmsg_input *input;

	if (inp->index >= ARRAY_SIZE(inputs))
		return -EINVAL;

	input = &inputs[inp->index];
	WARN_ON(input->index != inp->index);

	inp->type = input->type;
	inp->capabilities = input->caps;
	strlcpy(inp->name, input->name, sizeof(inp->name));
	inp->std = (input->caps & V4L2_IN_CAP_STD) ?
		   (V4L2_STD_NTSC_M | V4L2_STD_PAL) : 0;

	return 0;
}

static int imx_rpmsg_vidioc_g_input(struct file *filp, void *fh, unsigned int *i)
{
	struct imx_rpmsg_device *rpmsg_dev = video_drvdata(filp);

	*i = rpmsg_dev->curr_input;

	return 0;
}

static int imx_rpmsg_vidioc_s_input(struct file *filp, void *fh, unsigned int i)
{
	struct imx_rpmsg_device *rpmsg_dev = video_drvdata(filp);

	if (i >= ARRAY_SIZE(inputs))
		return -EINVAL;

	/* 's_input' need to be called at the beginning */
	if (vb2_is_busy(&rpmsg_dev->queue))
		return -EBUSY;

	rpmsg_dev->curr_input = i;

	return 0;
}

static int imx_rpmsg_vidioc_enum_fmt_vid_cap(struct file *filp, void *fh,
					   struct v4l2_fmtdesc *f)
{
	int ret;
	struct imx_rpmsg_device *rpmsg_dev = video_drvdata(filp);
	struct v4l2_subdev *v4l2_sd = rpmsg_dev->v4l2_sd;
	const struct imx_rpmsg_fmt *rpmsg_fmt;
	struct v4l2_subdev_mbus_code_enum  code = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		/* format index during enumeration */
		.index = f->index,
	};

	ret = v4l2_subdev_call(v4l2_sd, pad, enum_mbus_code, NULL, &code);
	if (ret)
		return -EINVAL;

	rpmsg_fmt = rpmsg_fmt_by_mbus(code.code);
	if (!rpmsg_fmt) {
		dev_err(rpmsg_dev->dev, "mbus code %#x invalid\n", code.code);
		return -EINVAL;
	}

	strlcpy(f->description, rpmsg_fmt->name, sizeof(f->description));
	f->pixelformat = rpmsg_fmt->pix_fmt;

	return 0;
}

static int imx_rpmsg_vidioc_try_fmt_vid_cap(struct file *filp, void *fh,
					  struct v4l2_format *f)
{
	int ret;
	struct imx_rpmsg_device *rpmsg_dev = video_drvdata(filp);
	struct v4l2_subdev *v4l2_sd = rpmsg_dev->v4l2_sd;
	struct v4l2_pix_format *v4l2_pix_fmt = &f->fmt.pix;
	const struct imx_rpmsg_fmt *rpmsg_fmt;
	struct v4l2_subdev_format v4l2_sd_fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};

	rpmsg_fmt = rpmsg_fmt_by_pix_fmt(v4l2_pix_fmt->pixelformat);
	if (!rpmsg_fmt)
		return -EINVAL;

	if (!v4l2_pix_fmt->width || !v4l2_pix_fmt->height) {
		dev_err(rpmsg_dev->dev, "invalid with or height\n");
		return -EINVAL;
	}

	v4l2_fill_mbus_format(&v4l2_sd_fmt.format,
			      v4l2_pix_fmt,
			      rpmsg_fmt->mbus_code);

	ret = v4l2_subdev_call(v4l2_sd, pad, set_fmt, NULL, &v4l2_sd_fmt);
	if (ret)
		return -EINVAL;

	v4l2_fill_pix_format(v4l2_pix_fmt, &v4l2_sd_fmt.format);

	/* TODO: set field none */
	v4l2_pix_fmt->field = V4L2_FIELD_NONE;

	v4l2_pix_fmt->sizeimage = (16 >> 3) *
				  v4l2_pix_fmt->height *
				  v4l2_pix_fmt->width;
	v4l2_pix_fmt->bytesperline = (16 >> 3) * v4l2_pix_fmt->width;

	return 0;
}

static int imx_rpmsg_vidioc_g_fmt_vid_cap(struct file *filp, void *fh,
					struct v4l2_format *f)
{
	struct imx_rpmsg_device *rpmsg_dev = video_drvdata(filp);

	f->fmt.pix = rpmsg_dev->v4l2_pix_fmt;

	return 0;
}

static int imx_rpmsg_vidioc_s_fmt_vid_cap(struct file *filp, void *fh,
					struct v4l2_format *f)
{
	int ret;
	struct imx_rpmsg_device *rpmsg_dev = video_drvdata(filp);
	struct v4l2_pix_format *v4l2_pix_fmt = &f->fmt.pix;
	const struct imx_rpmsg_fmt *rpmsg_fmt;

	dev_info(rpmsg_dev->dev,
		 "%s width %d  height %d\n",
		 __func__,
		 f->fmt.pix.width,
		 f->fmt.pix.height);
	dev_info(rpmsg_dev->dev,
		 "%s pixformat %s\n",
		 __func__,
		 v4l2_pix_fmt->pixelformat == V4L2_PIX_FMT_YUYV?"YUYV":"Y16");
	dev_info(rpmsg_dev->dev,
		 "%s field %d  bytesperline: %d sizeimage: %d\n",
		 __func__,
		 f->fmt.pix.field,
		 f->fmt.pix.bytesperline,
		 f->fmt.pix.sizeimage);

	ret = imx_rpmsg_vidioc_try_fmt_vid_cap(filp, rpmsg_dev, f);
	if (ret < 0)
		return ret;

	rpmsg_fmt = rpmsg_fmt_by_pix_fmt(v4l2_pix_fmt->pixelformat);
	rpmsg_dev->rpmsg_fmt   = *rpmsg_fmt;
	rpmsg_dev->mbus_code = rpmsg_fmt->mbus_code;
	rpmsg_dev->v4l2_pix_fmt.pixelformat = v4l2_pix_fmt->pixelformat;
	rpmsg_dev->v4l2_pix_fmt.width	= v4l2_pix_fmt->width;
	rpmsg_dev->v4l2_pix_fmt.height	= v4l2_pix_fmt->height;
	rpmsg_dev->v4l2_pix_fmt.sizeimage	= v4l2_pix_fmt->sizeimage;
	rpmsg_dev->v4l2_pix_fmt.bytesperline = v4l2_pix_fmt->bytesperline;
	rpmsg_dev->v4l2_pix_fmt.field	= V4L2_FIELD_NONE;
	rpmsg_dev->buf_type		= f->type;

	if (v4l2_pix_fmt->pixelformat == V4L2_PIX_FMT_YUYV)
		rpmsg_set_pixelformat(ovRpmsg_format_YUYV);
	else if (v4l2_pix_fmt->pixelformat == V4L2_PIX_FMT_Y16)
		rpmsg_set_pixelformat(ovRpmsg_format_Y16);

	if (v4l2_pix_fmt->width == IR_RESOLUTION_FULL_WIDTH) {
		if (v4l2_pix_fmt->height == IR_RESOLUTION_FULL_HEIGHT)
			rpmsg_set_resolution(ovRpmsg_mode_QQVGA_160_120);
		else if (v4l2_pix_fmt->height == IR_RESOLUTION_FULL_TELEMETRY_HEIGHT) {
			if (v4l2_pix_fmt->pixelformat == V4L2_PIX_FMT_Y16)
				rpmsg_set_resolution(ovRpmsg_mode_QQVGA_160_122);
			else
				dev_err(rpmsg_dev->dev, "160x122 resolution only available in Y16 pixel format");
		}
	} else if (v4l2_pix_fmt->width == IR_RESOLUTION_FULL_WIDTH_VOSPI) {
		if (v4l2_pix_fmt->height == IR_RESOLUTION_FULL_TELEMETRY_HEIGHT) {
			if (v4l2_pix_fmt->pixelformat == V4L2_PIX_FMT_Y16)
				rpmsg_set_resolution(ovRpmsg_mode_QQVGA_164_122);
			else
				dev_err(rpmsg_dev->dev, "164x122 resolution (raw vospi) only available in Y16 pixel format");
		}
	} else if (v4l2_pix_fmt->width == IR_RESOLUTION_REDUCED_WIDTH)
		rpmsg_set_resolution(ovRpmsg_mode_QQVGA_128_96);
	else if (v4l2_pix_fmt->width == IR_RESOLUTION_256x192_WIDTH)
		rpmsg_set_resolution(ovRpmsg_mode_256x192);
	else if (v4l2_pix_fmt->width == IR_RESOLUTION_HQVGA_WIDTH)
		rpmsg_set_resolution(ovRpmsg_mode_HQVGA);
	else if (v4l2_pix_fmt->width == IR_RESOLUTION_QVGA_WIDTH)
		rpmsg_set_resolution(ovRpmsg_mode_QVGA);

	return 0;
}

static int imx_rpmsg_vidioc_g_parm(struct file *filp, void *fh,
				 struct v4l2_streamparm *a)
{
	struct imx_rpmsg_device *rpmsg_dev = video_drvdata(filp);
	struct v4l2_subdev *v4l2_sd = rpmsg_dev->v4l2_sd;

	return v4l2_subdev_call(v4l2_sd, video, g_parm, a);
}

static int imx_rpmsg_vidioc_s_parm(struct file *filp, void *fh,
				 struct v4l2_streamparm *a)
{
	struct imx_rpmsg_device *rpmsg_dev = video_drvdata(filp);
	struct v4l2_subdev *v4l2_sd = rpmsg_dev->v4l2_sd;

	return v4l2_subdev_call(v4l2_sd, video, s_parm, a);
}

static int imx_rpmsg_vidioc_enum_framesizes(struct file *filp, void *fh,
					  struct v4l2_frmsizeenum *fsize)
{
	int ret;
	struct imx_rpmsg_device *rpmsg_dev = video_drvdata(filp);
	struct v4l2_subdev *v4l2_sd = rpmsg_dev->v4l2_sd;
	const struct imx_rpmsg_fmt *rpmsg_fmt;
	struct v4l2_subdev_frame_size_enum fse = {
		.index = fsize->index,
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};

	rpmsg_fmt  = rpmsg_fmt_by_pix_fmt(fsize->pixel_format);
	fse.code = rpmsg_fmt->mbus_code;

	ret = v4l2_subdev_call(v4l2_sd, pad, enum_frame_size, NULL, &fse);
	if (ret)
		return ret;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width  = fse.max_width;
	fsize->discrete.height = fse.max_height;

	return 0;
}

static int imx_rpmsg_vidioc_enum_frameintervals(struct file *filp, void *fh,
					      struct v4l2_frmivalenum *fival)
{
	int ret;
	struct imx_rpmsg_device *rpmsg_dev = video_drvdata(filp);
	struct v4l2_subdev *v4l2_sd = rpmsg_dev->v4l2_sd;
	const struct imx_rpmsg_fmt *rpmsg_fmt;
	struct v4l2_subdev_frame_interval_enum fie = {
		.index  = fival->index,
		.width  = fival->width,
		.height = fival->height,
		.which  = V4L2_SUBDEV_FORMAT_ACTIVE,
	};

	rpmsg_fmt  = rpmsg_fmt_by_pix_fmt(fival->pixel_format);
	fie.code = rpmsg_fmt->mbus_code;

	ret = v4l2_subdev_call(v4l2_sd, pad, enum_frame_interval, NULL, &fie);
	if (ret)
		return ret;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete = fie.interval;

	return 0;
}

static const struct v4l2_ioctl_ops imx_rpmsg_ioctl_ops = {
	.vidioc_querycap		= imx_rpmsg_vidioc_querycap,
	.vidioc_enum_fmt_vid_cap	= imx_rpmsg_vidioc_enum_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap		= imx_rpmsg_vidioc_try_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap		= imx_rpmsg_vidioc_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= imx_rpmsg_vidioc_s_fmt_vid_cap,

	.vidioc_enum_input		= imx_rpmsg_vidioc_enum_input,
	.vidioc_g_input			= imx_rpmsg_vidioc_g_input,
	.vidioc_s_input			= imx_rpmsg_vidioc_s_input,

	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,

	.vidioc_g_parm			= imx_rpmsg_vidioc_g_parm,
	.vidioc_s_parm			= imx_rpmsg_vidioc_s_parm,
	.vidioc_enum_framesizes		= imx_rpmsg_vidioc_enum_framesizes,
	.vidioc_enum_frameintervals	= imx_rpmsg_vidioc_enum_frameintervals,
};

static int imx_rpmsg_open(struct file *filp)
{
	struct imx_rpmsg_device *rpmsg_dev = video_drvdata(filp);
	struct video_device *vdev = video_devdata(filp);
	struct imx_rpmsg_fh *rpmsg_fh;

	if (mutex_lock_interruptible(&rpmsg_dev->lock))
		return -ERESTARTSYS;

	rpmsg_fh = kzalloc(sizeof(*rpmsg_fh), GFP_KERNEL);
	if (!rpmsg_fh) {
		mutex_unlock(&rpmsg_dev->lock);
		return -ENOMEM;
	}

	/* first open: RPMSG should be clean */
	if (atomic_inc_return(&rpmsg_dev->use_count) == 1) {
		pm_runtime_get_sync(rpmsg_dev->dev);
		rpmsg_dev->frame_count = 0;
	}

	v4l2_fh_init(&rpmsg_fh->fh, vdev);
	filp->private_data = &rpmsg_fh->fh;
	v4l2_fh_add(&rpmsg_fh->fh);

	mutex_unlock(&rpmsg_dev->lock);

	return 0;
}

static int imx_rpmsg_release(struct file *filp)
{
	struct imx_rpmsg_device *rpmsg_dev = video_drvdata(filp);
	struct v4l2_fh *fh = filp->private_data;
	struct imx_rpmsg_fh *rpmsg_fh;

	if (!fh) {
		WARN_ON(1);
		return 0;
	}

	mutex_lock(&rpmsg_dev->lock);

	rpmsg_fh = container_of(fh, struct imx_rpmsg_fh, fh);

	if (atomic_dec_and_test(&rpmsg_dev->use_count)) {
		pm_runtime_put(rpmsg_dev->dev);
		mutex_unlock(&rpmsg_dev->lock);
		vb2_fop_release(filp);
	} else
		mutex_unlock(&rpmsg_dev->lock);

	return 0;
}

static const struct v4l2_file_operations imx_rpmsg_fops = {
	.owner	= THIS_MODULE,
	.open	= imx_rpmsg_open,
	.release = imx_rpmsg_release,
	.unlocked_ioctl = video_ioctl2,
	.read	= vb2_fop_read,
	.mmap	= vb2_fop_mmap,
	.poll	= vb2_fop_poll,
};

static int imx_rpmsg_queue_setup(struct vb2_queue *q,
			       unsigned int *num_buffers,
			       unsigned int *num_planes,
			       unsigned int sizes[],
			       struct device *alloc_devs[])
{
	struct imx_rpmsg_device *rpmsg_dev = vb2_get_drv_priv(q);
	struct v4l2_pix_format *fmt = &rpmsg_dev->v4l2_pix_fmt;

	/* TODO: don't support multiple plane format */

	WARN_ON(*num_buffers < 3);

	switch (fmt->pixelformat) {
	case V4L2_PIX_FMT_UYVY:
		if (!*num_planes || *num_planes > 1)
			*num_planes = 1;
		sizes[0] = fmt->sizeimage;
		break;
	case V4L2_PIX_FMT_YUYV:
		if (!*num_planes || *num_planes > 1)
			*num_planes = 1;
		sizes[0] = fmt->sizeimage;
		break;
	case V4L2_PIX_FMT_Y16:
		if (!*num_planes || *num_planes > 1)
			*num_planes = 1;
		sizes[0] = fmt->sizeimage;
		break;
	case V4L2_PIX_FMT_ABGR32:
		if (!*num_planes || *num_planes > 1)
			*num_planes = 1;
		sizes[0] = fmt->sizeimage;
		break;
	case V4L2_PIX_FMT_RGB32:
		if (!*num_planes || *num_planes > 1)
			*num_planes = 1;
		sizes[0] = fmt->sizeimage;
		break;
	default:
		/* unsupported format */
		dev_err(rpmsg_dev->dev, "unsupported format %X\n", fmt->pixelformat);
		return -EINVAL;
	}

	return 0;
}

static int imx_rpmsg_buf_prepare(struct vb2_buffer *vb)
{
	struct imx_rpmsg_device *rpmsg_dev = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format *fmt = &rpmsg_dev->v4l2_pix_fmt;
	unsigned int plane_no = 0;

	if (WARN_ON(vb->num_planes != 1))
		return -EINVAL;

	if (vb2_plane_size(vb, plane_no) < fmt->sizeimage)
		return -EINVAL;

	vb2_set_plane_payload(vb, plane_no, fmt->sizeimage);

	return 0;
}

static int imx_rpmsg_start_streaming(struct vb2_queue *q,
		unsigned int count)
{
	int ret = 0;
	unsigned long flags;
	struct imx_rpmsg_device *rpmsg_dev = vb2_get_drv_priv(q);
	struct imx_rpmsg_buffer *buf, *tmp;
	struct vb2_buffer *vb;
	int d = 0, i = 0;
	dma_addr_t dma_addr[16] = {0};

	if (WARN_ON(count < 3) || WARN_ON(count > 16))
		return -ENOBUFS;

	spin_lock_irqsave(&rpmsg_dev->slock, flags);

	if (unlikely(list_empty(&rpmsg_dev->active_queue))) {
		WARN_ON(1);
		spin_unlock_irqrestore(&rpmsg_dev->slock, flags);
		return -ENOBUFS;
	}

	list_for_each_entry_safe(buf, tmp,
			&rpmsg_dev->active_queue, internal.queue) {
		vb = &buf->vb.vb2_buf;
		vb->state = VB2_BUF_STATE_ACTIVE;
		dma_addr[d++] = vb2_dma_contig_plane_dma_addr(vb, 0);
		buf->field = 0;
	}

	spin_unlock_irqrestore(&rpmsg_dev->slock, flags);

	ret = imx_rpmsg_set_fmt(rpmsg_dev);
	if (ret)
		return ret;

	rpmsg_dev->streaming = true;

	/* enable dma transfer */
	for (i = 0; i < d && ret == 0; ++i)
		ret = imx_rpmsg_config_dma(rpmsg_dev, dma_addr[i], 0);

	return ret;
}

static void imx_rpmsg_stop_streaming(struct vb2_queue *q)
{
	unsigned long flags;
	struct imx_rpmsg_device *rpmsg_dev = vb2_get_drv_priv(q);
	struct imx_rpmsg_buffer *pos, *tmp;
	struct vb2_buffer *vb;

	rpmsg_dev->streaming = false;

	spin_lock_irqsave(&rpmsg_dev->slock, flags);

	list_for_each_entry_safe(pos, tmp,
			&rpmsg_dev->active_queue, internal.queue) {

		list_del_init(&pos->internal.queue);
		vb = &pos->vb.vb2_buf;
		if (vb->state == VB2_BUF_STATE_ACTIVE)
			vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
	}

	INIT_LIST_HEAD(&rpmsg_dev->active_queue);

	spin_unlock_irqrestore(&rpmsg_dev->slock, flags);
}

static void imx_rpmsg_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct imx_rpmsg_device *rpmsg_dev = vb2_get_drv_priv(vb->vb2_queue);
	struct imx_rpmsg_buffer *rpmsg_buf = container_of(vbuf,
						struct imx_rpmsg_buffer,
						vb);
	unsigned long flags;
	dma_addr_t dma_addr;

	spin_lock_irqsave(&rpmsg_dev->slock, flags);

	/* the 'vb' is in active state, so add this
	 * buffer to 'active_queue' to be fetched
	 * when streamon called
	 */
	list_add_tail(&rpmsg_buf->internal.queue, &rpmsg_dev->active_queue);

	spin_unlock_irqrestore(&rpmsg_dev->slock, flags);

	if (rpmsg_dev->streaming) {
		vb->state = VB2_BUF_STATE_ACTIVE;
		dma_addr = vb2_dma_contig_plane_dma_addr(vb, 0);
		imx_rpmsg_config_dma(rpmsg_dev, dma_addr, 0);
	}

}

static struct vb2_ops imx_rpmsg_vb2_ops = {
	.queue_setup		= imx_rpmsg_queue_setup,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
	.buf_prepare		= imx_rpmsg_buf_prepare,
	.start_streaming	= imx_rpmsg_start_streaming,
	.stop_streaming		= imx_rpmsg_stop_streaming,
	.buf_queue		= imx_rpmsg_buf_queue,
};

static int imx_rpmsg_dma_done_handle(struct imx_rpmsg_device *rpmsg_dev, dma_addr_t dma_addr)
{
	int buf_idx = 0;
	unsigned long flags;
	struct imx_rpmsg_buffer *buf, *tmp;
	struct vb2_buffer *vb;

	spin_lock_irqsave(&rpmsg_dev->slock, flags);

	if (unlikely(list_empty(&rpmsg_dev->active_queue))) {
		WARN_ON(1);
		spin_unlock_irqrestore(&rpmsg_dev->slock, flags);
		return -ENOBUFS;
	}

	/* Find the matching buffer by dma addr */
	list_for_each_entry_safe(buf, tmp,
			&rpmsg_dev->active_queue, internal.queue) {
		vb = &buf->vb.vb2_buf;
		if (vb2_dma_contig_plane_dma_addr(vb, 0) == dma_addr) {
			/* delete current buffer from queue  and make vb2 know buffer is done. */
			list_del_init(&buf->internal.queue);
			to_vb2_v4l2_buffer(vb)->sequence = rpmsg_dev->frame_count++;

			/* set timestamp */
			vb->timestamp = ktime_get_ns();
			vb2_buffer_done(vb, VB2_BUF_STATE_DONE);
			break;
		}
		buf_idx++;
	}

	/*
	 * If the dma addr doesn't match the first buffer in the list a buffer
	 * must have been lost so we have to reset the buffer list in the m4
	 * and queue all active buffers again.
	 */
	if (buf_idx > 0) {
		dma_addr_t dma_to_m4;
		/* reset buffers in m4 */
		rpmsg_drop_buffers();
		/* readd buffers m4 with same order as in driver */
		list_for_each_entry_safe(buf, tmp,
				&rpmsg_dev->active_queue, internal.queue) {
			vb = &buf->vb.vb2_buf;
			dma_to_m4 = vb2_dma_contig_plane_dma_addr(vb, 0);
			imx_rpmsg_config_dma(rpmsg_dev, dma_to_m4, 0);
		}
	}

	spin_unlock_irqrestore(&rpmsg_dev->slock, flags);

	return 0;
}

static void imx_rpmsg_callback(uint32_t addr, uint32_t buf_num, void *ptr)
{
	struct imx_rpmsg_device *rpmsg_dev = ptr;

	if (rpmsg_dev->streaming)
		imx_rpmsg_dma_done_handle(rpmsg_dev, (dma_addr_t)addr);
}

static int imx_rpmsg_subdev_bound(struct v4l2_async_notifier *notifier,
				struct v4l2_subdev *subdev,
				struct v4l2_async_subdev *asd)
{
	struct imx_rpmsg_device *rpmsg_dev = notifier_to_rpmsg_dev(notifier);

	/* TODO support multiple subdevs */
	rpmsg_dev->v4l2_sd = subdev;

	dev_info(rpmsg_dev->dev, "subdev %s bound success\n", subdev->name);

	return 0;
}

static void imx_rpmsg_subdev_unbind(struct v4l2_async_notifier *notifier,
				  struct v4l2_subdev *subdev,
				  struct v4l2_async_subdev *asd)
{
	struct imx_rpmsg_device *rpmsg_dev = notifier_to_rpmsg_dev(notifier);

	rpmsg_dev->v4l2_sd = NULL;

	dev_info(rpmsg_dev->dev, "subdev %s unbind\n", subdev->name);
}

static const struct v4l2_async_notifier_operations imx_rpmsg_async_ops = {
	.bound = imx_rpmsg_subdev_bound,
	.unbind = imx_rpmsg_subdev_unbind,
};

static int imx_rpmsg_async_subdevs_register(struct imx_rpmsg_device *rpmsg_dev)
{
	int ret;
	unsigned int num_subdevs = 0;
	struct fwnode_handle *endpoint = NULL;
	struct fwnode_handle *np = dev_fwnode(rpmsg_dev->dev);
	struct v4l2_async_notifier *notifier = &rpmsg_dev->notifier;
	struct v4l2_async_subdev *asd;

	while (num_subdevs < IMX_RPMSG_MAX_SUBDEV_NUM) {
		endpoint = fwnode_graph_get_next_endpoint(np, endpoint);
		if (!endpoint)
			break;

		asd = v4l2_async_notifier_add_fwnode_remote_subdev(
			notifier, endpoint, sizeof(struct v4l2_subdev));
		fwnode_handle_put(endpoint);

		if (IS_ERR(asd))
			return PTR_ERR(asd);

		num_subdevs++;
	}

	if (!num_subdevs) {
		dev_err(rpmsg_dev->dev, "no subdev found for rpmsg\n");
		return -ENODEV;
	}

	notifier->ops = &imx_rpmsg_async_ops;
	ret = v4l2_async_notifier_register(&rpmsg_dev->v4l2_dev, notifier);
	if (ret) {
		dev_err(rpmsg_dev->dev, "register async notifier failed\n");
		return -EINVAL;
	}

	return 0;
}

static int imx_rpmsg_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct imx_rpmsg_device *rpmsg_dev;
	struct vb2_queue *vb2q;

	rpmsg_dev = devm_kzalloc(&pdev->dev, sizeof(*rpmsg_dev), GFP_KERNEL);
	if (!rpmsg_dev)
		return -ENOMEM;

	rpmsg_dev->dev = &pdev->dev;

	mutex_init(&rpmsg_dev->lock);

	/* Register 'v4l2_device' */
	snprintf(rpmsg_dev->v4l2_dev.name,
		 sizeof(rpmsg_dev->v4l2_dev.name), "IMX-RPMSG");

	ret = v4l2_device_register(rpmsg_dev->dev, &rpmsg_dev->v4l2_dev);

	/* Initialize the vb2 queue */
	vb2q = &rpmsg_dev->queue;
	vb2q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vb2q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	vb2q->dev = rpmsg_dev->dev;
	vb2q->drv_priv = rpmsg_dev;
	vb2q->buf_struct_size = sizeof(struct imx_rpmsg_buffer);
	vb2q->min_buffers_needed = 3;
	vb2q->ops = &imx_rpmsg_vb2_ops;
	vb2q->mem_ops = &vb2_dma_contig_memops;
	vb2q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vb2q->lock = &rpmsg_dev->lock;

	/* TODO other possible configurable fields:
	 *	'buf_struct_size';
	 *	'gfp_flags';
	 *	'min_buffers_needed';
	 *	'alloc_devs'
	 */
	ret = vb2_queue_init(vb2q);
	if (ret)
		goto unregister_v4l2_dev;

	/* Register 'video_device' */
	rpmsg_dev->vdev = video_device_alloc();
	if (!rpmsg_dev->vdev) {
		ret = -ENOMEM;
		goto unregister_v4l2_dev;
	}

	strlcpy(rpmsg_dev->vdev->name, "IMX rpmsg", sizeof(rpmsg_dev->vdev->name));

	rpmsg_dev->vdev->release	 = video_device_release;
	rpmsg_dev->vdev->v4l2_dev	 = &rpmsg_dev->v4l2_dev;
	rpmsg_dev->vdev->vfl_dir	 = VFL_DIR_RX;
	rpmsg_dev->vdev->fops	 = &imx_rpmsg_fops;
	rpmsg_dev->vdev->ioctl_ops = &imx_rpmsg_ioctl_ops;
	rpmsg_dev->vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE |
				     V4L2_CAP_STREAMING |
				     V4L2_CAP_READWRITE;
	rpmsg_dev->vdev->lock	 = &rpmsg_dev->lock;
	rpmsg_dev->vdev->queue	 = vb2q;

	video_set_drvdata(rpmsg_dev->vdev, rpmsg_dev);

	ret = video_register_device(rpmsg_dev->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto release_video_device;

	/* Register async subdevs */
	v4l2_async_notifier_init(&rpmsg_dev->notifier);
	ret = imx_rpmsg_async_subdevs_register(rpmsg_dev);
	if (ret) {
		dev_err(rpmsg_dev->dev, "register async subdevs failed\n");
		goto notifier_cleanup;
	}

	/* Seems necessary to set up some default parameters */
	rpmsg_dev->v4l2_pix_fmt.pixelformat = V4L2_PIX_FMT_YUYV;
	rpmsg_dev->v4l2_pix_fmt.width	= IR_RESOLUTION_DEFAULT_WIDTH;
	rpmsg_dev->v4l2_pix_fmt.height	= IR_RESOLUTION_DEFAULT_HEIGHT;
	rpmsg_dev->v4l2_pix_fmt.sizeimage = (16 >> 3) *
			rpmsg_dev->v4l2_pix_fmt.height *
			rpmsg_dev->v4l2_pix_fmt.width;
	rpmsg_dev->v4l2_pix_fmt.bytesperline = (16 >> 3) * rpmsg_dev->v4l2_pix_fmt.width;
	rpmsg_dev->v4l2_pix_fmt.field	= V4L2_FIELD_NONE;


	pm_runtime_enable(rpmsg_dev->dev);
	spin_lock_init(&rpmsg_dev->slock);

	pm_runtime_get_sync(rpmsg_dev->dev);

	INIT_LIST_HEAD(&rpmsg_dev->active_queue);

	rpmsg_setup_callback(imx_rpmsg_callback, (void *)rpmsg_dev);

	dev_info(rpmsg_dev->dev, "IMX RPMSG Driver probe success\n");

	return 0;

notifier_cleanup:
	v4l2_async_notifier_cleanup(&rpmsg_dev->notifier);
release_video_device:
	video_device_release(rpmsg_dev->vdev);
unregister_v4l2_dev:
	v4l2_device_unregister(&rpmsg_dev->v4l2_dev);

	return ret;
}

static int imx_rpmsg_remove(struct platform_device *pdev)
{
	struct v4l2_device *v4l2_dev = dev_get_drvdata(&pdev->dev);
	struct imx_rpmsg_device *rpmsg_dev = v4l2_dev_to_rpmsg_dev(v4l2_dev);

	v4l2_async_notifier_unregister(&rpmsg_dev->notifier);
	v4l2_async_notifier_cleanup(&rpmsg_dev->notifier);
	video_device_release(rpmsg_dev->vdev);
	v4l2_device_unregister(&rpmsg_dev->v4l2_dev);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int imx_rpmsg_suspend(struct device *dev)
{
	struct v4l2_device *v4l2_dev = dev_get_drvdata(dev);
	struct imx_rpmsg_device *rpmsg_dev = v4l2_dev_to_rpmsg_dev(v4l2_dev);

	if (!vb2_is_streaming(&rpmsg_dev->queue))
		return 0;

	return 0;
}

static int imx_rpmsg_resume(struct device *dev)
{
	struct v4l2_device *v4l2_dev = dev_get_drvdata(dev);
	struct imx_rpmsg_device *rpmsg_dev = v4l2_dev_to_rpmsg_dev(v4l2_dev);

	if (!vb2_is_streaming(&rpmsg_dev->queue))
		return 0;

	return 0;
}
#else
#define imx_rpmsg_suspend		NULL
#define imx_rpmsg_resume		NULL
#endif

#ifdef CONFIG_PM
static int imx_rpmsg_runtime_suspend(struct device *dev)
{
	return 0;
}

static int imx_rpmsg_runtime_resume(struct device *dev)
{
	int ret = 0;

	return ret;
}
#else
#define imx_rpmsg_runtime_suspend	NULL
#define imx_rpmsg_runtime_resume	NULL
#endif

static const struct dev_pm_ops imx_rpmsg_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(imx_rpmsg_suspend, imx_rpmsg_resume)
	SET_RUNTIME_PM_OPS(imx_rpmsg_runtime_suspend, imx_rpmsg_runtime_resume, NULL)
};

static const struct of_device_id imx_rpmsg_of_match[] = {
	{ .compatible = "fsl,imx7-rpmsg-capture", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx_rpmsg_of_match);

static struct platform_driver imx_rpmsg_driver = {
	.probe    = imx_rpmsg_probe,
	.remove   = imx_rpmsg_remove,
	.driver   = {
		.name = IMX_RPMSG_DRV_NAME,
		.of_match_table = of_match_ptr(imx_rpmsg_of_match),
		.pm = &imx_rpmsg_pm_ops,
	},
};

module_platform_driver(imx_rpmsg_driver);

MODULE_DESCRIPTION("NXP i.MX RPMSG driver");
MODULE_AUTHOR("Peter Fitger <peter.fitger@flir.se>");
MODULE_LICENSE("GPL");

/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zephyr_video_sw_h264enc

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/video/video.h>

#include "video_common.h"
#include "video_sw_h264enc.h"

LOG_MODULE_REGISTER(video_sw_h264enc, CONFIG_VIDEO_LOG_LEVEL);

#define DEFAULT_WIDTH     320
#define DEFAULT_HEIGHT    240
#define DEFAULT_FPS       30
#define DEFAULT_GOP_SIZE  30
#define DEFAULT_BITRATE   2000000
#define DEFAULT_LEVEL_IDC 40

struct sw_h264enc_data {
	struct k_mutex lock;
	struct video_format in_fmt;
	struct video_format out_fmt;
	struct video_frmival frmival;
	struct k_fifo in_fifo;
	struct k_fifo in_done;
	struct k_fifo out_fifo;
	struct k_fifo out_done;
	struct sw_h264enc enc;
	uint32_t frame_nb;
	uint32_t gop_size;
	uint32_t bitrate;
	bool started;
};

#define SW_H264ENC_FORMAT_CAP(fmt)                                                                 \
	{                                                                                          \
		.pixelformat = fmt,                                                                \
		.width_min = 48,                                                                   \
		.width_max = 1920,                                                                 \
		.height_min = 48,                                                                  \
		.height_max = 1088,                                                                \
		.width_step = 16,                                                                  \
		.height_step = 16,                                                                 \
	}

static const struct video_format_cap in_fmts[] = {
	SW_H264ENC_FORMAT_CAP(VIDEO_PIX_FMT_NV12),
	SW_H264ENC_FORMAT_CAP(VIDEO_PIX_FMT_YUYV),
	{0},
};

static const struct video_format_cap out_fmts[] = {
	SW_H264ENC_FORMAT_CAP(VIDEO_PIX_FMT_H264),
	{0},
};

static struct sw_h264enc_cfg sw_h264enc_get_cfg(const struct video_format *fmt)
{
	return (struct sw_h264enc_cfg){
		.width_mbs = fmt->width / 16,
		.height_mbs = fmt->height / 16,
		.level_idc = DEFAULT_LEVEL_IDC,
		.constrained_baseline = true,
	};
}

static int sw_h264enc_set_fmt(const struct device *dev, struct video_format *fmt)
{
	struct sw_h264enc_data *data = dev->data;
	const struct video_format_cap *caps =
		(fmt->type == VIDEO_BUF_TYPE_INPUT) ? in_fmts : out_fmts;
	size_t idx;
	int ret;

	ret = video_format_caps_index(caps, fmt, &idx);
	if (ret < 0 || fmt->width % 16 != 0 || fmt->height % 16 != 0) {
		LOG_ERR("Unsupported format %s %ux%u", VIDEO_FOURCC_TO_STR(fmt->pixelformat),
			fmt->width, fmt->height);
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->started) {
		ret = -EBUSY;
		goto out;
	}

	if (fmt->type == VIDEO_BUF_TYPE_INPUT) {
		if (fmt->pixelformat == VIDEO_PIX_FMT_NV12) {
			fmt->pitch = fmt->width;
			fmt->size = fmt->width * fmt->height * 3 / 2;
		} else {
			fmt->pitch = fmt->width * 2;
			fmt->size = fmt->pitch * fmt->height;
		}
		data->in_fmt = *fmt;
	} else {
		struct sw_h264enc_cfg cfg = sw_h264enc_get_cfg(fmt);

		fmt->pitch = 0;
		fmt->size = sw_h264enc_max_size(&cfg);
		data->out_fmt = *fmt;
	}

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int sw_h264enc_get_fmt(const struct device *dev, struct video_format *fmt)
{
	struct sw_h264enc_data *data = dev->data;

	*fmt = (fmt->type == VIDEO_BUF_TYPE_INPUT) ? data->in_fmt : data->out_fmt;

	return 0;
}

static int sw_h264enc_get_caps(const struct device *dev, struct video_caps *caps)
{
	caps->format_caps = (caps->type == VIDEO_BUF_TYPE_INPUT) ? in_fmts : out_fmts;
	caps->min_vbuf_count = 1;

	return 0;
}

static int sw_h264enc_set_frmival(const struct device *dev, struct video_frmival *frmival)
{
	struct sw_h264enc_data *data = dev->data;

	if (frmival->numerator == 0 || frmival->denominator == 0) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	data->frmival = *frmival;
	k_mutex_unlock(&data->lock);

	return 0;
}

static int sw_h264enc_get_frmival(const struct device *dev, struct video_frmival *frmival)
{
	struct sw_h264enc_data *data = dev->data;

	*frmival = data->frmival;

	return 0;
}

/* Target size of a P frame for the configured bitrate and frame interval */
static uint32_t sw_h264enc_budget(const struct sw_h264enc_data *data)
{
	return (uint64_t)data->bitrate * data->frmival.numerator /
	       (8ULL * data->frmival.denominator);
}

static int sw_h264enc_start(struct sw_h264enc_data *data, struct video_buffer *out)
{
	struct sw_h264enc_cfg cfg = sw_h264enc_get_cfg(&data->out_fmt);
	size_t len;
	int ret;

	if (data->in_fmt.width != data->out_fmt.width ||
	    data->in_fmt.height != data->out_fmt.height) {
		LOG_ERR("Input %ux%u and output %ux%u sizes differ", data->in_fmt.width,
			data->in_fmt.height, data->out_fmt.width, data->out_fmt.height);
		return -EINVAL;
	}

	ret = sw_h264enc_init(&data->enc, &cfg);
	if (ret < 0) {
		return ret;
	}

	ret = sw_h264enc_headers(&data->enc, out->buffer, out->size, &len);
	if (ret < 0) {
		return ret;
	}

	out->bytesused = len;
	data->frame_nb = 0;
	data->started = true;

	return 0;
}

static int sw_h264enc_encode_frame(struct sw_h264enc_data *data, struct video_buffer *in,
				   struct video_buffer *out)
{
	struct sw_h264enc_frame frame = {
		.data = in->buffer,
		.pitch = data->in_fmt.pitch,
		.pixfmt = (data->in_fmt.pixelformat == VIDEO_PIX_FMT_NV12) ? SW_H264ENC_NV12
									   : SW_H264ENC_YUYV,
	};
	struct sw_h264enc_params params = {
		.key_frame = (data->gop_size == 0) ? (data->frame_nb == 0)
						   : (data->frame_nb % data->gop_size == 0),
		.budget = sw_h264enc_budget(data),
	};
	size_t len;
	int ret;

	if (in->bytesused < data->in_fmt.size) {
		LOG_ERR("Input buffer has %u bytes, expected %u", in->bytesused, data->in_fmt.size);
		return -EINVAL;
	}

	ret = sw_h264enc_encode(&data->enc, &frame, &params, out->buffer, out->size, &len);
	if (ret < 0) {
		LOG_ERR("Output buffer of %u bytes is too small", out->size);
		return ret;
	}

	LOG_DBG("frame %u: %s, %u bytes, %u coded macroblocks", data->frame_nb,
		params.key_frame ? "IDR" : "P", (uint32_t)len, data->enc.coded_mbs);

	out->bytesused = len;
	out->timestamp = in->timestamp;
	data->frame_nb++;

	return 0;
}

/* Called with the lock held, once both an input and an output buffer are queued */
static int sw_h264enc_process(struct sw_h264enc_data *data)
{
	struct video_buffer *in;
	struct video_buffer *out;
	int ret;

	if (k_fifo_is_empty(&data->in_fifo) || k_fifo_is_empty(&data->out_fifo)) {
		return 0;
	}

	in = k_fifo_get(&data->in_fifo, K_NO_WAIT);
	out = k_fifo_get(&data->out_fifo, K_NO_WAIT);

	if (!data->started) {
		/* The first output buffer only carries the SPS and PPS, the input is returned */
		ret = sw_h264enc_start(data, out);
	} else {
		ret = sw_h264enc_encode_frame(data, in, out);
	}

	if (ret < 0) {
		out->bytesused = 0;
	}

	k_fifo_put(&data->in_done, in);
	k_fifo_put(&data->out_done, out);

	return ret;
}

static int sw_h264enc_enqueue(const struct device *dev, struct video_buffer *vbuf)
{
	struct sw_h264enc_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	k_fifo_put((vbuf->type == VIDEO_BUF_TYPE_INPUT) ? &data->in_fifo : &data->out_fifo, vbuf);
	ret = sw_h264enc_process(data);

	k_mutex_unlock(&data->lock);

	return ret;
}

static int sw_h264enc_dequeue(const struct device *dev, struct video_buffer **vbuf,
			      k_timeout_t timeout)
{
	struct sw_h264enc_data *data = dev->data;

	*vbuf = k_fifo_get(((*vbuf)->type == VIDEO_BUF_TYPE_INPUT) ? &data->in_done
								   : &data->out_done,
			   timeout);

	return (*vbuf == NULL) ? -EAGAIN : 0;
}

static int sw_h264enc_set_stream(const struct device *dev, bool enable, enum video_buf_type type)
{
	struct sw_h264enc_data *data = dev->data;

	ARG_UNUSED(type);

	if (!enable) {
		/* The next output buffer starts a new stream */
		k_mutex_lock(&data->lock, K_FOREVER);
		data->started = false;
		k_mutex_unlock(&data->lock);
	}

	return 0;
}

static DEVICE_API(video, sw_h264enc_driver_api) = {
	.set_format = sw_h264enc_set_fmt,
	.get_format = sw_h264enc_get_fmt,
	.get_caps = sw_h264enc_get_caps,
	.set_frmival = sw_h264enc_set_frmival,
	.get_frmival = sw_h264enc_get_frmival,
	.set_stream = sw_h264enc_set_stream,
	.enqueue = sw_h264enc_enqueue,
	.dequeue = sw_h264enc_dequeue,
};

static int sw_h264enc_init_dev(const struct device *dev)
{
	struct sw_h264enc_data *data = dev->data;
	struct video_format fmt = {
		.type = VIDEO_BUF_TYPE_INPUT,
		.pixelformat = VIDEO_PIX_FMT_NV12,
		.width = DEFAULT_WIDTH,
		.height = DEFAULT_HEIGHT,
	};
	int ret;

	k_mutex_init(&data->lock);
	k_fifo_init(&data->in_fifo);
	k_fifo_init(&data->in_done);
	k_fifo_init(&data->out_fifo);
	k_fifo_init(&data->out_done);

	data->frmival = (struct video_frmival){.numerator = 1, .denominator = DEFAULT_FPS};
	data->gop_size = DEFAULT_GOP_SIZE;
	data->bitrate = DEFAULT_BITRATE;

	ret = sw_h264enc_set_fmt(dev, &fmt);
	if (ret < 0) {
		return ret;
	}

	fmt.type = VIDEO_BUF_TYPE_OUTPUT;
	fmt.pixelformat = VIDEO_PIX_FMT_H264;

	return sw_h264enc_set_fmt(dev, &fmt);
}

#define SW_H264ENC_DEFINE(n)                                                                       \
	static struct sw_h264enc_data sw_h264enc_data_##n;                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, sw_h264enc_init_dev, NULL, &sw_h264enc_data_##n, NULL,            \
			      POST_KERNEL, CONFIG_VIDEO_INIT_PRIORITY, &sw_h264enc_driver_api);    \
                                                                                                   \
	VIDEO_DEVICE_DEFINE(sw_h264enc_##n, DEVICE_DT_INST_GET(n), NULL);

DT_INST_FOREACH_STATUS_OKAY(SW_H264ENC_DEFINE)

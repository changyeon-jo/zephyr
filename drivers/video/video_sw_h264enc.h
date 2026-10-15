/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_VIDEO_VIDEO_SW_H264ENC_H_
#define ZEPHYR_DRIVERS_VIDEO_VIDEO_SW_H264ENC_H_

/*
 * Bitstream engine of the software H.264 encoder.
 *
 * It writes Constrained Baseline streams using only two macroblock types: I_PCM, which carries
 * raw samples, and P_Skip, which copies the co-located macroblock of the previous frame. Key
 * frames are made of I_PCM macroblocks only. P frames are skipped except for a number of I_PCM
 * macroblocks that fits a byte budget, refreshed in raster order across frames.
 *
 * It does not depend on Zephyr so it can be built and checked against a decoder on a host.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Input pixel formats, 8-bit */
enum sw_h264enc_pixfmt {
	/** Y plane followed by an interleaved Cb/Cr plane subsampled 2x2 */
	SW_H264ENC_NV12,
	/** Packed Y0 Cb Y1 Cr, chroma subsampled 2x1 */
	SW_H264ENC_YUYV,
};

/** Stream configuration, fixed between two calls to sw_h264enc_init() */
struct sw_h264enc_cfg {
	/** Width in macroblocks of 16 pixels */
	uint16_t width_mbs;
	/** Height in macroblocks of 16 pixels */
	uint16_t height_mbs;
	/** level_idc written in the SPS, i.e. 40 for level 4.0 */
	uint8_t level_idc;
	/** Signal Constrained Baseline instead of Baseline */
	bool constrained_baseline;
};

/** Input frame */
struct sw_h264enc_frame {
	/** First byte of the frame */
	const uint8_t *data;
	/** Bytes per line of the Y plane (NV12) or of the packed frame (YUYV) */
	uint32_t pitch;
	/** Layout of data */
	enum sw_h264enc_pixfmt pixfmt;
};

/** Per-frame parameters */
struct sw_h264enc_params {
	/** Encode an IDR frame. The first frame after init is always an IDR frame. */
	bool key_frame;
	/** Target size in bytes of a P frame, which bounds the number of refreshed macroblocks */
	uint32_t budget;
};

/** Encoder state */
struct sw_h264enc {
	struct sw_h264enc_cfg cfg;
	/** Next macroblock to refresh in a P frame */
	uint32_t refresh_mb;
	/** Number of I_PCM macroblocks in the last frame */
	uint32_t coded_mbs;
	uint16_t frame_num;
	uint8_t idr_pic_id;
	bool has_idr;
};

/**
 * @brief Reset the encoder for a new stream.
 *
 * @retval 0 on success
 * @retval -EINVAL if the configuration is not supported
 */
int sw_h264enc_init(struct sw_h264enc *enc, const struct sw_h264enc_cfg *cfg);

/**
 * @brief Write the SPS and PPS NAL units, each with a start code.
 *
 * @retval 0 on success, with the number of bytes written in @p len
 * @retval -ENOSPC if @p cap is too small
 */
int sw_h264enc_headers(struct sw_h264enc *enc, uint8_t *out, size_t cap, size_t *len);

/**
 * @brief Encode one frame as a single slice NAL unit with a start code.
 *
 * @retval 0 on success, with the number of bytes written in @p len
 * @retval -ENOSPC if @p cap is too small, in which case the encoder state is unchanged
 */
int sw_h264enc_encode(struct sw_h264enc *enc, const struct sw_h264enc_frame *in,
		      const struct sw_h264enc_params *params, uint8_t *out, size_t cap,
		      size_t *len);

/** @brief Worst-case size of the SPS, PPS and one frame for a configuration. */
size_t sw_h264enc_max_size(const struct sw_h264enc_cfg *cfg);

#endif /* ZEPHYR_DRIVERS_VIDEO_VIDEO_SW_H264ENC_H_ */

/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Syntax references are to ITU-T H.264 (Annex B byte stream, CAVLC). See video_sw_h264enc.h
 * for an overview.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "video_sw_h264enc.h"

#define NAL_SLICE 1
#define NAL_IDR   5
#define NAL_SPS   7
#define NAL_PPS   8

#define SLICE_TYPE_P 5
#define SLICE_TYPE_I 7

#define MB_TYPE_I_PCM        25
#define MB_TYPE_P_OFFSET     5
#define LOG2_MAX_FRAME_NUM   8
#define MB_PCM_BYTES         (256 + 64 + 64)
/* I_PCM samples plus mb_type, mb_skip_run and alignment, rounded up */
#define MB_CODED_BYTES       (MB_PCM_BYTES + 4)
/* Start code, NAL header, slice header and trailing bits, rounded up */
#define SLICE_OVERHEAD_BYTES 16
#define HEADERS_MAX_BYTES    64

/* Bit writer that inserts emulation prevention bytes as it goes */
struct bitwriter {
	uint8_t *buf;
	size_t cap;
	size_t pos;
	uint8_t acc;
	uint8_t nbits;
	uint8_t zeros;
	bool overflow;
};

static void bw_raw(struct bitwriter *bw, uint8_t byte)
{
	if (bw->pos < bw->cap) {
		bw->buf[bw->pos++] = byte;
	} else {
		bw->overflow = true;
	}
}

/* Append a byte of NAL unit payload, see 7.4.1 emulation_prevention_three_byte */
static void bw_byte(struct bitwriter *bw, uint8_t byte)
{
	if (bw->zeros >= 2 && byte <= 0x03) {
		bw_raw(bw, 0x03);
		bw->zeros = 0;
	}
	bw_raw(bw, byte);
	bw->zeros = (byte == 0) ? bw->zeros + 1 : 0;
}

static void bw_bits(struct bitwriter *bw, uint32_t val, uint8_t n)
{
	while (n-- > 0) {
		bw->acc = (bw->acc << 1) | ((val >> n) & 1);
		if (++bw->nbits == 8) {
			bw_byte(bw, bw->acc);
			bw->acc = 0;
			bw->nbits = 0;
		}
	}
}

/* Exp-Golomb codes, see 9.1 */
static void bw_ue(struct bitwriter *bw, uint32_t val)
{
	uint32_t code = val + 1;
	uint8_t len = 0;

	for (uint32_t tmp = code; tmp != 0; tmp >>= 1) {
		len++;
	}
	bw_bits(bw, 0, len - 1);
	bw_bits(bw, code, len);
}

static void bw_se(struct bitwriter *bw, int32_t val)
{
	bw_ue(bw, val > 0 ? 2 * (uint32_t)val - 1 : 2 * (uint32_t)(-val));
}

static void bw_align_zero(struct bitwriter *bw)
{
	if (bw->nbits != 0) {
		bw_bits(bw, 0, 8 - bw->nbits);
	}
}

static void bw_trailing_bits(struct bitwriter *bw)
{
	bw_bits(bw, 1, 1);
	bw_align_zero(bw);
}

static void bw_nal_start(struct bitwriter *bw, uint8_t nal_ref_idc, uint8_t nal_unit_type)
{
	bw_raw(bw, 0x00);
	bw_raw(bw, 0x00);
	bw_raw(bw, 0x00);
	bw_raw(bw, 0x01);
	bw->zeros = 0;

	bw_bits(bw, 0, 1);
	bw_bits(bw, nal_ref_idc, 2);
	bw_bits(bw, nal_unit_type, 5);
}

/* 7.3.2.1.1 seq_parameter_set_data */
static void write_sps(struct bitwriter *bw, const struct sw_h264enc_cfg *cfg)
{
	bw_nal_start(bw, 3, NAL_SPS);
	bw_bits(bw, 66, 8);                        /* profile_idc: Baseline */
	bw_bits(bw, 1, 1);                         /* constraint_set0_flag */
	bw_bits(bw, cfg->constrained_baseline, 1); /* constraint_set1_flag */
	bw_bits(bw, 0, 6);                         /* constraint_set2..5_flag, reserved bits */
	bw_bits(bw, cfg->level_idc, 8);
	bw_ue(bw, 0); /* seq_parameter_set_id */
	bw_ue(bw, LOG2_MAX_FRAME_NUM - 4);
	bw_ue(bw, 2);      /* pic_order_cnt_type: decoding order */
	bw_ue(bw, 1);      /* max_num_ref_frames */
	bw_bits(bw, 0, 1); /* gaps_in_frame_num_value_allowed_flag */
	bw_ue(bw, cfg->width_mbs - 1);
	bw_ue(bw, cfg->height_mbs - 1);
	bw_bits(bw, 1, 1); /* frame_mbs_only_flag */
	bw_bits(bw, 1, 1); /* direct_8x8_inference_flag */
	bw_bits(bw, 0, 1); /* frame_cropping_flag */
	bw_bits(bw, 0, 1); /* vui_parameters_present_flag */
	bw_trailing_bits(bw);
}

/* 7.3.2.2 pic_parameter_set_rbsp */
static void write_pps(struct bitwriter *bw)
{
	bw_nal_start(bw, 3, NAL_PPS);
	bw_ue(bw, 0);      /* pic_parameter_set_id */
	bw_ue(bw, 0);      /* seq_parameter_set_id */
	bw_bits(bw, 0, 1); /* entropy_coding_mode_flag: CAVLC */
	bw_bits(bw, 0, 1); /* bottom_field_pic_order_in_frame_present_flag */
	bw_ue(bw, 0);      /* num_slice_groups_minus1 */
	bw_ue(bw, 0);      /* num_ref_idx_l0_default_active_minus1 */
	bw_ue(bw, 0);      /* num_ref_idx_l1_default_active_minus1 */
	bw_bits(bw, 0, 1); /* weighted_pred_flag */
	bw_bits(bw, 0, 2); /* weighted_bipred_idc */
	bw_se(bw, 0);      /* pic_init_qp_minus26 */
	bw_se(bw, 0);      /* pic_init_qs_minus26 */
	bw_se(bw, 0);      /* chroma_qp_index_offset */
	bw_bits(bw, 1, 1); /* deblocking_filter_control_present_flag */
	bw_bits(bw, 0, 1); /* constrained_intra_pred_flag */
	bw_bits(bw, 0, 1); /* redundant_pic_cnt_present_flag */
	bw_trailing_bits(bw);
}

/* 7.3.5 macroblock_layer with mb_type I_PCM, converting the input to 4:2:0 */
static void write_pcm_mb(struct bitwriter *bw, const struct sw_h264enc *enc,
			 const struct sw_h264enc_frame *in, uint32_t mb, uint32_t mb_type)
{
	uint32_t x0 = (mb % enc->cfg.width_mbs) * 16;
	uint32_t y0 = (mb / enc->cfg.width_mbs) * 16;
	uint32_t pitch = in->pitch;

	bw_ue(bw, mb_type);
	bw_align_zero(bw); /* pcm_alignment_zero_bit */

	if (in->pixfmt == SW_H264ENC_NV12) {
		const uint8_t *uv = in->data + pitch * enc->cfg.height_mbs * 16;

		for (uint32_t y = 0; y < 16; y++) {
			const uint8_t *row = in->data + (y0 + y) * pitch + x0;

			for (uint32_t x = 0; x < 16; x++) {
				bw_byte(bw, row[x]);
			}
		}
		for (uint32_t c = 0; c < 2; c++) {
			for (uint32_t y = 0; y < 8; y++) {
				const uint8_t *row = uv + (y0 / 2 + y) * pitch + x0 + c;

				for (uint32_t x = 0; x < 8; x++) {
					bw_byte(bw, row[2 * x]);
				}
			}
		}
	} else {
		for (uint32_t y = 0; y < 16; y++) {
			const uint8_t *row = in->data + (y0 + y) * pitch + 2 * x0;

			for (uint32_t x = 0; x < 16; x++) {
				bw_byte(bw, row[2 * x]);
			}
		}
		/* Cb at offset 1 and Cr at offset 3 of each pixel pair, averaged over two lines */
		for (uint32_t c = 0; c < 2; c++) {
			uint32_t offset = (c == 0) ? 1 : 3;

			for (uint32_t y = 0; y < 8; y++) {
				const uint8_t *row0 = in->data + (y0 + 2 * y) * pitch + 2 * x0;
				const uint8_t *row1 = row0 + pitch;

				for (uint32_t x = 0; x < 8; x++) {
					bw_byte(bw, (row0[4 * x + offset] + row1[4 * x + offset] +
						     1) / 2);
				}
			}
		}
	}
}

static uint32_t num_mbs(const struct sw_h264enc_cfg *cfg)
{
	return (uint32_t)cfg->width_mbs * cfg->height_mbs;
}

int sw_h264enc_init(struct sw_h264enc *enc, const struct sw_h264enc_cfg *cfg)
{
	if (cfg->width_mbs == 0 || cfg->height_mbs == 0 || cfg->level_idc == 0) {
		return -EINVAL;
	}

	*enc = (struct sw_h264enc){.cfg = *cfg};

	return 0;
}

size_t sw_h264enc_max_size(const struct sw_h264enc_cfg *cfg)
{
	/* Emulation prevention adds at most one byte for every two bytes of payload */
	return HEADERS_MAX_BYTES +
	       (SLICE_OVERHEAD_BYTES + (size_t)num_mbs(cfg) * MB_CODED_BYTES) * 3 / 2;
}

int sw_h264enc_headers(struct sw_h264enc *enc, uint8_t *out, size_t cap, size_t *len)
{
	struct bitwriter bw = {.buf = out, .cap = cap};

	write_sps(&bw, &enc->cfg);
	write_pps(&bw);
	if (bw.overflow) {
		return -ENOSPC;
	}

	*len = bw.pos;

	return 0;
}

/* 7.3.3 slice_header for an IDR frame, then 7.3.4 slice_data of I_PCM macroblocks */
static void write_idr(struct bitwriter *bw, const struct sw_h264enc *enc,
		      const struct sw_h264enc_frame *in)
{
	bw_nal_start(bw, 3, NAL_IDR);
	bw_ue(bw, 0); /* first_mb_in_slice */
	bw_ue(bw, SLICE_TYPE_I);
	bw_ue(bw, 0);                       /* pic_parameter_set_id */
	bw_bits(bw, 0, LOG2_MAX_FRAME_NUM); /* frame_num */
	bw_ue(bw, enc->idr_pic_id);
	bw_bits(bw, 0, 1); /* no_output_of_prior_pics_flag */
	bw_bits(bw, 0, 1); /* long_term_reference_flag */
	bw_se(bw, 0);      /* slice_qp_delta */
	bw_ue(bw, 1);      /* disable_deblocking_filter_idc */

	for (uint32_t mb = 0; mb < num_mbs(&enc->cfg); mb++) {
		write_pcm_mb(bw, enc, in, mb, MB_TYPE_I_PCM);
	}
	bw_trailing_bits(bw);
}

/*
 * 7.3.3 slice_header for a P frame, then 7.3.4 slice_data where macroblocks
 * [first, first + count) modulo the frame size are I_PCM and the others are skipped.
 * Skipped macroblocks always predict a zero motion vector here (8.4.1.1), as no
 * macroblock carries motion, so they are exact copies of the previous frame.
 */
static void write_p(struct bitwriter *bw, const struct sw_h264enc *enc,
		    const struct sw_h264enc_frame *in, uint32_t first, uint32_t count)
{
	uint32_t total = num_mbs(&enc->cfg);
	uint32_t skip_run = 0;

	bw_nal_start(bw, 3, NAL_SLICE);
	bw_ue(bw, 0); /* first_mb_in_slice */
	bw_ue(bw, SLICE_TYPE_P);
	bw_ue(bw, 0); /* pic_parameter_set_id */
	bw_bits(bw, enc->frame_num, LOG2_MAX_FRAME_NUM);
	bw_bits(bw, 0, 1); /* num_ref_idx_active_override_flag */
	bw_bits(bw, 0, 1); /* ref_pic_list_modification_flag_l0 */
	bw_bits(bw, 0, 1); /* adaptive_ref_pic_marking_mode_flag */
	bw_se(bw, 0);      /* slice_qp_delta */
	bw_ue(bw, 1);      /* disable_deblocking_filter_idc */

	for (uint32_t mb = 0; mb < total; mb++) {
		if ((mb + total - first) % total >= count) {
			skip_run++;
			continue;
		}
		bw_ue(bw, skip_run);
		skip_run = 0;
		write_pcm_mb(bw, enc, in, mb, MB_TYPE_P_OFFSET + MB_TYPE_I_PCM);
	}
	if (skip_run > 0) {
		bw_ue(bw, skip_run);
	}
	bw_trailing_bits(bw);
}

int sw_h264enc_encode(struct sw_h264enc *enc, const struct sw_h264enc_frame *in,
		      const struct sw_h264enc_params *params, uint8_t *out, size_t cap, size_t *len)
{
	struct bitwriter bw = {.buf = out, .cap = cap};
	uint32_t total = num_mbs(&enc->cfg);
	bool key_frame = params->key_frame || !enc->has_idr;
	uint32_t count = total;

	if (key_frame) {
		write_idr(&bw, enc, in);
	} else {
		count = 0;
		if (params->budget > SLICE_OVERHEAD_BYTES) {
			count = (params->budget - SLICE_OVERHEAD_BYTES) / MB_CODED_BYTES;
		}
		count = count < total ? count : total;
		write_p(&bw, enc, in, enc->refresh_mb, count);
	}

	if (bw.overflow) {
		return -ENOSPC;
	}

	if (key_frame) {
		enc->has_idr = true;
		enc->idr_pic_id ^= 1;
		enc->frame_num = 1;
		enc->refresh_mb = 0;
	} else {
		enc->frame_num = (enc->frame_num + 1) % (1 << LOG2_MAX_FRAME_NUM);
		enc->refresh_mb = (enc->refresh_mb + count) % total;
	}
	enc->coded_mbs = count;
	*len = bw.pos;

	return 0;
}

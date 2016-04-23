/*
 * MPEG-4 decoder
 * Copyright (c) 2000,2001 Fabrice Bellard
 * Copyright (c) 2002-2010 Michael Niedermayer <michaelni@gmx.at>
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "bitstream.h"
#include "error_resilience.h"
#include "idctdsp.h"
#include "internal.h"
#include "mpegutils.h"
#include "mpegvideo.h"
#include "mpegvideodata.h"
#include "mpeg4video.h"
#include "h263.h"
#include "profiles.h"
#include "thread.h"
#include "vlc.h"
#include "xvididct.h"

/* The defines below define the number of bits that are read at once for
 * reading vlc values. Changing these may improve speed and data cache needs
 * be aware though that decreasing them may need the number of stages that is
 * passed to get_vlc* to be increased. */
#define SPRITE_TRAJ_VLC_BITS 6
#define DC_VLC_BITS 9
#define MB_TYPE_B_VLC_BITS 4

static VLC dc_lum, dc_chrom;
static VLC sprite_trajectory;
static VLC mb_type_b_vlc;

static const int mb_type_b_map[4] = {
    MB_TYPE_DIRECT2 | MB_TYPE_L0L1,
    MB_TYPE_L0L1    | MB_TYPE_16x16,
    MB_TYPE_L1      | MB_TYPE_16x16,
    MB_TYPE_L0      | MB_TYPE_16x16,
};

static inline int check_marker(AVCodecContext *avctx, BitstreamContext *bc, const char *msg)
{
    int bit = bitstream_read_bit(bc);
    if (!bit)
        av_log(avctx, AV_LOG_INFO, "Marker bit missing %s\n", msg);

    return bit;
}

/**
 * Predict the ac.
 * @param n block index (0-3 are luma, 4-5 are chroma)
 * @param dir the ac prediction direction
 */
void ff_mpeg4_pred_ac(MpegEncContext *s, int16_t *block, int n, int dir)
{
    int i;
    int16_t *ac_val, *ac_val1;
    int8_t *const qscale_table = s->current_picture.qscale_table;

    /* find prediction */
    ac_val  = s->ac_val[0][0] + s->block_index[n] * 16;
    ac_val1 = ac_val;
    if (s->ac_pred) {
        if (dir == 0) {
            const int xy = s->mb_x - 1 + s->mb_y * s->mb_stride;
            /* left prediction */
            ac_val -= 16;

            if (s->mb_x == 0 || s->qscale == qscale_table[xy] ||
                n == 1 || n == 3) {
                /* same qscale */
                for (i = 1; i < 8; i++)
                    block[s->idsp.idct_permutation[i << 3]] += ac_val[i];
            } else {
                /* different qscale, we must rescale */
                for (i = 1; i < 8; i++)
                    block[s->idsp.idct_permutation[i << 3]] += ROUNDED_DIV(ac_val[i] * qscale_table[xy], s->qscale);
            }
        } else {
            const int xy = s->mb_x + s->mb_y * s->mb_stride - s->mb_stride;
            /* top prediction */
            ac_val -= 16 * s->block_wrap[n];

            if (s->mb_y == 0 || s->qscale == qscale_table[xy] ||
                n == 2 || n == 3) {
                /* same qscale */
                for (i = 1; i < 8; i++)
                    block[s->idsp.idct_permutation[i]] += ac_val[i + 8];
            } else {
                /* different qscale, we must rescale */
                for (i = 1; i < 8; i++)
                    block[s->idsp.idct_permutation[i]] += ROUNDED_DIV(ac_val[i + 8] * qscale_table[xy], s->qscale);
            }
        }
    }
    /* left copy */
    for (i = 1; i < 8; i++)
        ac_val1[i] = block[s->idsp.idct_permutation[i << 3]];

    /* top copy */
    for (i = 1; i < 8; i++)
        ac_val1[8 + i] = block[s->idsp.idct_permutation[i]];
}

/**
 * check if the next stuff is a resync marker or the end.
 * @return 0 if not
 */
static inline int mpeg4_is_resync(MpegEncContext *s)
{
    int bits_count = bitstream_tell(&s->bc);
    int v          = bitstream_peek(&s->bc, 16);

    if (s->workaround_bugs & FF_BUG_NO_PADDING)
        return 0;

    while (v <= 0xFF) {
        if (s->pict_type == AV_PICTURE_TYPE_B ||
            (v >> (8 - s->pict_type) != 1) || s->partitioned_frame)
            break;
        bitstream_skip(&s->bc, 8 + s->pict_type);
        bits_count += 8 + s->pict_type;
        v = bitstream_peek(&s->bc, 16);
    }

    if (bitstream_bits_left(&s->bc) <= 8) {
        v >>= 8;
        v  |= 0x7F >> (7 - (bits_count & 7));

        if (v == 0x7F)
            return 1;
    } else {
        if (v == ff_mpeg4_resync_prefix[bits_count & 7]) {
            int len;
            BitstreamContext bc = s->bc;

            bitstream_skip(&s->bc, 1);
            bitstream_align(&s->bc);

            for (len = 0; len < 32; len++)
                if (bitstream_read_bit(&s->bc))
                    break;

            s->bc = bc;

            if (len >= ff_mpeg4_get_video_packet_prefix_length(s))
                return 1;
        }
    }
    return 0;
}

static int mpeg4_decode_sprite_trajectory(Mpeg4DecContext *ctx, BitstreamContext *bc)
{
    MpegEncContext *s = &ctx->m;
    int a     = 2 << s->sprite_warping_accuracy;
    int rho   = 3  - s->sprite_warping_accuracy;
    int r     = 16 / a;
    int alpha = 0;
    int beta  = 0;
    int w     = s->width;
    int h     = s->height;
    int min_ab, i, w2, h2, w3, h3;
    int sprite_ref[4][2];
    int virtual_ref[2][2];

    // only true for rectangle shapes
    const int vop_ref[4][2] = { { 0, 0 },         { s->width, 0 },
                                { 0, s->height }, { s->width, s->height } };
    int d[4][2]             = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };

    if (w <= 0 || h <= 0)
        return AVERROR_INVALIDDATA;

    for (i = 0; i < ctx->num_sprite_warping_points; i++) {
        int length;
        int x = 0, y = 0;

        length = bitstream_read_vlc(bc, sprite_trajectory.table, SPRITE_TRAJ_VLC_BITS, 3);
        if (length)
            x = bitstream_read_xbits(bc, length);

        if (!(ctx->divx_version == 500 && ctx->divx_build == 413))
            bitstream_skip(bc, 1);     /* marker bit */

        length = bitstream_read_vlc(bc, sprite_trajectory.table, SPRITE_TRAJ_VLC_BITS, 3);
        if (length)
            y = bitstream_read_xbits(bc, length);

        bitstream_skip(bc, 1);         /* marker bit */
        ctx->sprite_traj[i][0] = d[i][0] = x;
        ctx->sprite_traj[i][1] = d[i][1] = y;
    }
    for (; i < 4; i++)
        ctx->sprite_traj[i][0] = ctx->sprite_traj[i][1] = 0;

    while ((1 << alpha) < w)
        alpha++;
    while ((1 << beta) < h)
        beta++;  /* typo in the MPEG-4 std for the definition of w' and h' */
    w2 = 1 << alpha;
    h2 = 1 << beta;

    // Note, the 4th point isn't used for GMC
    if (ctx->divx_version == 500 && ctx->divx_build == 413) {
        sprite_ref[0][0] = a * vop_ref[0][0] + d[0][0];
        sprite_ref[0][1] = a * vop_ref[0][1] + d[0][1];
        sprite_ref[1][0] = a * vop_ref[1][0] + d[0][0] + d[1][0];
        sprite_ref[1][1] = a * vop_ref[1][1] + d[0][1] + d[1][1];
        sprite_ref[2][0] = a * vop_ref[2][0] + d[0][0] + d[2][0];
        sprite_ref[2][1] = a * vop_ref[2][1] + d[0][1] + d[2][1];
    } else {
        sprite_ref[0][0] = (a >> 1) * (2 * vop_ref[0][0] + d[0][0]);
        sprite_ref[0][1] = (a >> 1) * (2 * vop_ref[0][1] + d[0][1]);
        sprite_ref[1][0] = (a >> 1) * (2 * vop_ref[1][0] + d[0][0] + d[1][0]);
        sprite_ref[1][1] = (a >> 1) * (2 * vop_ref[1][1] + d[0][1] + d[1][1]);
        sprite_ref[2][0] = (a >> 1) * (2 * vop_ref[2][0] + d[0][0] + d[2][0]);
        sprite_ref[2][1] = (a >> 1) * (2 * vop_ref[2][1] + d[0][1] + d[2][1]);
    }
    /* sprite_ref[3][0] = (a >> 1) * (2 * vop_ref[3][0] + d[0][0] + d[1][0] + d[2][0] + d[3][0]);
     * sprite_ref[3][1] = (a >> 1) * (2 * vop_ref[3][1] + d[0][1] + d[1][1] + d[2][1] + d[3][1]); */

    /* This is mostly identical to the MPEG-4 std (and is totally unreadable
     * because of that...). Perhaps it should be reordered to be more readable.
     * The idea behind this virtual_ref mess is to be able to use shifts later
     * per pixel instead of divides so the distance between points is converted
     * from w&h based to w2&h2 based which are of the 2^x form. */
    virtual_ref[0][0] = 16 * (vop_ref[0][0] + w2) +
                         ROUNDED_DIV(((w - w2) *
                                      (r * sprite_ref[0][0] - 16 * vop_ref[0][0]) +
                                      w2 * (r * sprite_ref[1][0] - 16 * vop_ref[1][0])), w);
    virtual_ref[0][1] = 16 * vop_ref[0][1] +
                        ROUNDED_DIV(((w - w2) *
                                     (r * sprite_ref[0][1] - 16 * vop_ref[0][1]) +
                                     w2 * (r * sprite_ref[1][1] - 16 * vop_ref[1][1])), w);
    virtual_ref[1][0] = 16 * vop_ref[0][0] +
                        ROUNDED_DIV(((h - h2) * (r * sprite_ref[0][0] - 16 * vop_ref[0][0]) +
                                     h2 * (r * sprite_ref[2][0] - 16 * vop_ref[2][0])), h);
    virtual_ref[1][1] = 16 * (vop_ref[0][1] + h2) +
                        ROUNDED_DIV(((h - h2) * (r * sprite_ref[0][1] - 16 * vop_ref[0][1]) +
                                     h2 * (r * sprite_ref[2][1] - 16 * vop_ref[2][1])), h);

    switch (ctx->num_sprite_warping_points) {
    case 0:
        s->sprite_offset[0][0] =
        s->sprite_offset[0][1] =
        s->sprite_offset[1][0] =
        s->sprite_offset[1][1] = 0;
        s->sprite_delta[0][0]  = a;
        s->sprite_delta[0][1]  =
        s->sprite_delta[1][0]  = 0;
        s->sprite_delta[1][1]  = a;
        ctx->sprite_shift[0]   =
        ctx->sprite_shift[1]   = 0;
        break;
    case 1:     // GMC only
        s->sprite_offset[0][0] = sprite_ref[0][0] - a * vop_ref[0][0];
        s->sprite_offset[0][1] = sprite_ref[0][1] - a * vop_ref[0][1];
        s->sprite_offset[1][0] = ((sprite_ref[0][0] >> 1) | (sprite_ref[0][0] & 1)) -
                                 a * (vop_ref[0][0] / 2);
        s->sprite_offset[1][1] = ((sprite_ref[0][1] >> 1) | (sprite_ref[0][1] & 1)) -
                                 a * (vop_ref[0][1] / 2);
        s->sprite_delta[0][0]  = a;
        s->sprite_delta[0][1]  =
        s->sprite_delta[1][0]  = 0;
        s->sprite_delta[1][1]  = a;
        ctx->sprite_shift[0]   =
        ctx->sprite_shift[1]   = 0;
        break;
    case 2:
        s->sprite_offset[0][0] = (sprite_ref[0][0] << (alpha + rho)) +
                                 (-r * sprite_ref[0][0] + virtual_ref[0][0]) *
                                 (-vop_ref[0][0]) +
                                 (r * sprite_ref[0][1] - virtual_ref[0][1]) *
                                 (-vop_ref[0][1]) + (1 << (alpha + rho - 1));
        s->sprite_offset[0][1] = (sprite_ref[0][1] << (alpha + rho)) +
                                 (-r * sprite_ref[0][1] + virtual_ref[0][1]) *
                                 (-vop_ref[0][0]) +
                                 (-r * sprite_ref[0][0] + virtual_ref[0][0]) *
                                 (-vop_ref[0][1]) + (1 << (alpha + rho - 1));
        s->sprite_offset[1][0] = ((-r * sprite_ref[0][0] + virtual_ref[0][0]) *
                                  (-2 * vop_ref[0][0] + 1) +
                                  (r * sprite_ref[0][1] - virtual_ref[0][1]) *
                                  (-2 * vop_ref[0][1] + 1) + 2 * w2 * r *
                                  sprite_ref[0][0] - 16 * w2 + (1 << (alpha + rho + 1)));
        s->sprite_offset[1][1] = ((-r * sprite_ref[0][1] + virtual_ref[0][1]) *
                                  (-2 * vop_ref[0][0] + 1) +
                                  (-r * sprite_ref[0][0] + virtual_ref[0][0]) *
                                  (-2 * vop_ref[0][1] + 1) + 2 * w2 * r *
                                  sprite_ref[0][1] - 16 * w2 + (1 << (alpha + rho + 1)));
        s->sprite_delta[0][0] = (-r * sprite_ref[0][0] + virtual_ref[0][0]);
        s->sprite_delta[0][1] = (+r * sprite_ref[0][1] - virtual_ref[0][1]);
        s->sprite_delta[1][0] = (-r * sprite_ref[0][1] + virtual_ref[0][1]);
        s->sprite_delta[1][1] = (-r * sprite_ref[0][0] + virtual_ref[0][0]);

        ctx->sprite_shift[0]  = alpha + rho;
        ctx->sprite_shift[1]  = alpha + rho + 2;
        break;
    case 3:
        min_ab = FFMIN(alpha, beta);
        w3     = w2 >> min_ab;
        h3     = h2 >> min_ab;
        s->sprite_offset[0][0] = (sprite_ref[0][0] << (alpha + beta + rho - min_ab)) +
                                 (-r * sprite_ref[0][0] + virtual_ref[0][0]) *
                                 h3 * (-vop_ref[0][0]) +
                                 (-r * sprite_ref[0][0] + virtual_ref[1][0]) *
                                 w3 * (-vop_ref[0][1]) +
                                 (1 << (alpha + beta + rho - min_ab - 1));
        s->sprite_offset[0][1] = (sprite_ref[0][1] << (alpha + beta + rho - min_ab)) +
                                 (-r * sprite_ref[0][1] + virtual_ref[0][1]) *
                                 h3 * (-vop_ref[0][0]) +
                                 (-r * sprite_ref[0][1] + virtual_ref[1][1]) *
                                 w3 * (-vop_ref[0][1]) +
                                 (1 << (alpha + beta + rho - min_ab - 1));
        s->sprite_offset[1][0] = (-r * sprite_ref[0][0] + virtual_ref[0][0]) *
                                 h3 * (-2 * vop_ref[0][0] + 1) +
                                 (-r * sprite_ref[0][0] + virtual_ref[1][0]) *
                                 w3 * (-2 * vop_ref[0][1] + 1) + 2 * w2 * h3 *
                                 r * sprite_ref[0][0] - 16 * w2 * h3 +
                                 (1 << (alpha + beta + rho - min_ab + 1));
        s->sprite_offset[1][1] = (-r * sprite_ref[0][1] + virtual_ref[0][1]) *
                                 h3 * (-2 * vop_ref[0][0] + 1) +
                                 (-r * sprite_ref[0][1] + virtual_ref[1][1]) *
                                 w3 * (-2 * vop_ref[0][1] + 1) + 2 * w2 * h3 *
                                 r * sprite_ref[0][1] - 16 * w2 * h3 +
                                 (1 << (alpha + beta + rho - min_ab + 1));
        s->sprite_delta[0][0] = (-r * sprite_ref[0][0] + virtual_ref[0][0]) * h3;
        s->sprite_delta[0][1] = (-r * sprite_ref[0][0] + virtual_ref[1][0]) * w3;
        s->sprite_delta[1][0] = (-r * sprite_ref[0][1] + virtual_ref[0][1]) * h3;
        s->sprite_delta[1][1] = (-r * sprite_ref[0][1] + virtual_ref[1][1]) * w3;

        ctx->sprite_shift[0]  = alpha + beta + rho - min_ab;
        ctx->sprite_shift[1]  = alpha + beta + rho - min_ab + 2;
        break;
    }
    /* try to simplify the situation */
    if (s->sprite_delta[0][0] == a << ctx->sprite_shift[0] &&
        s->sprite_delta[0][1] == 0 &&
        s->sprite_delta[1][0] == 0 &&
        s->sprite_delta[1][1] == a << ctx->sprite_shift[0]) {
        s->sprite_offset[0][0] >>= ctx->sprite_shift[0];
        s->sprite_offset[0][1] >>= ctx->sprite_shift[0];
        s->sprite_offset[1][0] >>= ctx->sprite_shift[1];
        s->sprite_offset[1][1] >>= ctx->sprite_shift[1];
        s->sprite_delta[0][0] = a;
        s->sprite_delta[0][1] = 0;
        s->sprite_delta[1][0] = 0;
        s->sprite_delta[1][1] = a;
        ctx->sprite_shift[0] = 0;
        ctx->sprite_shift[1] = 0;
        s->real_sprite_warping_points = 1;
    } else {
        int shift_y = 16 - ctx->sprite_shift[0];
        int shift_c = 16 - ctx->sprite_shift[1];
        for (i = 0; i < 2; i++) {
            s->sprite_offset[0][i] <<= shift_y;
            s->sprite_offset[1][i] <<= shift_c;
            s->sprite_delta[0][i]  <<= shift_y;
            s->sprite_delta[1][i]  <<= shift_y;
            ctx->sprite_shift[i]     = 16;
        }
        s->real_sprite_warping_points = ctx->num_sprite_warping_points;
    }

    return 0;
}

/**
 * Decode the next video packet.
 * @return <0 if something went wrong
 */
int ff_mpeg4_decode_video_packet_header(Mpeg4DecContext *ctx)
{
    MpegEncContext *s = &ctx->m;

    int mb_num_bits      = av_log2(s->mb_num - 1) + 1;
    int header_extension = 0, mb_num, len;

    /* is there enough space left for a video packet + header */
    if (bitstream_bits_left(&s->bc) < 20) {
        return -1;
    }

    for (len = 0; len < 32; len++)
        if (bitstream_read_bit(&s->bc))
            break;

    if (len != ff_mpeg4_get_video_packet_prefix_length(s)) {
        av_log(s->avctx, AV_LOG_ERROR, "marker does not match f_code\n");
        return -1;
    }

    if (ctx->shape != RECT_SHAPE) {
        header_extension = bitstream_read_bit(&s->bc);
        // FIXME more stuff here
    }

    mb_num = bitstream_read(&s->bc, mb_num_bits);
    if (mb_num >= s->mb_num) {
        av_log(s->avctx, AV_LOG_ERROR,
               "illegal mb_num in video packet (%d %d) \n", mb_num, s->mb_num);
        return -1;
    }
    if (s->pict_type == AV_PICTURE_TYPE_B) {
        int mb_x = 0, mb_y = 0;

        while (s->next_picture.mbskip_table[s->mb_index2xy[mb_num]]) {
            if (!mb_x)
                ff_thread_await_progress(&s->next_picture_ptr->tf, mb_y++, 0);
            mb_num++;
            if (++mb_x == s->mb_width)
                mb_x = 0;
        }
        if (mb_num >= s->mb_num)
            return -1;  // slice contains just skipped MBs (already decoded)
    }

    s->mb_x = mb_num % s->mb_width;
    s->mb_y = mb_num / s->mb_width;

    if (ctx->shape != BIN_ONLY_SHAPE) {
        int qscale = bitstream_read(&s->bc, s->quant_precision);
        if (qscale)
            s->chroma_qscale = s->qscale = qscale;
    }

    if (ctx->shape == RECT_SHAPE)
        header_extension = bitstream_read_bit(&s->bc);

    if (header_extension) {
        int time_incr = 0;

        while (bitstream_read_bit(&s->bc) != 0)
            time_incr++;

        check_marker(s->avctx, &s->bc, "before time_increment in video packed header");
        bitstream_skip(&s->bc, ctx->time_increment_bits);      /* time_increment */

        bitstream_skip(&s->bc, 2); /* vop coding type */
        // FIXME not rect stuff here

        if (ctx->shape != BIN_ONLY_SHAPE) {
            bitstream_skip(&s->bc, 3); /* intra dc vlc threshold */
            // FIXME don't just ignore everything
            if (s->pict_type == AV_PICTURE_TYPE_S &&
                ctx->vol_sprite_usage == GMC_SPRITE) {
                if (mpeg4_decode_sprite_trajectory(ctx, &s->bc) < 0)
                    return AVERROR_INVALIDDATA;
                av_log(s->avctx, AV_LOG_ERROR, "untested\n");
            }

            // FIXME reduced res stuff here

            if (s->pict_type != AV_PICTURE_TYPE_I) {
                int f_code = bitstream_read(&s->bc, 3);       /* fcode_for */
                if (f_code == 0)
                    av_log(s->avctx, AV_LOG_ERROR,
                           "Error, video packet header damaged (f_code=0)\n");
            }
            if (s->pict_type == AV_PICTURE_TYPE_B) {
                int b_code = bitstream_read(&s->bc, 3);
                if (b_code == 0)
                    av_log(s->avctx, AV_LOG_ERROR,
                           "Error, video packet header damaged (b_code=0)\n");
            }
        }
    }
    // FIXME new-pred stuff

    return 0;
}

/**
 * Get the average motion vector for a GMC MB.
 * @param n either 0 for the x component or 1 for y
 * @return the average MV for a GMC MB
 */
static inline int get_amv(Mpeg4DecContext *ctx, int n)
{
    MpegEncContext *s = &ctx->m;
    int x, y, mb_v, sum, dx, dy, shift;
    int len     = 1 << (s->f_code + 4);
    const int a = s->sprite_warping_accuracy;

    if (s->workaround_bugs & FF_BUG_AMV)
        len >>= s->quarter_sample;

    if (s->real_sprite_warping_points == 1) {
        if (ctx->divx_version == 500 && ctx->divx_build == 413)
            sum = s->sprite_offset[0][n] / (1 << (a - s->quarter_sample));
        else
            sum = RSHIFT(s->sprite_offset[0][n] << s->quarter_sample, a);
    } else {
        dx    = s->sprite_delta[n][0];
        dy    = s->sprite_delta[n][1];
        shift = ctx->sprite_shift[0];
        if (n)
            dy -= 1 << (shift + a + 1);
        else
            dx -= 1 << (shift + a + 1);
        mb_v = s->sprite_offset[0][n] + dx * s->mb_x * 16 + dy * s->mb_y * 16;

        sum = 0;
        for (y = 0; y < 16; y++) {
            int v;

            v = mb_v + dy * y;
            // FIXME optimize
            for (x = 0; x < 16; x++) {
                sum += v >> shift;
                v   += dx;
            }
        }
        sum = RSHIFT(sum, a + 8 - s->quarter_sample);
    }

    if (sum < -len)
        sum = -len;
    else if (sum >= len)
        sum = len - 1;

    return sum;
}

/**
 * Decode the dc value.
 * @param n block index (0-3 are luma, 4-5 are chroma)
 * @param dir_ptr the prediction direction will be stored here
 * @return the quantized dc
 */
static inline int mpeg4_decode_dc(MpegEncContext *s, int n, int *dir_ptr)
{
    int level, code;

    if (n < 4)
        code = bitstream_read_vlc(&s->bc, dc_lum.table, DC_VLC_BITS, 1);
    else
        code = bitstream_read_vlc(&s->bc, dc_chrom.table, DC_VLC_BITS, 1);

    if (code < 0 || code > 9 /* && s->nbit < 9 */) {
        av_log(s->avctx, AV_LOG_ERROR, "illegal dc vlc\n");
        return -1;
    }

    if (code == 0) {
        level = 0;
    } else {
        if (IS_3IV1) {
            if (code == 1)
                level = 2 * bitstream_read_bit(&s->bc) - 1;
            else {
                if (bitstream_read_bit(&s->bc))
                    level = bitstream_read(&s->bc, code - 1) + (1 << (code - 1));
                else
                    level = -bitstream_read(&s->bc, code - 1) - (1 << (code - 1));
            }
        } else
            level = bitstream_read_xbits(&s->bc, code);

        if (code > 8) {
            if (bitstream_read_bit(&s->bc) == 0) { /* marker */
                if (s->avctx->err_recognition & AV_EF_BITSTREAM) {
                    av_log(s->avctx, AV_LOG_ERROR, "dc marker bit missing\n");
                    return -1;
                }
            }
        }
    }

    return ff_mpeg4_pred_dc(s, n, level, dir_ptr, 0);
}

/**
 * Decode first partition.
 * @return number of MBs decoded or <0 if an error occurred
 */
static int mpeg4_decode_partition_a(Mpeg4DecContext *ctx)
{
    MpegEncContext *s = &ctx->m;
    int mb_num = 0;
    static const int8_t quant_tab[4] = { -1, -2, 1, 2 };

    /* decode first partition */
    s->first_slice_line = 1;
    for (; s->mb_y < s->mb_height; s->mb_y++) {
        ff_init_block_index(s);
        for (; s->mb_x < s->mb_width; s->mb_x++) {
            const int xy = s->mb_x + s->mb_y * s->mb_stride;
            int cbpc;
            int dir = 0;

            mb_num++;
            ff_update_block_index(s);
            if (s->mb_x == s->resync_mb_x && s->mb_y == s->resync_mb_y + 1)
                s->first_slice_line = 0;

            if (s->pict_type == AV_PICTURE_TYPE_I) {
                int i;

                do {
                    if (bitstream_peek(&s->bc, 19) == DC_MARKER)
                        return mb_num - 1;

                    cbpc = bitstream_read_vlc(&s->bc, ff_h263_intra_MCBPC_vlc.table,
                                              INTRA_MCBPC_VLC_BITS, 2);
                    if (cbpc < 0) {
                        av_log(s->avctx, AV_LOG_ERROR,
                               "cbpc corrupted at %d %d\n", s->mb_x, s->mb_y);
                        return -1;
                    }
                } while (cbpc == 8);

                s->cbp_table[xy]               = cbpc & 3;
                s->current_picture.mb_type[xy] = MB_TYPE_INTRA;
                s->mb_intra                    = 1;

                if (cbpc & 4)
                    ff_set_qscale(s, s->qscale + quant_tab[bitstream_read(&s->bc, 2)]);

                s->current_picture.qscale_table[xy] = s->qscale;

                s->mbintra_table[xy] = 1;
                for (i = 0; i < 6; i++) {
                    int dc_pred_dir;
                    int dc = mpeg4_decode_dc(s, i, &dc_pred_dir);
                    if (dc < 0) {
                        av_log(s->avctx, AV_LOG_ERROR,
                               "DC corrupted at %d %d\n", s->mb_x, s->mb_y);
                        return -1;
                    }
                    dir <<= 1;
                    if (dc_pred_dir)
                        dir |= 1;
                }
                s->pred_dir_table[xy] = dir;
            } else { /* P/S_TYPE */
                int mx, my, pred_x, pred_y, bits;
                int16_t *const mot_val = s->current_picture.motion_val[0][s->block_index[0]];
                const int stride       = s->b8_stride * 2;

try_again:
                bits = bitstream_peek(&s->bc, 17);
                if (bits == MOTION_MARKER)
                    return mb_num - 1;

                bitstream_skip(&s->bc, 1);
                if (bits & 0x10000) {
                    /* skip mb */
                    if (s->pict_type == AV_PICTURE_TYPE_S &&
                        ctx->vol_sprite_usage == GMC_SPRITE) {
                        s->current_picture.mb_type[xy] = MB_TYPE_SKIP  |
                                                         MB_TYPE_16x16 |
                                                         MB_TYPE_GMC   |
                                                         MB_TYPE_L0;
                        mx = get_amv(ctx, 0);
                        my = get_amv(ctx, 1);
                    } else {
                        s->current_picture.mb_type[xy] = MB_TYPE_SKIP  |
                                                         MB_TYPE_16x16 |
                                                         MB_TYPE_L0;
                        mx = my = 0;
                    }
                    mot_val[0]          =
                    mot_val[2]          =
                    mot_val[0 + stride] =
                    mot_val[2 + stride] = mx;
                    mot_val[1]          =
                    mot_val[3]          =
                    mot_val[1 + stride] =
                    mot_val[3 + stride] = my;

                    if (s->mbintra_table[xy])
                        ff_clean_intra_table_entries(s);
                    continue;
                }

                cbpc = bitstream_read_vlc(&s->bc, ff_h263_inter_MCBPC_vlc.table,
                                          INTER_MCBPC_VLC_BITS, 2);
                if (cbpc < 0) {
                    av_log(s->avctx, AV_LOG_ERROR,
                           "cbpc corrupted at %d %d\n", s->mb_x, s->mb_y);
                    return -1;
                }
                if (cbpc == 20)
                    goto try_again;

                s->cbp_table[xy] = cbpc & (8 + 3);  // 8 is dquant

                s->mb_intra = ((cbpc & 4) != 0);

                if (s->mb_intra) {
                    s->current_picture.mb_type[xy] = MB_TYPE_INTRA;
                    s->mbintra_table[xy] = 1;
                    mot_val[0]          =
                    mot_val[2]          =
                    mot_val[0 + stride] =
                    mot_val[2 + stride] = 0;
                    mot_val[1]          =
                    mot_val[3]          =
                    mot_val[1 + stride] =
                    mot_val[3 + stride] = 0;
                } else {
                    if (s->mbintra_table[xy])
                        ff_clean_intra_table_entries(s);

                    if (s->pict_type == AV_PICTURE_TYPE_S &&
                        ctx->vol_sprite_usage == GMC_SPRITE &&
                        (cbpc & 16) == 0)
                        s->mcsel = bitstream_read_bit(&s->bc);
                    else
                        s->mcsel = 0;

                    if ((cbpc & 16) == 0) {
                        /* 16x16 motion prediction */

                        ff_h263_pred_motion(s, 0, 0, &pred_x, &pred_y);
                        if (!s->mcsel) {
                            mx = ff_h263_decode_motion(s, pred_x, s->f_code);
                            if (mx >= 0xffff)
                                return -1;

                            my = ff_h263_decode_motion(s, pred_y, s->f_code);
                            if (my >= 0xffff)
                                return -1;
                            s->current_picture.mb_type[xy] = MB_TYPE_16x16 |
                                                             MB_TYPE_L0;
                        } else {
                            mx = get_amv(ctx, 0);
                            my = get_amv(ctx, 1);
                            s->current_picture.mb_type[xy] = MB_TYPE_16x16 |
                                                             MB_TYPE_GMC   |
                                                             MB_TYPE_L0;
                        }

                        mot_val[0]          =
                        mot_val[2]          =
                        mot_val[0 + stride] =
                        mot_val[2 + stride] = mx;
                        mot_val[1]          =
                        mot_val[3]          =
                        mot_val[1 + stride] =
                        mot_val[3 + stride] = my;
                    } else {
                        int i;
                        s->current_picture.mb_type[xy] = MB_TYPE_8x8 |
                                                         MB_TYPE_L0;
                        for (i = 0; i < 4; i++) {
                            int16_t *mot_val = ff_h263_pred_motion(s, i, 0, &pred_x, &pred_y);
                            mx = ff_h263_decode_motion(s, pred_x, s->f_code);
                            if (mx >= 0xffff)
                                return -1;

                            my = ff_h263_decode_motion(s, pred_y, s->f_code);
                            if (my >= 0xffff)
                                return -1;
                            mot_val[0] = mx;
                            mot_val[1] = my;
                        }
                    }
                }
            }
        }
        s->mb_x = 0;
    }

    return mb_num;
}

/**
 * decode second partition.
 * @return <0 if an error occurred
 */
static int mpeg4_decode_partition_b(MpegEncContext *s, int mb_count)
{
    int mb_num = 0;
    static const int8_t quant_tab[4] = { -1, -2, 1, 2 };

    s->mb_x = s->resync_mb_x;
    s->first_slice_line = 1;
    for (s->mb_y = s->resync_mb_y; mb_num < mb_count; s->mb_y++) {
        ff_init_block_index(s);
        for (; mb_num < mb_count && s->mb_x < s->mb_width; s->mb_x++) {
            const int xy = s->mb_x + s->mb_y * s->mb_stride;

            mb_num++;
            ff_update_block_index(s);
            if (s->mb_x == s->resync_mb_x && s->mb_y == s->resync_mb_y + 1)
                s->first_slice_line = 0;

            if (s->pict_type == AV_PICTURE_TYPE_I) {
                int ac_pred = bitstream_read_bit(&s->bc);
                int cbpy    = bitstream_read_vlc(&s->bc, ff_h263_cbpy_vlc.table,
                                                 CBPY_VLC_BITS, 1);
                if (cbpy < 0) {
                    av_log(s->avctx, AV_LOG_ERROR,
                           "cbpy corrupted at %d %d\n", s->mb_x, s->mb_y);
                    return -1;
                }

                s->cbp_table[xy]               |= cbpy << 2;
                s->current_picture.mb_type[xy] |= ac_pred * MB_TYPE_ACPRED;
            } else { /* P || S_TYPE */
                if (IS_INTRA(s->current_picture.mb_type[xy])) {
                    int i;
                    int dir     = 0;
                    int ac_pred = bitstream_read_bit(&s->bc);
                    int cbpy    = bitstream_read_vlc(&s->bc, ff_h263_cbpy_vlc.table,
                                                     CBPY_VLC_BITS, 1);

                    if (cbpy < 0) {
                        av_log(s->avctx, AV_LOG_ERROR,
                               "I cbpy corrupted at %d %d\n", s->mb_x, s->mb_y);
                        return -1;
                    }

                    if (s->cbp_table[xy] & 8)
                        ff_set_qscale(s, s->qscale + quant_tab[bitstream_read(&s->bc, 2)]);
                    s->current_picture.qscale_table[xy] = s->qscale;

                    for (i = 0; i < 6; i++) {
                        int dc_pred_dir;
                        int dc = mpeg4_decode_dc(s, i, &dc_pred_dir);
                        if (dc < 0) {
                            av_log(s->avctx, AV_LOG_ERROR,
                                   "DC corrupted at %d %d\n", s->mb_x, s->mb_y);
                            return -1;
                        }
                        dir <<= 1;
                        if (dc_pred_dir)
                            dir |= 1;
                    }
                    s->cbp_table[xy]               &= 3;  // remove dquant
                    s->cbp_table[xy]               |= cbpy << 2;
                    s->current_picture.mb_type[xy] |= ac_pred * MB_TYPE_ACPRED;
                    s->pred_dir_table[xy]           = dir;
                } else if (IS_SKIP(s->current_picture.mb_type[xy])) {
                    s->current_picture.qscale_table[xy] = s->qscale;
                    s->cbp_table[xy]                    = 0;
                } else {
                    int cbpy = bitstream_read_vlc(&s->bc, ff_h263_cbpy_vlc.table,
                                                  CBPY_VLC_BITS, 1);

                    if (cbpy < 0) {
                        av_log(s->avctx, AV_LOG_ERROR,
                               "P cbpy corrupted at %d %d\n", s->mb_x, s->mb_y);
                        return -1;
                    }

                    if (s->cbp_table[xy] & 8)
                        ff_set_qscale(s, s->qscale + quant_tab[bitstream_read(&s->bc, 2)]);
                    s->current_picture.qscale_table[xy] = s->qscale;

                    s->cbp_table[xy] &= 3;  // remove dquant
                    s->cbp_table[xy] |= (cbpy ^ 0xf) << 2;
                }
            }
        }
        if (mb_num >= mb_count)
            return 0;
        s->mb_x = 0;
    }
    return 0;
}

/**
 * Decode the first and second partition.
 * @return <0 if error (and sets error type in the error_status_table)
 */
int ff_mpeg4_decode_partitions(Mpeg4DecContext *ctx)
{
    MpegEncContext *s = &ctx->m;
    int mb_num;
    const int part_a_error = s->pict_type == AV_PICTURE_TYPE_I ? (ER_DC_ERROR | ER_MV_ERROR) : ER_MV_ERROR;
    const int part_a_end   = s->pict_type == AV_PICTURE_TYPE_I ? (ER_DC_END   | ER_MV_END)   : ER_MV_END;

    mb_num = mpeg4_decode_partition_a(ctx);
    if (mb_num < 0) {
        ff_er_add_slice(&s->er, s->resync_mb_x, s->resync_mb_y,
                        s->mb_x, s->mb_y, part_a_error);
        return -1;
    }

    if (s->resync_mb_x + s->resync_mb_y * s->mb_width + mb_num > s->mb_num) {
        av_log(s->avctx, AV_LOG_ERROR, "slice below monitor ...\n");
        ff_er_add_slice(&s->er, s->resync_mb_x, s->resync_mb_y,
                        s->mb_x, s->mb_y, part_a_error);
        return -1;
    }

    s->mb_num_left = mb_num;

    if (s->pict_type == AV_PICTURE_TYPE_I) {
        while (bitstream_peek(&s->bc, 9) == 1)
            bitstream_skip(&s->bc, 9);
        if (bitstream_read(&s->bc, 19) != DC_MARKER) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "marker missing after first I partition at %d %d\n",
                   s->mb_x, s->mb_y);
            return -1;
        }
    } else {
        while (bitstream_peek(&s->bc, 10) == 1)
            bitstream_skip(&s->bc, 10);
        if (bitstream_read(&s->bc, 17) != MOTION_MARKER) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "marker missing after first P partition at %d %d\n",
                   s->mb_x, s->mb_y);
            return -1;
        }
    }
    ff_er_add_slice(&s->er, s->resync_mb_x, s->resync_mb_y,
                    s->mb_x - 1, s->mb_y, part_a_end);

    if (mpeg4_decode_partition_b(s, mb_num) < 0) {
        if (s->pict_type == AV_PICTURE_TYPE_P)
            ff_er_add_slice(&s->er, s->resync_mb_x, s->resync_mb_y,
                            s->mb_x, s->mb_y, ER_DC_ERROR);
        return -1;
    } else {
        if (s->pict_type == AV_PICTURE_TYPE_P)
            ff_er_add_slice(&s->er, s->resync_mb_x, s->resync_mb_y,
                            s->mb_x - 1, s->mb_y, ER_DC_END);
    }

    return 0;
}

/**
 * Decode a block.
 * @return <0 if an error occurred
 */
static inline int mpeg4_decode_block(Mpeg4DecContext *ctx, int16_t *block,
                                     int n, int coded, int intra, int rvlc)
{
    MpegEncContext *s = &ctx->m;
    int level, i, last, run, qmul, qadd, dc_pred_dir;
    RLTable *rl;
    RL_VLC_ELEM *rl_vlc;
    const uint8_t *scan_table;

    // Note intra & rvlc should be optimized away if this is inlined

    if (intra) {
        if (ctx->use_intra_dc_vlc) {
            /* DC coef */
            if (s->partitioned_frame) {
                level = s->dc_val[0][s->block_index[n]];
                if (n < 4)
                    level = FASTDIV((level + (s->y_dc_scale >> 1)), s->y_dc_scale);
                else
                    level = FASTDIV((level + (s->c_dc_scale >> 1)), s->c_dc_scale);
                dc_pred_dir = (s->pred_dir_table[s->mb_x + s->mb_y * s->mb_stride] << n) & 32;
            } else {
                level = mpeg4_decode_dc(s, n, &dc_pred_dir);
                if (level < 0)
                    return -1;
            }
            block[0] = level;
            i        = 0;
        } else {
            i = -1;
            ff_mpeg4_pred_dc(s, n, 0, &dc_pred_dir, 0);
        }
        if (!coded)
            goto not_coded;

        if (rvlc) {
            rl     = &ff_rvlc_rl_intra;
            rl_vlc = ff_rvlc_rl_intra.rl_vlc[0];
        } else {
            rl     = &ff_mpeg4_rl_intra;
            rl_vlc = ff_mpeg4_rl_intra.rl_vlc[0];
        }
        if (s->ac_pred) {
            if (dc_pred_dir == 0)
                scan_table = s->intra_v_scantable.permutated;  /* left */
            else
                scan_table = s->intra_h_scantable.permutated;  /* top */
        } else {
            scan_table = s->intra_scantable.permutated;
        }
        qmul = 1;
        qadd = 0;
    } else {
        i = -1;
        if (!coded) {
            s->block_last_index[n] = i;
            return 0;
        }
        if (rvlc)
            rl = &ff_rvlc_rl_inter;
        else
            rl = &ff_h263_rl_inter;

        scan_table = s->intra_scantable.permutated;

        if (s->mpeg_quant) {
            qmul = 1;
            qadd = 0;
            if (rvlc)
                rl_vlc = ff_rvlc_rl_inter.rl_vlc[0];
            else
                rl_vlc = ff_h263_rl_inter.rl_vlc[0];
        } else {
            qmul = s->qscale << 1;
            qadd = (s->qscale - 1) | 1;
            if (rvlc)
                rl_vlc = ff_rvlc_rl_inter.rl_vlc[s->qscale];
            else
                rl_vlc = ff_h263_rl_inter.rl_vlc[s->qscale];
        }
    }
    {
        for (;;) {
            BITSTREAM_RL_VLC(level, run, &s->bc, rl_vlc, TEX_VLC_BITS, 2);
            if (level == 0) {
                /* escape */
                if (rvlc) {
                    if (bitstream_read_bit(&s->bc) == 0) {
                        av_log(s->avctx, AV_LOG_ERROR,
                               "1. marker bit missing in rvlc esc\n");
                        return -1;
                    }

                    last = bitstream_read_bit(&s->bc);
                    run = bitstream_read(&s->bc, 6);

                    if (bitstream_read_bit(&s->bc) == 0) {
                        av_log(s->avctx, AV_LOG_ERROR,
                               "2. marker bit missing in rvlc esc\n");
                        return -1;
                    }

                    level = bitstream_read(&s->bc, 11);

                    if (bitstream_read(&s->bc, 5) != 0x10) {
                        av_log(s->avctx, AV_LOG_ERROR, "reverse esc missing\n");
                        return -1;
                    }

                    level = level * qmul + qadd;
                    level = (level ^ bitstream_peek_signed(&s->bc, 1)) -
                             bitstream_peek_signed(&s->bc, 1);
                    bitstream_skip(&s->bc, 1);

                    i += run + 1;
                    if (last)
                        i += 192;
                } else {
                    int cache;

                    cache = bitstream_peek(&s->bc, 32);

                    if (IS_3IV1)
                        cache ^= 0xC0000000;

                    if (cache & 0x80000000) {
                        if (cache & 0x40000000) {
                            /* third escape */
                            bitstream_skip(&s->bc, 2);
                            last = bitstream_read_bit(&s->bc);
                            run = bitstream_read(&s->bc, 6);

                            if (IS_3IV1) {
                                level = bitstream_read_signed(&s->bc, 12);
                            } else {
                                if (bitstream_read(&s->bc, 1) == 0) {
                                    av_log(s->avctx, AV_LOG_ERROR,
                                           "1. marker bit missing in 3. esc\n");
                                    return -1;
                                }

                                level = bitstream_read_signed(&s->bc, 12);

                                if (bitstream_read(&s->bc, 1) == 0) {
                                    av_log(s->avctx, AV_LOG_ERROR,
                                           "2. marker bit missing in 3. esc\n");
                                    return -1;
                                }
                            }

                            if (level > 0)
                                level = level * qmul + qadd;
                            else
                                level = level * qmul - qadd;

                            if ((unsigned)(level + 2048) > 4095) {
                                if (s->avctx->err_recognition & AV_EF_BITSTREAM) {
                                    if (level > 2560 || level < -2560) {
                                        av_log(s->avctx, AV_LOG_ERROR,
                                               "|level| overflow in 3. esc, qp=%d\n",
                                               s->qscale);
                                        return -1;
                                    }
                                }
                                level = level < 0 ? -2048 : 2047;
                            }

                            i += run + 1;
                            if (last)
                                i += 192;
                        } else {
                            /* second escape */
                            bitstream_skip(&s->bc, 2);
                            BITSTREAM_RL_VLC(level, run, &s->bc, rl_vlc, TEX_VLC_BITS, 2);
                            i    += run + rl->max_run[run >> 7][level / qmul] + 1;  // FIXME opt indexing
                            level = (level ^ bitstream_peek_signed(&s->bc, 1)) -
                                     bitstream_peek_signed(&s->bc, 1);
                            bitstream_skip(&s->bc, 1);
                        }
                    } else {
                        /* first escape */
                        bitstream_skip(&s->bc, 1);
                        BITSTREAM_RL_VLC(level, run, &s->bc, rl_vlc, TEX_VLC_BITS, 2);
                        i    += run;
                        level = level + rl->max_level[run >> 7][(run - 1) & 63] * qmul;  // FIXME opt indexing
                        level = (level ^ bitstream_peek_signed(&s->bc, 1)) -
                                 bitstream_peek_signed(&s->bc, 1);
                        bitstream_skip(&s->bc, 1);
                    }
                }
            } else {
                i    += run;
                level = (level ^ bitstream_peek_signed(&s->bc, 1)) -
                         bitstream_peek_signed(&s->bc, 1);
                bitstream_skip(&s->bc, 1);
            }
            if (i > 62) {
                i -= 192;
                if (i & (~63)) {
                    av_log(s->avctx, AV_LOG_ERROR,
                           "ac-tex damaged at %d %d\n", s->mb_x, s->mb_y);
                    return -1;
                }

                block[scan_table[i]] = level;
                break;
            }

            block[scan_table[i]] = level;
        }
    }

not_coded:
    if (intra) {
        if (!ctx->use_intra_dc_vlc) {
            block[0] = ff_mpeg4_pred_dc(s, n, block[0], &dc_pred_dir, 0);

            i -= i >> 31;  // if (i == -1) i = 0;
        }

        ff_mpeg4_pred_ac(s, block, n, dc_pred_dir);
        if (s->ac_pred)
            i = 63;  // FIXME not optimal
    }
    s->block_last_index[n] = i;
    return 0;
}

/**
 * decode partition C of one MB.
 * @return <0 if an error occurred
 */
static int mpeg4_decode_partitioned_mb(MpegEncContext *s, int16_t block[6][64])
{
    Mpeg4DecContext *ctx = (Mpeg4DecContext *)s;
    int cbp, mb_type;
    const int xy = s->mb_x + s->mb_y * s->mb_stride;

    mb_type = s->current_picture.mb_type[xy];
    cbp     = s->cbp_table[xy];

    ctx->use_intra_dc_vlc = s->qscale < ctx->intra_dc_threshold;

    if (s->current_picture.qscale_table[xy] != s->qscale)
        ff_set_qscale(s, s->current_picture.qscale_table[xy]);

    if (s->pict_type == AV_PICTURE_TYPE_P ||
        s->pict_type == AV_PICTURE_TYPE_S) {
        int i;
        for (i = 0; i < 4; i++) {
            s->mv[0][i][0] = s->current_picture.motion_val[0][s->block_index[i]][0];
            s->mv[0][i][1] = s->current_picture.motion_val[0][s->block_index[i]][1];
        }
        s->mb_intra = IS_INTRA(mb_type);

        if (IS_SKIP(mb_type)) {
            /* skip mb */
            for (i = 0; i < 6; i++)
                s->block_last_index[i] = -1;
            s->mv_dir  = MV_DIR_FORWARD;
            s->mv_type = MV_TYPE_16X16;
            if (s->pict_type == AV_PICTURE_TYPE_S
                && ctx->vol_sprite_usage == GMC_SPRITE) {
                s->mcsel      = 1;
                s->mb_skipped = 0;
            } else {
                s->mcsel      = 0;
                s->mb_skipped = 1;
            }
        } else if (s->mb_intra) {
            s->ac_pred = IS_ACPRED(s->current_picture.mb_type[xy]);
        } else if (!s->mb_intra) {
            // s->mcsel = 0;  // FIXME do we need to init that?

            s->mv_dir = MV_DIR_FORWARD;
            if (IS_8X8(mb_type)) {
                s->mv_type = MV_TYPE_8X8;
            } else {
                s->mv_type = MV_TYPE_16X16;
            }
        }
    } else { /* I-Frame */
        s->mb_intra = 1;
        s->ac_pred  = IS_ACPRED(s->current_picture.mb_type[xy]);
    }

    if (!IS_SKIP(mb_type)) {
        int i;
        s->bdsp.clear_blocks(s->block[0]);
        /* decode each block */
        for (i = 0; i < 6; i++) {
            if (mpeg4_decode_block(ctx, block[i], i, cbp & 32, s->mb_intra, ctx->rvlc) < 0) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "texture corrupted at %d %d %d\n",
                       s->mb_x, s->mb_y, s->mb_intra);
                return -1;
            }
            cbp += cbp;
        }
    }

    /* per-MB end of slice check */
    if (--s->mb_num_left <= 0) {
        if (mpeg4_is_resync(s))
            return SLICE_END;
        else
            return SLICE_NOEND;
    } else {
        if (mpeg4_is_resync(s)) {
            const int delta = s->mb_x + 1 == s->mb_width ? 2 : 1;
            if (s->cbp_table[xy + delta])
                return SLICE_END;
        }
        return SLICE_OK;
    }
}

static int mpeg4_decode_mb(MpegEncContext *s, int16_t block[6][64])
{
    Mpeg4DecContext *ctx = (Mpeg4DecContext *)s;
    int cbpc, cbpy, i, cbp, pred_x, pred_y, mx, my, dquant;
    int16_t *mot_val;
    static int8_t quant_tab[4] = { -1, -2, 1, 2 };
    const int xy = s->mb_x + s->mb_y * s->mb_stride;

    assert(s->h263_pred);

    if (s->pict_type == AV_PICTURE_TYPE_P ||
        s->pict_type == AV_PICTURE_TYPE_S) {
        do {
            if (bitstream_read_bit(&s->bc)) {
                /* skip mb */
                s->mb_intra = 0;
                for (i = 0; i < 6; i++)
                    s->block_last_index[i] = -1;
                s->mv_dir  = MV_DIR_FORWARD;
                s->mv_type = MV_TYPE_16X16;
                if (s->pict_type == AV_PICTURE_TYPE_S &&
                    ctx->vol_sprite_usage == GMC_SPRITE) {
                    s->current_picture.mb_type[xy] = MB_TYPE_SKIP  |
                                                     MB_TYPE_GMC   |
                                                     MB_TYPE_16x16 |
                                                     MB_TYPE_L0;
                    s->mcsel       = 1;
                    s->mv[0][0][0] = get_amv(ctx, 0);
                    s->mv[0][0][1] = get_amv(ctx, 1);
                    s->mb_skipped  = 0;
                } else {
                    s->current_picture.mb_type[xy] = MB_TYPE_SKIP  |
                                                     MB_TYPE_16x16 |
                                                     MB_TYPE_L0;
                    s->mcsel       = 0;
                    s->mv[0][0][0] = 0;
                    s->mv[0][0][1] = 0;
                    s->mb_skipped  = 1;
                }
                goto end;
            }
            cbpc = bitstream_read_vlc(&s->bc, ff_h263_inter_MCBPC_vlc.table,
                                      INTER_MCBPC_VLC_BITS, 2);
            if (cbpc < 0) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "cbpc damaged at %d %d\n", s->mb_x, s->mb_y);
                return -1;
            }
        } while (cbpc == 20);

        s->bdsp.clear_blocks(s->block[0]);
        dquant      = cbpc & 8;
        s->mb_intra = ((cbpc & 4) != 0);
        if (s->mb_intra)
            goto intra;

        if (s->pict_type == AV_PICTURE_TYPE_S &&
            ctx->vol_sprite_usage == GMC_SPRITE && (cbpc & 16) == 0)
            s->mcsel = bitstream_read_bit(&s->bc);
        else
            s->mcsel = 0;
        cbpy = bitstream_read_vlc(&s->bc, ff_h263_cbpy_vlc.table,
                                  CBPY_VLC_BITS, 1) ^ 0x0F;

        cbp = (cbpc & 3) | (cbpy << 2);
        if (dquant)
            ff_set_qscale(s, s->qscale + quant_tab[bitstream_read(&s->bc, 2)]);
        if ((!s->progressive_sequence) &&
            (cbp || (s->workaround_bugs & FF_BUG_XVID_ILACE)))
            s->interlaced_dct = bitstream_read_bit(&s->bc);

        s->mv_dir = MV_DIR_FORWARD;
        if ((cbpc & 16) == 0) {
            if (s->mcsel) {
                s->current_picture.mb_type[xy] = MB_TYPE_GMC   |
                                                 MB_TYPE_16x16 |
                                                 MB_TYPE_L0;
                /* 16x16 global motion prediction */
                s->mv_type     = MV_TYPE_16X16;
                mx             = get_amv(ctx, 0);
                my             = get_amv(ctx, 1);
                s->mv[0][0][0] = mx;
                s->mv[0][0][1] = my;
            } else if ((!s->progressive_sequence) && bitstream_read_bit(&s->bc)) {
                s->current_picture.mb_type[xy] = MB_TYPE_16x8 |
                                                 MB_TYPE_L0   |
                                                 MB_TYPE_INTERLACED;
                /* 16x8 field motion prediction */
                s->mv_type = MV_TYPE_FIELD;

                s->field_select[0][0] = bitstream_read_bit(&s->bc);
                s->field_select[0][1] = bitstream_read_bit(&s->bc);

                ff_h263_pred_motion(s, 0, 0, &pred_x, &pred_y);

                for (i = 0; i < 2; i++) {
                    mx = ff_h263_decode_motion(s, pred_x, s->f_code);
                    if (mx >= 0xffff)
                        return -1;

                    my = ff_h263_decode_motion(s, pred_y / 2, s->f_code);
                    if (my >= 0xffff)
                        return -1;

                    s->mv[0][i][0] = mx;
                    s->mv[0][i][1] = my;
                }
            } else {
                s->current_picture.mb_type[xy] = MB_TYPE_16x16 | MB_TYPE_L0;
                /* 16x16 motion prediction */
                s->mv_type = MV_TYPE_16X16;
                ff_h263_pred_motion(s, 0, 0, &pred_x, &pred_y);
                mx = ff_h263_decode_motion(s, pred_x, s->f_code);

                if (mx >= 0xffff)
                    return -1;

                my = ff_h263_decode_motion(s, pred_y, s->f_code);

                if (my >= 0xffff)
                    return -1;
                s->mv[0][0][0] = mx;
                s->mv[0][0][1] = my;
            }
        } else {
            s->current_picture.mb_type[xy] = MB_TYPE_8x8 | MB_TYPE_L0;
            s->mv_type                     = MV_TYPE_8X8;
            for (i = 0; i < 4; i++) {
                mot_val = ff_h263_pred_motion(s, i, 0, &pred_x, &pred_y);
                mx      = ff_h263_decode_motion(s, pred_x, s->f_code);
                if (mx >= 0xffff)
                    return -1;

                my = ff_h263_decode_motion(s, pred_y, s->f_code);
                if (my >= 0xffff)
                    return -1;
                s->mv[0][i][0] = mx;
                s->mv[0][i][1] = my;
                mot_val[0]     = mx;
                mot_val[1]     = my;
            }
        }
    } else if (s->pict_type == AV_PICTURE_TYPE_B) {
        int modb1;   // first bit of modb
        int modb2;   // second bit of modb
        int mb_type;

        s->mb_intra = 0;  // B-frames never contain intra blocks
        s->mcsel    = 0;  //      ...               true gmc blocks

        if (s->mb_x == 0) {
            for (i = 0; i < 2; i++) {
                s->last_mv[i][0][0] =
                s->last_mv[i][0][1] =
                s->last_mv[i][1][0] =
                s->last_mv[i][1][1] = 0;
            }

            ff_thread_await_progress(&s->next_picture_ptr->tf, s->mb_y, 0);
        }

        /* if we skipped it in the future P-frame than skip it now too */
        s->mb_skipped = s->next_picture.mbskip_table[s->mb_y * s->mb_stride + s->mb_x];  // Note, skiptab=0 if last was GMC

        if (s->mb_skipped) {
            /* skip mb */
            for (i = 0; i < 6; i++)
                s->block_last_index[i] = -1;

            s->mv_dir      = MV_DIR_FORWARD;
            s->mv_type     = MV_TYPE_16X16;
            s->mv[0][0][0] =
            s->mv[0][0][1] =
            s->mv[1][0][0] =
            s->mv[1][0][1] = 0;
            s->current_picture.mb_type[xy] = MB_TYPE_SKIP  |
                                             MB_TYPE_16x16 |
                                             MB_TYPE_L0;
            goto end;
        }

        modb1 = bitstream_read_bit(&s->bc);
        if (modb1) {
            // like MB_TYPE_B_DIRECT but no vectors coded
            mb_type = MB_TYPE_DIRECT2 | MB_TYPE_SKIP | MB_TYPE_L0L1;
            cbp     = 0;
        } else {
            modb2   = bitstream_read_bit(&s->bc);
            mb_type = bitstream_read_vlc(&s->bc, mb_type_b_vlc.table,
                                         MB_TYPE_B_VLC_BITS, 1);
            if (mb_type < 0) {
                av_log(s->avctx, AV_LOG_ERROR, "illegal MB_type\n");
                return -1;
            }
            mb_type = mb_type_b_map[mb_type];
            if (modb2) {
                cbp = 0;
            } else {
                s->bdsp.clear_blocks(s->block[0]);
                cbp = bitstream_read(&s->bc, 6);
            }

            if ((!IS_DIRECT(mb_type)) && cbp) {
                if (bitstream_read_bit(&s->bc))
                    ff_set_qscale(s, s->qscale + bitstream_read_bit(&s->bc) * 4 - 2);
            }

            if (!s->progressive_sequence) {
                if (cbp)
                    s->interlaced_dct = bitstream_read_bit(&s->bc);

                if (!IS_DIRECT(mb_type) && bitstream_read_bit(&s->bc)) {
                    mb_type |= MB_TYPE_16x8 | MB_TYPE_INTERLACED;
                    mb_type &= ~MB_TYPE_16x16;

                    if (USES_LIST(mb_type, 0)) {
                        s->field_select[0][0] = bitstream_read_bit(&s->bc);
                        s->field_select[0][1] = bitstream_read_bit(&s->bc);
                    }
                    if (USES_LIST(mb_type, 1)) {
                        s->field_select[1][0] = bitstream_read_bit(&s->bc);
                        s->field_select[1][1] = bitstream_read_bit(&s->bc);
                    }
                }
            }

            s->mv_dir = 0;
            if ((mb_type & (MB_TYPE_DIRECT2 | MB_TYPE_INTERLACED)) == 0) {
                s->mv_type = MV_TYPE_16X16;

                if (USES_LIST(mb_type, 0)) {
                    s->mv_dir = MV_DIR_FORWARD;

                    mx = ff_h263_decode_motion(s, s->last_mv[0][0][0], s->f_code);
                    my = ff_h263_decode_motion(s, s->last_mv[0][0][1], s->f_code);
                    s->last_mv[0][1][0] =
                    s->last_mv[0][0][0] =
                    s->mv[0][0][0]      = mx;
                    s->last_mv[0][1][1] =
                    s->last_mv[0][0][1] =
                    s->mv[0][0][1]      = my;
                }

                if (USES_LIST(mb_type, 1)) {
                    s->mv_dir |= MV_DIR_BACKWARD;

                    mx = ff_h263_decode_motion(s, s->last_mv[1][0][0], s->b_code);
                    my = ff_h263_decode_motion(s, s->last_mv[1][0][1], s->b_code);
                    s->last_mv[1][1][0] =
                    s->last_mv[1][0][0] =
                    s->mv[1][0][0]      = mx;
                    s->last_mv[1][1][1] =
                    s->last_mv[1][0][1] =
                    s->mv[1][0][1]      = my;
                }
            } else if (!IS_DIRECT(mb_type)) {
                s->mv_type = MV_TYPE_FIELD;

                if (USES_LIST(mb_type, 0)) {
                    s->mv_dir = MV_DIR_FORWARD;

                    for (i = 0; i < 2; i++) {
                        mx = ff_h263_decode_motion(s, s->last_mv[0][i][0], s->f_code);
                        my = ff_h263_decode_motion(s, s->last_mv[0][i][1] / 2, s->f_code);
                        s->last_mv[0][i][0] =
                        s->mv[0][i][0]      = mx;
                        s->last_mv[0][i][1] = (s->mv[0][i][1] = my) * 2;
                    }
                }

                if (USES_LIST(mb_type, 1)) {
                    s->mv_dir |= MV_DIR_BACKWARD;

                    for (i = 0; i < 2; i++) {
                        mx = ff_h263_decode_motion(s, s->last_mv[1][i][0], s->b_code);
                        my = ff_h263_decode_motion(s, s->last_mv[1][i][1] / 2, s->b_code);
                        s->last_mv[1][i][0] =
                        s->mv[1][i][0]      = mx;
                        s->last_mv[1][i][1] = (s->mv[1][i][1] = my) * 2;
                    }
                }
            }
        }

        if (IS_DIRECT(mb_type)) {
            if (IS_SKIP(mb_type)) {
                mx =
                my = 0;
            } else {
                mx = ff_h263_decode_motion(s, 0, 1);
                my = ff_h263_decode_motion(s, 0, 1);
            }

            s->mv_dir = MV_DIR_FORWARD | MV_DIR_BACKWARD | MV_DIRECT;
            mb_type  |= ff_mpeg4_set_direct_mv(s, mx, my);
        }
        s->current_picture.mb_type[xy] = mb_type;
    } else { /* I-Frame */
        do {
            cbpc = bitstream_read_vlc(&s->bc, ff_h263_intra_MCBPC_vlc.table,
                                      INTRA_MCBPC_VLC_BITS, 2);
            if (cbpc < 0) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "I cbpc damaged at %d %d\n", s->mb_x, s->mb_y);
                return -1;
            }
        } while (cbpc == 8);

        dquant = cbpc & 4;
        s->mb_intra = 1;

intra:
        s->ac_pred = bitstream_read_bit(&s->bc);
        if (s->ac_pred)
            s->current_picture.mb_type[xy] = MB_TYPE_INTRA | MB_TYPE_ACPRED;
        else
            s->current_picture.mb_type[xy] = MB_TYPE_INTRA;

        cbpy = bitstream_read_vlc(&s->bc, ff_h263_cbpy_vlc.table, CBPY_VLC_BITS, 1);
        if (cbpy < 0) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "I cbpy damaged at %d %d\n", s->mb_x, s->mb_y);
            return -1;
        }
        cbp = (cbpc & 3) | (cbpy << 2);

        ctx->use_intra_dc_vlc = s->qscale < ctx->intra_dc_threshold;

        if (dquant)
            ff_set_qscale(s, s->qscale + quant_tab[bitstream_read(&s->bc, 2)]);

        if (!s->progressive_sequence)
            s->interlaced_dct = bitstream_read_bit(&s->bc);

        s->bdsp.clear_blocks(s->block[0]);
        /* decode each block */
        for (i = 0; i < 6; i++) {
            if (mpeg4_decode_block(ctx, block[i], i, cbp & 32, 1, 0) < 0)
                return -1;
            cbp += cbp;
        }
        goto end;
    }

    /* decode each block */
    for (i = 0; i < 6; i++) {
        if (mpeg4_decode_block(ctx, block[i], i, cbp & 32, 0, 0) < 0)
            return -1;
        cbp += cbp;
    }

end:
    /* per-MB end of slice check */
    if (s->codec_id == AV_CODEC_ID_MPEG4) {
        if (mpeg4_is_resync(s)) {
            const int delta = s->mb_x + 1 == s->mb_width ? 2 : 1;

            if (s->pict_type == AV_PICTURE_TYPE_B &&
                s->next_picture.mbskip_table[xy + delta]) {
                ff_thread_await_progress(&s->next_picture_ptr->tf,
                                         (s->mb_x + delta >= s->mb_width)
                                         ? FFMIN(s->mb_y + 1, s->mb_height - 1)
                                         : s->mb_y, 0);
            }

            if (s->pict_type == AV_PICTURE_TYPE_B &&
                s->next_picture.mbskip_table[xy + delta])
                return SLICE_OK;
            return SLICE_END;
        }
    }

    return SLICE_OK;
}

static int mpeg4_decode_gop_header(MpegEncContext *s, BitstreamContext *bc)
{
    int hours, minutes, seconds;
    unsigned time_code = bitstream_peek(bc, 18);

    if (time_code & 0x40) {     /* marker_bit */
        hours   = time_code >> 13;
        minutes = time_code >> 7 & 0x3f;
        seconds = time_code & 0x3f;
        s->time_base = seconds + 60 * (minutes + 60 * hours);
        bitstream_skip(bc, 20);      /* time_code, closed_gov, broken_link */
    } else {
        av_log(s->avctx, AV_LOG_WARNING, "GOP header missing marker_bit\n");
    }

    return 0;
}

static int mpeg4_decode_profile_level(MpegEncContext *s, BitstreamContext *bc)
{
    int profile_and_level_indication;

    profile_and_level_indication = bitstream_read(bc, 8);

    s->avctx->profile = (profile_and_level_indication & 0xf0) >> 4;
    s->avctx->level   = (profile_and_level_indication & 0x0f);

    // for Simple profile, level 0
    if (s->avctx->profile == 0 && s->avctx->level == 8) {
        s->avctx->level = 0;
    }

    return 0;
}

static int decode_vol_header(Mpeg4DecContext *ctx, BitstreamContext *bc)
{
    MpegEncContext *s = &ctx->m;
    int width, height, vo_ver_id;

    /* vol header */
    bitstream_skip(bc, 1);                   /* random access */
    s->vo_type = bitstream_read(bc, 8);
    if (bitstream_read_bit(bc) != 0) {       /* is_ol_id */
        vo_ver_id = bitstream_read(bc, 4);   /* vo_ver_id */
        bitstream_skip(bc, 3);               /* vo_priority */
    } else {
        vo_ver_id = 1;
    }
    s->aspect_ratio_info = bitstream_read(bc, 4);
    if (s->aspect_ratio_info == FF_ASPECT_EXTENDED) {
        s->avctx->sample_aspect_ratio.num = bitstream_read(bc, 8);  // par_width
        s->avctx->sample_aspect_ratio.den = bitstream_read(bc, 8);  // par_height
    } else {
        s->avctx->sample_aspect_ratio = ff_h263_pixel_aspect[s->aspect_ratio_info];
    }

    if ((ctx->vol_control_parameters = bitstream_read_bit(bc))) { /* vol control parameter */
        int chroma_format = bitstream_read(bc, 2);
        if (chroma_format != CHROMA_420)
            av_log(s->avctx, AV_LOG_ERROR, "illegal chroma format\n");

        s->low_delay = bitstream_read_bit(bc);
        if (bitstream_read_bit(bc)) { /* vbv parameters */
            bitstream_read(bc, 15);   /* first_half_bitrate */
            bitstream_skip(bc, 1);    /* marker */
            bitstream_read(bc, 15);   /* latter_half_bitrate */
            bitstream_skip(bc, 1);    /* marker */
            bitstream_read(bc, 15);   /* first_half_vbv_buffer_size */
            bitstream_skip(bc, 1);    /* marker */
            bitstream_read(bc, 3);    /* latter_half_vbv_buffer_size */
            bitstream_read(bc, 11);   /* first_half_vbv_occupancy */
            bitstream_skip(bc, 1);    /* marker */
            bitstream_read(bc, 15);   /* latter_half_vbv_occupancy */
            bitstream_skip(bc, 1);    /* marker */
        }
    } else {
        /* is setting low delay flag only once the smartest thing to do?
         * low delay detection will not be overridden. */
        if (s->picture_number == 0)
            s->low_delay = 0;
    }

    ctx->shape = bitstream_read(bc, 2); /* vol shape */
    if (ctx->shape != RECT_SHAPE)
        av_log(s->avctx, AV_LOG_ERROR, "only rectangular vol supported\n");
    if (ctx->shape == GRAY_SHAPE && vo_ver_id != 1) {
        av_log(s->avctx, AV_LOG_ERROR, "Gray shape not supported\n");
        bitstream_skip(bc, 4);          /* video_object_layer_shape_extension */
    }

    check_marker(s->avctx, bc, "before time_increment_resolution");

    s->avctx->framerate.num = bitstream_read(bc, 16);
    if (!s->avctx->framerate.num) {
        av_log(s->avctx, AV_LOG_ERROR, "framerate==0\n");
        return -1;
    }

    ctx->time_increment_bits = av_log2(s->avctx->framerate.num - 1) + 1;
    if (ctx->time_increment_bits < 1)
        ctx->time_increment_bits = 1;

    check_marker(s->avctx, bc, "before fixed_vop_rate");

    if (bitstream_read_bit(bc) != 0)     /* fixed_vop_rate  */
        s->avctx->framerate.den = bitstream_read(bc, ctx->time_increment_bits);
    else
        s->avctx->framerate.den = 1;

    ctx->t_frame = 0;

    if (ctx->shape != BIN_ONLY_SHAPE) {
        if (ctx->shape == RECT_SHAPE) {
            bitstream_skip(bc, 1);  /* marker */
            width = bitstream_read(bc, 13);
            bitstream_skip(bc, 1);  /* marker */
            height = bitstream_read(bc, 13);
            bitstream_skip(bc, 1);  /* marker */
            if (width && height &&  /* they should be non zero but who knows */
                !(s->width && s->codec_tag == AV_RL32("MP4S"))) {
                if (s->width && s->height &&
                    (s->width != width || s->height != height))
                    s->context_reinit = 1;
                s->width  = width;
                s->height = height;
            }
        }

        s->progressive_sequence  =
        s->progressive_frame     = bitstream_read_bit(bc) ^ 1;
        s->interlaced_dct        = 0;
        if (!bitstream_read_bit(bc) && (s->avctx->debug & FF_DEBUG_PICT_INFO))
            av_log(s->avctx, AV_LOG_INFO,           /* OBMC Disable */
                   "MPEG-4 OBMC not supported (very likely buggy encoder)\n");
        if (vo_ver_id == 1)
            ctx->vol_sprite_usage = bitstream_read_bit(bc); /* vol_sprite_usage */
        else
            ctx->vol_sprite_usage = bitstream_read(bc, 2);  /* vol_sprite_usage */

        if (ctx->vol_sprite_usage == STATIC_SPRITE)
            av_log(s->avctx, AV_LOG_ERROR, "Static Sprites not supported\n");
        if (ctx->vol_sprite_usage == STATIC_SPRITE ||
            ctx->vol_sprite_usage == GMC_SPRITE) {
            if (ctx->vol_sprite_usage == STATIC_SPRITE) {
                bitstream_skip(bc, 13); // sprite_width
                bitstream_skip(bc, 1); /* marker */
                bitstream_skip(bc, 13); // sprite_height
                bitstream_skip(bc, 1); /* marker */
                bitstream_skip(bc, 13); // sprite_left
                bitstream_skip(bc, 1); /* marker */
                bitstream_skip(bc, 13); // sprite_top
                bitstream_skip(bc, 1); /* marker */
            }
            ctx->num_sprite_warping_points = bitstream_read(bc, 6);
            if (ctx->num_sprite_warping_points > 3) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "%d sprite_warping_points\n",
                       ctx->num_sprite_warping_points);
                ctx->num_sprite_warping_points = 0;
                return -1;
            }
            s->sprite_warping_accuracy  = bitstream_read(bc, 2);
            ctx->sprite_brightness_change = bitstream_read_bit(bc);
            if (ctx->vol_sprite_usage == STATIC_SPRITE)
                bitstream_skip(bc, 1); // low_latency_sprite
        }
        // FIXME sadct disable bit if verid!=1 && shape not rect

        if (bitstream_read_bit(bc) == 1) {                /* not_8_bit */
            s->quant_precision = bitstream_read(bc, 4);   /* quant_precision */
            if (bitstream_read(bc, 4) != 8)               /* bits_per_pixel */
                av_log(s->avctx, AV_LOG_ERROR, "N-bit not supported\n");
            if (s->quant_precision != 5)
                av_log(s->avctx, AV_LOG_ERROR,
                       "quant precision %d\n", s->quant_precision);
        } else {
            s->quant_precision = 5;
        }

        // FIXME a bunch of grayscale shape things

        if ((s->mpeg_quant = bitstream_read_bit(bc))) { /* vol_quant_type */
            int i, v;

            /* load default matrixes */
            for (i = 0; i < 64; i++) {
                int j = s->idsp.idct_permutation[i];
                v = ff_mpeg4_default_intra_matrix[i];
                s->intra_matrix[j]        = v;
                s->chroma_intra_matrix[j] = v;

                v = ff_mpeg4_default_non_intra_matrix[i];
                s->inter_matrix[j]        = v;
                s->chroma_inter_matrix[j] = v;
            }

            /* load custom intra matrix */
            if (bitstream_read_bit(bc)) {
                int last = 0;
                for (i = 0; i < 64; i++) {
                    int j;
                    v = bitstream_read(bc, 8);
                    if (v == 0)
                        break;

                    last = v;
                    j = s->idsp.idct_permutation[ff_zigzag_direct[i]];
                    s->intra_matrix[j]        = last;
                    s->chroma_intra_matrix[j] = last;
                }

                /* replicate last value */
                for (; i < 64; i++) {
                    int j = s->idsp.idct_permutation[ff_zigzag_direct[i]];
                    s->intra_matrix[j]        = last;
                    s->chroma_intra_matrix[j] = last;
                }
            }

            /* load custom non intra matrix */
            if (bitstream_read_bit(bc)) {
                int last = 0;
                for (i = 0; i < 64; i++) {
                    int j;
                    v = bitstream_read(bc, 8);
                    if (v == 0)
                        break;

                    last = v;
                    j = s->idsp.idct_permutation[ff_zigzag_direct[i]];
                    s->inter_matrix[j]        = v;
                    s->chroma_inter_matrix[j] = v;
                }

                /* replicate last value */
                for (; i < 64; i++) {
                    int j = s->idsp.idct_permutation[ff_zigzag_direct[i]];
                    s->inter_matrix[j]        = last;
                    s->chroma_inter_matrix[j] = last;
                }
            }

            // FIXME a bunch of grayscale shape things
        }

        if (vo_ver_id != 1)
            s->quarter_sample = bitstream_read_bit(bc);
        else
            s->quarter_sample = 0;

        if (!bitstream_read_bit(bc)) {
            int pos               = bitstream_tell(bc);
            int estimation_method = bitstream_read(bc, 2);
            if (estimation_method < 2) {
                if (!bitstream_read_bit(bc)) {
                    ctx->cplx_estimation_trash_i += 8 * bitstream_read_bit(bc);  /* opaque */
                    ctx->cplx_estimation_trash_i += 8 * bitstream_read_bit(bc);  /* transparent */
                    ctx->cplx_estimation_trash_i += 8 * bitstream_read_bit(bc);  /* intra_cae */
                    ctx->cplx_estimation_trash_i += 8 * bitstream_read_bit(bc);  /* inter_cae */
                    ctx->cplx_estimation_trash_i += 8 * bitstream_read_bit(bc);  /* no_update */
                    ctx->cplx_estimation_trash_i += 8 * bitstream_read_bit(bc);  /* upampling */
                }
                if (!bitstream_read_bit(bc)) {
                    ctx->cplx_estimation_trash_i += 8 * bitstream_read_bit(bc);  /* intra_blocks */
                    ctx->cplx_estimation_trash_p += 8 * bitstream_read_bit(bc);  /* inter_blocks */
                    ctx->cplx_estimation_trash_p += 8 * bitstream_read_bit(bc);  /* inter4v_blocks */
                    ctx->cplx_estimation_trash_i += 8 * bitstream_read_bit(bc);  /* not coded blocks */
                }
                if (!check_marker(s->avctx, bc, "in complexity estimation part 1")) {
                    bitstream_skip(bc, pos - bitstream_tell(bc));
                    goto no_cplx_est;
                }
                if (!bitstream_read_bit(bc)) {
                    ctx->cplx_estimation_trash_i += 8 * bitstream_read_bit(bc);  /* dct_coeffs */
                    ctx->cplx_estimation_trash_i += 8 * bitstream_read_bit(bc);  /* dct_lines */
                    ctx->cplx_estimation_trash_i += 8 * bitstream_read_bit(bc);  /* vlc_syms */
                    ctx->cplx_estimation_trash_i += 4 * bitstream_read_bit(bc);  /* vlc_bits */
                }
                if (!bitstream_read_bit(bc)) {
                    ctx->cplx_estimation_trash_p += 8 * bitstream_read_bit(bc);  /* apm */
                    ctx->cplx_estimation_trash_p += 8 * bitstream_read_bit(bc);  /* npm */
                    ctx->cplx_estimation_trash_b += 8 * bitstream_read_bit(bc);  /* interpolate_mc_q */
                    ctx->cplx_estimation_trash_p += 8 * bitstream_read_bit(bc);  /* forwback_mc_q */
                    ctx->cplx_estimation_trash_p += 8 * bitstream_read_bit(bc);  /* halfpel2 */
                    ctx->cplx_estimation_trash_p += 8 * bitstream_read_bit(bc);  /* halfpel4 */
                }
                if (!check_marker(s->avctx, bc, "in complexity estimation part 2")) {
                    bitstream_skip(bc, pos - bitstream_tell(bc));
                    goto no_cplx_est;
                }
                if (estimation_method == 1) {
                    ctx->cplx_estimation_trash_i += 8 * bitstream_read_bit(bc);  /* sadct */
                    ctx->cplx_estimation_trash_p += 8 * bitstream_read_bit(bc);  /* qpel */
                }
            } else
                av_log(s->avctx, AV_LOG_ERROR,
                       "Invalid Complexity estimation method %d\n",
                       estimation_method);
        } else {

no_cplx_est:
            ctx->cplx_estimation_trash_i =
            ctx->cplx_estimation_trash_p =
            ctx->cplx_estimation_trash_b = 0;
        }

        ctx->resync_marker = !bitstream_read_bit(bc); /* resync_marker_disabled */

        s->data_partitioning = bitstream_read_bit(bc);
        if (s->data_partitioning)
            ctx->rvlc = bitstream_read_bit(bc);

        if (vo_ver_id != 1) {
            ctx->new_pred = bitstream_read_bit(bc);
            if (ctx->new_pred) {
                av_log(s->avctx, AV_LOG_ERROR, "new pred not supported\n");
                bitstream_skip(bc, 2); /* requested upstream message type */
                bitstream_skip(bc, 1); /* newpred segment type */
            }
            if (bitstream_read_bit(bc)) // reduced_res_vop
                av_log(s->avctx, AV_LOG_ERROR,
                       "reduced resolution VOP not supported\n");
        } else {
            ctx->new_pred = 0;
        }

        ctx->scalability = bitstream_read_bit(bc);

        if (ctx->scalability) {
            BitstreamContext bak = *bc;
            int h_sampling_factor_n;
            int h_sampling_factor_m;
            int v_sampling_factor_n;
            int v_sampling_factor_m;

            bitstream_skip(bc, 1);  // hierarchy_type
            bitstream_skip(bc, 4);  /* ref_layer_id */
            bitstream_skip(bc, 1);  /* ref_layer_sampling_dir */
            h_sampling_factor_n = bitstream_read(bc, 5);
            h_sampling_factor_m = bitstream_read(bc, 5);
            v_sampling_factor_n = bitstream_read(bc, 5);
            v_sampling_factor_m = bitstream_read(bc, 5);
            ctx->enhancement_type = bitstream_read_bit(bc);

            if (h_sampling_factor_n == 0 || h_sampling_factor_m == 0 ||
                v_sampling_factor_n == 0 || v_sampling_factor_m == 0) {
                /* illegal scalability header (VERY broken encoder),
                 * trying to workaround */
                ctx->scalability = 0;
                *bc            = bak;
            } else
                av_log(s->avctx, AV_LOG_ERROR, "scalability not supported\n");

            // bin shape stuff FIXME
        }
    }

    return 0;
}

/**
 * Decode the user data stuff in the header.
 * Also initializes divx/xvid/lavc_version/build.
 */
static int decode_user_data(Mpeg4DecContext *ctx, BitstreamContext *bc)
{
    MpegEncContext *s = &ctx->m;
    char buf[256];
    int i;
    int e;
    int ver = 0, build = 0, ver2 = 0, ver3 = 0;
    char last;

    for (i = 0; i < 255 && bitstream_bits_left > 0; i++) {
        if (bitstream_peek(bc, 23) == 0)
            break;
        buf[i] = bitstream_read(bc, 8);
    }
    buf[i] = 0;

    /* divx detection */
    e = sscanf(buf, "DivX%dBuild%d%c", &ver, &build, &last);
    if (e < 2)
        e = sscanf(buf, "DivX%db%d%c", &ver, &build, &last);
    if (e >= 2) {
        ctx->divx_version = ver;
        ctx->divx_build   = build;
        s->divx_packed  = e == 3 && last == 'p';
        if (s->divx_packed && !ctx->showed_packed_warning) {
            av_log(s->avctx, AV_LOG_WARNING,
                   "Invalid and inefficient vfw-avi packed B-frames detected\n");
            ctx->showed_packed_warning = 1;
        }
    }

    /* libavcodec detection */
    e = sscanf(buf, "FFmpe%*[^b]b%d", &build) + 3;
    if (e != 4)
        e = sscanf(buf, "FFmpeg v%d.%d.%d / libavcodec build: %d", &ver, &ver2, &ver3, &build);
    if (e != 4) {
        e = sscanf(buf, "Lavc%d.%d.%d", &ver, &ver2, &ver3) + 1;
        if (e > 1)
            build = (ver << 16) + (ver2 << 8) + ver3;
    }
    if (e != 4) {
        if (strcmp(buf, "ffmpeg") == 0)
            ctx->lavc_build = 4600;
    }
    if (e == 4)
        ctx->lavc_build = build;

    /* Xvid detection */
    e = sscanf(buf, "XviD%d", &build);
    if (e == 1)
        ctx->xvid_build = build;

    if (ctx->xvid_build == -1 && ctx->divx_version == -1 && ctx->lavc_build == -1) {
        if (s->codec_tag        == AV_RL32("XVID") ||
            s->codec_tag        == AV_RL32("XVIX") ||
            s->codec_tag        == AV_RL32("RMP4") ||
            s->codec_tag        == AV_RL32("ZMP4") ||
            s->codec_tag        == AV_RL32("SIPP"))
            ctx->xvid_build = 0;
    }

    if (ctx->xvid_build == -1 && ctx->divx_version == -1 && ctx->lavc_build == -1)
        if (s->codec_tag == AV_RL32("DIVX") && s->vo_type == 0 &&
            ctx->vol_control_parameters == 0)
            ctx->divx_version = 400;  // divx 4

    if (ctx->xvid_build >= 0 && ctx->divx_version >= 0) {
        ctx->divx_version =
        ctx->divx_build   = -1;
    }

    if (CONFIG_MPEG4_DECODER && ctx->xvid_build >= 0)
        ff_xvid_idct_init(&s->idsp, s->avctx);

    return 0;
}

static int decode_vop_header(Mpeg4DecContext *ctx, BitstreamContext *bc)
{
    MpegEncContext *s = &ctx->m;
    int time_incr, time_increment;

    s->pict_type = bitstream_read(bc, 2) + AV_PICTURE_TYPE_I; /* pict type: I = 0 , P = 1 */
    if (s->pict_type == AV_PICTURE_TYPE_B && s->low_delay &&
        ctx->vol_control_parameters == 0 && !(s->avctx->flags & AV_CODEC_FLAG_LOW_DELAY)) {
        av_log(s->avctx, AV_LOG_ERROR, "low_delay flag set incorrectly, clearing it\n");
        s->low_delay = 0;
    }

    s->partitioned_frame = s->data_partitioning && s->pict_type != AV_PICTURE_TYPE_B;
    if (s->partitioned_frame)
        s->decode_mb = mpeg4_decode_partitioned_mb;
    else
        s->decode_mb = mpeg4_decode_mb;

    time_incr = 0;
    while (bitstream_read_bit(bc) != 0)
        time_incr++;

    check_marker(s->avctx, bc, "before time_increment");

    if (ctx->time_increment_bits == 0 ||
        !(bitstream_peek(bc, ctx->time_increment_bits + 1) & 1)) {
        /* Headers seem incomplete; try to guess time_increment_bits. */
        for (ctx->time_increment_bits = 1;
             ctx->time_increment_bits < 16;
             ctx->time_increment_bits++) {
            if (s->pict_type == AV_PICTURE_TYPE_P ||
                (s->pict_type == AV_PICTURE_TYPE_S &&
                 ctx->vol_sprite_usage == GMC_SPRITE)) {
                if ((bitstream_peek(bc, ctx->time_increment_bits + 6) & 0x37) == 0x30)
                    break;
            } else if ((bitstream_peek(bc, ctx->time_increment_bits + 5) & 0x1F) == 0x18)
                break;
        }
    }

    if (IS_3IV1)
        time_increment = bitstream_read_bit(bc);        // FIXME investigate further
    else
        time_increment = bitstream_read(bc, ctx->time_increment_bits);

    if (s->pict_type != AV_PICTURE_TYPE_B) {
        s->last_time_base = s->time_base;
        s->time_base     += time_incr;
        s->time = s->time_base * s->avctx->framerate.num + time_increment;
        if (s->workaround_bugs & FF_BUG_UMP4) {
            if (s->time < s->last_non_b_time) {
                /* header is not mpeg-4-compatible, broken encoder,
                 * trying to workaround */
                s->time_base++;
                s->time += s->avctx->framerate.num;
            }
        }
        s->pp_time         = s->time - s->last_non_b_time;
        s->last_non_b_time = s->time;
    } else {
        s->time    = (s->last_time_base + time_incr) * s->avctx->framerate.num + time_increment;
        s->pb_time = s->pp_time - (s->last_non_b_time - s->time);
        if (s->pp_time <= s->pb_time ||
            s->pp_time <= s->pp_time - s->pb_time ||
            s->pp_time <= 0) {
            /* messed up order, maybe after seeking? skipping current B-frame */
            return FRAME_SKIPPED;
        }
        ff_mpeg4_init_direct_mv(s);

        if (ctx->t_frame == 0)
            ctx->t_frame = s->pb_time;
        if (ctx->t_frame == 0)
            ctx->t_frame = 1;  // 1/0 protection
        s->pp_field_time = (ROUNDED_DIV(s->last_non_b_time, ctx->t_frame) -
                            ROUNDED_DIV(s->last_non_b_time - s->pp_time, ctx->t_frame)) * 2;
        s->pb_field_time = (ROUNDED_DIV(s->time, ctx->t_frame) -
                            ROUNDED_DIV(s->last_non_b_time - s->pp_time, ctx->t_frame)) * 2;
        if (!s->progressive_sequence) {
            if (s->pp_field_time <= s->pb_field_time || s->pb_field_time <= 1)
                return FRAME_SKIPPED;
        }
    }

    check_marker(s->avctx, bc, "before vop_coded");

    /* vop coded */
    if (bitstream_read_bit(bc) != 1) {
        if (s->avctx->debug & FF_DEBUG_PICT_INFO)
            av_log(s->avctx, AV_LOG_ERROR, "vop not coded\n");
        return FRAME_SKIPPED;
    }
    if (ctx->shape != BIN_ONLY_SHAPE &&
                    (s->pict_type == AV_PICTURE_TYPE_P ||
                     (s->pict_type == AV_PICTURE_TYPE_S &&
                      ctx->vol_sprite_usage == GMC_SPRITE))) {
        /* rounding type for motion estimation */
        s->no_rounding = bitstream_read_bit(bc);
    } else {
        s->no_rounding = 0;
    }
    // FIXME reduced res stuff

    if (ctx->shape != RECT_SHAPE) {
        if (ctx->vol_sprite_usage != 1 || s->pict_type != AV_PICTURE_TYPE_I) {
            bitstream_skip(bc, 13);  /* width */
            bitstream_skip(bc, 1);   /* marker */
            bitstream_skip(bc, 13);  /* height */
            bitstream_skip(bc, 1);   /* marker */
            bitstream_skip(bc, 13);  /* hor_spat_ref */
            bitstream_skip(bc, 1);   /* marker */
            bitstream_skip(bc, 13);  /* ver_spat_ref */
        }
        bitstream_skip(bc, 1);       /* change_CR_disable */

        if (bitstream_read_bit(bc) != 0)
            bitstream_skip(bc, 8);   /* constant_alpha_value */
    }

    // FIXME complexity estimation stuff

    if (ctx->shape != BIN_ONLY_SHAPE) {
        bitstream_skip(bc, ctx->cplx_estimation_trash_i);
        if (s->pict_type != AV_PICTURE_TYPE_I)
            bitstream_skip(bc, ctx->cplx_estimation_trash_p);
        if (s->pict_type == AV_PICTURE_TYPE_B)
            bitstream_skip(bc, ctx->cplx_estimation_trash_b);

        ctx->intra_dc_threshold = ff_mpeg4_dc_threshold[bitstream_read(bc, 3)];
        if (!s->progressive_sequence) {
            s->top_field_first = bitstream_read_bit(bc);
            s->alternate_scan  = bitstream_read_bit(bc);
        } else
            s->alternate_scan = 0;
    }

    if (s->alternate_scan) {
        ff_init_scantable(s->idsp.idct_permutation, &s->inter_scantable,   ff_alternate_vertical_scan);
        ff_init_scantable(s->idsp.idct_permutation, &s->intra_scantable,   ff_alternate_vertical_scan);
        ff_init_scantable(s->idsp.idct_permutation, &s->intra_h_scantable, ff_alternate_vertical_scan);
        ff_init_scantable(s->idsp.idct_permutation, &s->intra_v_scantable, ff_alternate_vertical_scan);
    } else {
        ff_init_scantable(s->idsp.idct_permutation, &s->inter_scantable,   ff_zigzag_direct);
        ff_init_scantable(s->idsp.idct_permutation, &s->intra_scantable,   ff_zigzag_direct);
        ff_init_scantable(s->idsp.idct_permutation, &s->intra_h_scantable, ff_alternate_horizontal_scan);
        ff_init_scantable(s->idsp.idct_permutation, &s->intra_v_scantable, ff_alternate_vertical_scan);
    }

    if (s->pict_type == AV_PICTURE_TYPE_S &&
        (ctx->vol_sprite_usage == STATIC_SPRITE ||
         ctx->vol_sprite_usage == GMC_SPRITE)) {
        if (mpeg4_decode_sprite_trajectory(ctx, bc) < 0)
            return AVERROR_INVALIDDATA;
        if (ctx->sprite_brightness_change)
            av_log(s->avctx, AV_LOG_ERROR,
                   "sprite_brightness_change not supported\n");
        if (ctx->vol_sprite_usage == STATIC_SPRITE)
            av_log(s->avctx, AV_LOG_ERROR, "static sprite not supported\n");
    }

    if (ctx->shape != BIN_ONLY_SHAPE) {
        s->chroma_qscale = s->qscale = bitstream_read(bc, s->quant_precision);
        if (s->qscale == 0) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "Error, header damaged or not MPEG-4 header (qscale=0)\n");
            return -1;  // makes no sense to continue, as there is nothing left from the image then
        }

        if (s->pict_type != AV_PICTURE_TYPE_I) {
            s->f_code = bitstream_read(bc, 3);        /* fcode_for */
            if (s->f_code == 0) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "Error, header damaged or not MPEG-4 header (f_code=0)\n");
                return -1;  // makes no sense to continue, as there is nothing left from the image then
            }
        } else
            s->f_code = 1;

        if (s->pict_type == AV_PICTURE_TYPE_B) {
            s->b_code = bitstream_read(bc, 3);
        } else
            s->b_code = 1;

        if (s->avctx->debug & FF_DEBUG_PICT_INFO) {
            av_log(s->avctx, AV_LOG_DEBUG,
                   "qp:%d fc:%d,%d %s pro:%d alt:%d top:%d %spel part:%d resync:%d w:%d a:%d rnd:%d vot:%d%s dc:%d ce:%d/%d/%d\n",
                   s->qscale, s->f_code, s->b_code,
                   s->pict_type == AV_PICTURE_TYPE_I ? "I" : (s->pict_type == AV_PICTURE_TYPE_P ? "P" : (s->pict_type == AV_PICTURE_TYPE_B ? "B" : "S")),
                   s->progressive_sequence, s->alternate_scan,
                   s->top_field_first, s->quarter_sample ? "q" : "h",
                   s->data_partitioning, ctx->resync_marker,
                   ctx->num_sprite_warping_points, s->sprite_warping_accuracy,
                   1 - s->no_rounding, s->vo_type,
                   ctx->vol_control_parameters ? " VOLC" : " ", ctx->intra_dc_threshold,
                   ctx->cplx_estimation_trash_i, ctx->cplx_estimation_trash_p,
                   ctx->cplx_estimation_trash_b);
        }

        if (!ctx->scalability) {
            if (ctx->shape != RECT_SHAPE && s->pict_type != AV_PICTURE_TYPE_I)
                bitstream_skip(bc, 1);  // vop shape coding type
        } else {
            if (ctx->enhancement_type) {
                int load_backward_shape = bitstream_read_bit(bc);
                if (load_backward_shape)
                    av_log(s->avctx, AV_LOG_ERROR,
                           "load backward shape isn't supported\n");
            }
            bitstream_skip(bc, 2);  // ref_select_code
        }
    }
    /* detect buggy encoders which don't set the low_delay flag
     * (divx4/xvid/opendivx). Note we cannot detect divx5 without B-frames
     * easily (although it's buggy too) */
    if (s->vo_type == 0 && ctx->vol_control_parameters == 0 &&
        ctx->divx_version == -1 && s->picture_number == 0) {
        av_log(s->avctx, AV_LOG_WARNING,
               "looks like this file was encoded with (divx4/(old)xvid/opendivx) -> forcing low_delay flag\n");
        s->low_delay = 1;
    }

    s->picture_number++;  // better than pic number==0 always ;)

    // FIXME add short header support
    s->y_dc_scale_table = ff_mpeg4_y_dc_scale_table;
    s->c_dc_scale_table = ff_mpeg4_c_dc_scale_table;

    if (s->workaround_bugs & FF_BUG_EDGE) {
        s->h_edge_pos = s->width;
        s->v_edge_pos = s->height;
    }
    return 0;
}

/**
 * Decode MPEG-4 headers.
 * @return <0 if no VOP found (or a damaged one)
 *         FRAME_SKIPPED if a not coded VOP is found
 *         0 if a VOP is found
 */
int ff_mpeg4_decode_picture_header(Mpeg4DecContext *ctx, BitstreamContext *bc)
{
    MpegEncContext *s = &ctx->m;
    unsigned startcode, v;

    /* search next start code */
    bitstream_align(bc);

    if (s->codec_tag == AV_RL32("WV1F") && bitstream_peek(bc, 24) == 0x575630) {
        bitstream_skip(bc, 24);
        if (bitstream_read(bc, 8) == 0xF0)
            goto end;
    }

    startcode = 0xff;
    for (;;) {
        if (bitstream_bits_left(bc) <= 0) {
            int size;
            if ((size = bitstream_bits_left(bc) + bitstream_tell(bc)) == 8 &&
                (ctx->divx_version >= 0 || ctx->xvid_build >= 0)) {
                av_log(s->avctx, AV_LOG_WARNING, "frame skip %d\n", size);
                return FRAME_SKIPPED;  // divx bug
            } else
                return -1;  // end of stream
        }

        /* use the bits after the test */
        v = bitstream_read(bc, 8);
        startcode = ((startcode << 8) | v) & 0xffffffff;

        if ((startcode & 0xFFFFFF00) != 0x100)
            continue;  // no startcode

        if (s->avctx->debug & FF_DEBUG_STARTCODE) {
            av_log(s->avctx, AV_LOG_DEBUG, "startcode: %3X ", startcode);
            if (startcode <= 0x11F)
                av_log(s->avctx, AV_LOG_DEBUG, "Video Object Start");
            else if (startcode <= 0x12F)
                av_log(s->avctx, AV_LOG_DEBUG, "Video Object Layer Start");
            else if (startcode <= 0x13F)
                av_log(s->avctx, AV_LOG_DEBUG, "Reserved");
            else if (startcode <= 0x15F)
                av_log(s->avctx, AV_LOG_DEBUG, "FGS bp start");
            else if (startcode <= 0x1AF)
                av_log(s->avctx, AV_LOG_DEBUG, "Reserved");
            else if (startcode == 0x1B0)
                av_log(s->avctx, AV_LOG_DEBUG, "Visual Object Seq Start");
            else if (startcode == 0x1B1)
                av_log(s->avctx, AV_LOG_DEBUG, "Visual Object Seq End");
            else if (startcode == 0x1B2)
                av_log(s->avctx, AV_LOG_DEBUG, "User Data");
            else if (startcode == 0x1B3)
                av_log(s->avctx, AV_LOG_DEBUG, "Group of VOP start");
            else if (startcode == 0x1B4)
                av_log(s->avctx, AV_LOG_DEBUG, "Video Session Error");
            else if (startcode == 0x1B5)
                av_log(s->avctx, AV_LOG_DEBUG, "Visual Object Start");
            else if (startcode == 0x1B6)
                av_log(s->avctx, AV_LOG_DEBUG, "Video Object Plane start");
            else if (startcode == 0x1B7)
                av_log(s->avctx, AV_LOG_DEBUG, "slice start");
            else if (startcode == 0x1B8)
                av_log(s->avctx, AV_LOG_DEBUG, "extension start");
            else if (startcode == 0x1B9)
                av_log(s->avctx, AV_LOG_DEBUG, "fgs start");
            else if (startcode == 0x1BA)
                av_log(s->avctx, AV_LOG_DEBUG, "FBA Object start");
            else if (startcode == 0x1BB)
                av_log(s->avctx, AV_LOG_DEBUG, "FBA Object Plane start");
            else if (startcode == 0x1BC)
                av_log(s->avctx, AV_LOG_DEBUG, "Mesh Object start");
            else if (startcode == 0x1BD)
                av_log(s->avctx, AV_LOG_DEBUG, "Mesh Object Plane start");
            else if (startcode == 0x1BE)
                av_log(s->avctx, AV_LOG_DEBUG, "Still Texture Object start");
            else if (startcode == 0x1BF)
                av_log(s->avctx, AV_LOG_DEBUG, "Texture Spatial Layer start");
            else if (startcode == 0x1C0)
                av_log(s->avctx, AV_LOG_DEBUG, "Texture SNR Layer start");
            else if (startcode == 0x1C1)
                av_log(s->avctx, AV_LOG_DEBUG, "Texture Tile start");
            else if (startcode == 0x1C2)
                av_log(s->avctx, AV_LOG_DEBUG, "Texture Shape Layer start");
            else if (startcode == 0x1C3)
                av_log(s->avctx, AV_LOG_DEBUG, "stuffing start");
            else if (startcode <= 0x1C5)
                av_log(s->avctx, AV_LOG_DEBUG, "reserved");
            else if (startcode <= 0x1FF)
                av_log(s->avctx, AV_LOG_DEBUG, "System start");
            av_log(s->avctx, AV_LOG_DEBUG, " at %d\n", bitstream_tell(bc));
        }

        if (startcode >= 0x120 && startcode <= 0x12F) {
            if (decode_vol_header(ctx, bc) < 0)
                return -1;
        } else if (startcode == USER_DATA_STARTCODE) {
            decode_user_data(ctx, bc);
        } else if (startcode == GOP_STARTCODE) {
            mpeg4_decode_gop_header(s, bc);
        } else if (startcode == VOS_STARTCODE) {
            mpeg4_decode_profile_level(s, bc);
        } else if (startcode == VOP_STARTCODE) {
            break;
        }

        bitstream_align(bc);
        startcode = 0xff;
    }

end:
    if (s->avctx->flags & AV_CODEC_FLAG_LOW_DELAY)
        s->low_delay = 1;
    s->avctx->has_b_frames = !s->low_delay;

    if (s->workaround_bugs & FF_BUG_AUTODETECT) {
        if (s->codec_tag == AV_RL32("XVIX"))
            s->workaround_bugs |= FF_BUG_XVID_ILACE;

        if (s->codec_tag == AV_RL32("UMP4"))
            s->workaround_bugs |= FF_BUG_UMP4;

        if (ctx->divx_version >= 500 && ctx->divx_build < 1814)
            s->workaround_bugs |= FF_BUG_QPEL_CHROMA;

        if (ctx->divx_version > 502 && ctx->divx_build < 1814)
            s->workaround_bugs |= FF_BUG_QPEL_CHROMA2;

        if (ctx->xvid_build <= 3U)
            s->padding_bug_score = 256 * 256 * 256 * 64;

        if (ctx->xvid_build <= 1U)
            s->workaround_bugs |= FF_BUG_QPEL_CHROMA;

        if (ctx->xvid_build <= 12U)
            s->workaround_bugs |= FF_BUG_EDGE;

        if (ctx->xvid_build <= 32U)
            s->workaround_bugs |= FF_BUG_DC_CLIP;

        if (ctx->lavc_build < 4653U)
            s->workaround_bugs |= FF_BUG_STD_QPEL;

        if (ctx->lavc_build < 4655U)
            s->workaround_bugs |= FF_BUG_DIRECT_BLOCKSIZE;

        if (ctx->lavc_build < 4670U)
            s->workaround_bugs |= FF_BUG_EDGE;

        if (ctx->lavc_build <= 4712U)
            s->workaround_bugs |= FF_BUG_DC_CLIP;

        if (ctx->divx_version >= 0)
            s->workaround_bugs |= FF_BUG_DIRECT_BLOCKSIZE;

        if (ctx->divx_version == 501 && ctx->divx_build == 20020416)
            s->padding_bug_score = 256 * 256 * 256 * 64;

        if (ctx->divx_version < 500U)
            s->workaround_bugs |= FF_BUG_EDGE;

        if (ctx->divx_version >= 0)
            s->workaround_bugs |= FF_BUG_HPEL_CHROMA;
    }


    if (s->avctx->debug & FF_DEBUG_BUGS)
        av_log(s->avctx, AV_LOG_DEBUG,
               "bugs: %X lavc_build:%d xvid_build:%d divx_version:%d divx_build:%d %s\n",
               s->workaround_bugs, ctx->lavc_build, ctx->xvid_build,
               ctx->divx_version, ctx->divx_build, s->divx_packed ? "p" : "");

    return decode_vop_header(ctx, bc);
}

int ff_mpeg4_frame_end(AVCodecContext *avctx, const uint8_t *buf, int buf_size)
{
    Mpeg4DecContext *ctx = avctx->priv_data;
    MpegEncContext    *s = &ctx->m;

    /* divx 5.01+ bitstream reorder stuff */
    if (s->divx_packed) {
        int current_pos     = bitstream_tell(&s->bc) >> 3;
        int startcode_found = 0;

        if (buf_size - current_pos > 5) {
            int i;
            for (i = current_pos; i < buf_size - 3; i++)
                if (buf[i]     == 0 &&
                    buf[i + 1] == 0 &&
                    buf[i + 2] == 1 &&
                    buf[i + 3] == 0xB6) {
                    startcode_found = 1;
                    break;
                }
        }
        if (s->bc.buffer == s->bitstream_buffer && buf_size > 7 &&
            ctx->xvid_build >= 0) {       // xvid style
            startcode_found = 1;
            current_pos     = 0;
        }

        if (startcode_found) {
            av_fast_malloc(&s->bitstream_buffer,
                           &s->allocated_bitstream_buffer_size,
                           buf_size - current_pos +
                           AV_INPUT_BUFFER_PADDING_SIZE);
            if (!s->bitstream_buffer)
                return AVERROR(ENOMEM);
            memcpy(s->bitstream_buffer, buf + current_pos,
                   buf_size - current_pos);
            s->bitstream_buffer_size = buf_size - current_pos;
        }
    }

    return 0;
}

static int mpeg4_update_thread_context(AVCodecContext *dst,
                                       const AVCodecContext *src)
{
    Mpeg4DecContext *s = dst->priv_data;
    const Mpeg4DecContext *s1 = src->priv_data;
    int init = s->m.context_initialized;

    int ret = ff_mpeg_update_thread_context(dst, src);

    if (ret < 0)
        return ret;

    if (CONFIG_MPEG4_DECODER && !init && s1->xvid_build >= 0)
        ff_xvid_idct_init(&s->m.idsp, dst);

    s->shape               = s1->shape;
    s->time_increment_bits = s1->time_increment_bits;
    s->xvid_build          = s1->xvid_build;

    return 0;
}

static av_cold int decode_init(AVCodecContext *avctx)
{
    Mpeg4DecContext *ctx = avctx->priv_data;
    MpegEncContext *s = &ctx->m;
    int ret;
    static int done = 0;

    ctx->divx_version =
    ctx->divx_build   =
    ctx->xvid_build   =
    ctx->lavc_build   = -1;

    if ((ret = ff_h263_decode_init(avctx)) < 0)
        return ret;

    if (!done) {
        done = 1;

        ff_rl_init(&ff_mpeg4_rl_intra, ff_mpeg4_static_rl_table_store[0]);
        ff_rl_init(&ff_rvlc_rl_inter, ff_mpeg4_static_rl_table_store[1]);
        ff_rl_init(&ff_rvlc_rl_intra, ff_mpeg4_static_rl_table_store[2]);
        INIT_VLC_RL(ff_mpeg4_rl_intra, 554);
        INIT_VLC_RL(ff_rvlc_rl_inter, 1072);
        INIT_VLC_RL(ff_rvlc_rl_intra, 1072);
        INIT_VLC_STATIC(&dc_lum, DC_VLC_BITS, 10 /* 13 */,
                        &ff_mpeg4_DCtab_lum[0][1], 2, 1,
                        &ff_mpeg4_DCtab_lum[0][0], 2, 1, 512);
        INIT_VLC_STATIC(&dc_chrom, DC_VLC_BITS, 10 /* 13 */,
                        &ff_mpeg4_DCtab_chrom[0][1], 2, 1,
                        &ff_mpeg4_DCtab_chrom[0][0], 2, 1, 512);
        INIT_VLC_STATIC(&sprite_trajectory, SPRITE_TRAJ_VLC_BITS, 15,
                        &ff_sprite_trajectory_tab[0][1], 4, 2,
                        &ff_sprite_trajectory_tab[0][0], 4, 2, 128);
        INIT_VLC_STATIC(&mb_type_b_vlc, MB_TYPE_B_VLC_BITS, 4,
                        &ff_mb_type_b_tab[0][1], 2, 1,
                        &ff_mb_type_b_tab[0][0], 2, 1, 16);
    }

    s->h263_pred = 1;
    s->low_delay = 0; /* default, might be overridden in the vol header during header parsing */
    s->decode_mb = mpeg4_decode_mb;
    ctx->time_increment_bits = 4; /* default value for broken headers */

    avctx->chroma_sample_location = AVCHROMA_LOC_LEFT;
    avctx->internal->allocate_progress = 1;

    return 0;
}

AVCodec ff_mpeg4_decoder = {
    .name                  = "mpeg4",
    .long_name             = NULL_IF_CONFIG_SMALL("MPEG-4 part 2"),
    .type                  = AVMEDIA_TYPE_VIDEO,
    .id                    = AV_CODEC_ID_MPEG4,
    .priv_data_size        = sizeof(Mpeg4DecContext),
    .init                  = decode_init,
    .close                 = ff_h263_decode_end,
    .decode                = ff_h263_decode_frame,
    .capabilities          = AV_CODEC_CAP_DRAW_HORIZ_BAND | AV_CODEC_CAP_DR1 |
                             AV_CODEC_CAP_TRUNCATED | AV_CODEC_CAP_DELAY |
                             AV_CODEC_CAP_FRAME_THREADS,
    .flush                 = ff_mpeg_flush,
    .pix_fmts              = ff_h263_hwaccel_pixfmt_list_420,
    .profiles              = NULL_IF_CONFIG_SMALL(ff_mpeg4_video_profiles),
    .update_thread_context = ONLY_IF_THREADS_ENABLED(mpeg4_update_thread_context),
};

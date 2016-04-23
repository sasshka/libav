/*
 * H.263i decoder
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
#include "mpegutils.h"
#include "mpegvideo.h"
#include "h263.h"
#include "mpegvideodata.h"

/* don't understand why they choose a different header ! */
int ff_intel_h263_decode_picture_header(MpegEncContext *s)
{
    int format;

    if (bitstream_bits_left(&s->bc) == 64) { /* special dummy frames */
        return FRAME_SKIPPED;
    }

    /* picture header */
    if (bitstream_read(&s->bc, 22) != 0x20) {
        av_log(s->avctx, AV_LOG_ERROR, "Bad picture start code\n");
        return -1;
    }
    s->picture_number = bitstream_read(&s->bc, 8); /* picture timestamp */

    if (bitstream_read_bit(&s->bc) != 1) {
        av_log(s->avctx, AV_LOG_ERROR, "Bad marker\n");
        return -1;      /* marker */
    }
    if (bitstream_read_bit(&s->bc) != 0) {
        av_log(s->avctx, AV_LOG_ERROR, "Bad H.263 id\n");
        return -1;      /* h263 id */
    }
    bitstream_skip(&s->bc, 1);         /* split screen off */
    bitstream_skip(&s->bc, 1);         /* camera  off */
    bitstream_skip(&s->bc, 1);         /* freeze picture release off */

    format = bitstream_read(&s->bc, 3);
    if (format == 0 || format == 6) {
        av_log(s->avctx, AV_LOG_ERROR, "Intel H.263 free format not supported\n");
        return -1;
    }
    s->h263_plus = 0;

    s->pict_type = AV_PICTURE_TYPE_I + bitstream_read_bit(&s->bc);

    s->unrestricted_mv = bitstream_read_bit(&s->bc);
    s->h263_long_vectors = s->unrestricted_mv;

    if (bitstream_read_bit(&s->bc) != 0) {
        av_log(s->avctx, AV_LOG_ERROR, "SAC not supported\n");
        return -1;      /* SAC: off */
    }
    s->obmc= bitstream_read_bit(&s->bc);
    s->pb_frame = bitstream_read_bit(&s->bc);

    if (format < 6) {
        s->width = ff_h263_format[format][0];
        s->height = ff_h263_format[format][1];
        s->avctx->sample_aspect_ratio.num = 12;
        s->avctx->sample_aspect_ratio.den = 11;
    } else {
        format = bitstream_read(&s->bc, 3);
        if(format == 0 || format == 7){
            av_log(s->avctx, AV_LOG_ERROR, "Wrong Intel H.263 format\n");
            return -1;
        }
        if (bitstream_read(&s->bc, 2))
            av_log(s->avctx, AV_LOG_ERROR, "Bad value for reserved field\n");
        s->loop_filter = bitstream_read_bit(&s->bc);
        if (bitstream_read_bit(&s->bc))
            av_log(s->avctx, AV_LOG_ERROR, "Bad value for reserved field\n");
        if (bitstream_read_bit(&s->bc))
            s->pb_frame = 2;
        if (bitstream_read(&s->bc, 5))
            av_log(s->avctx, AV_LOG_ERROR, "Bad value for reserved field\n");
        if (bitstream_read(&s->bc, 5) != 1)
            av_log(s->avctx, AV_LOG_ERROR, "Invalid marker\n");
    }
    if(format == 6){
        int ar = bitstream_read(&s->bc, 4);
        bitstream_skip(&s->bc, 9); // display width
        bitstream_skip(&s->bc, 1);
        bitstream_skip(&s->bc, 9); // display height
        if(ar == 15){
            s->avctx->sample_aspect_ratio.num = bitstream_read(&s->bc, 8); // aspect ratio - width
            s->avctx->sample_aspect_ratio.den = bitstream_read(&s->bc, 8); // aspect ratio - height
        } else {
            s->avctx->sample_aspect_ratio = ff_h263_pixel_aspect[ar];
        }
        if (s->avctx->sample_aspect_ratio.num == 0)
            av_log(s->avctx, AV_LOG_ERROR, "Invalid aspect ratio.\n");
    }

    s->chroma_qscale= s->qscale = bitstream_read(&s->bc, 5);
    bitstream_skip(&s->bc, 1); /* Continuous Presence Multipoint mode: off */

    if(s->pb_frame){
        bitstream_skip(&s->bc, 3); //temporal reference for B-frame
        bitstream_skip(&s->bc, 2); //dbquant
    }

    /* PEI */
    while (bitstream_read_bit(&s->bc) != 0) {
        bitstream_skip(&s->bc, 8);
    }
    s->f_code = 1;

    s->y_dc_scale_table=
    s->c_dc_scale_table= ff_mpeg1_dc_scale_table;

    ff_h263_show_pict_info(s);

    return 0;
}

AVCodec ff_h263i_decoder = {
    .name           = "h263i",
    .long_name      = NULL_IF_CONFIG_SMALL("Intel H.263"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_H263I,
    .priv_data_size = sizeof(MpegEncContext),
    .init           = ff_h263_decode_init,
    .close          = ff_h263_decode_end,
    .decode         = ff_h263_decode_frame,
    .capabilities   = AV_CODEC_CAP_DRAW_HORIZ_BAND | AV_CODEC_CAP_DR1,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE
    },
};

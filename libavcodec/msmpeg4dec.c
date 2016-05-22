/*
 * MSMPEG4 backend for encoder and decoder
 * Copyright (c) 2001 Fabrice Bellard
 * Copyright (c) 2002-2004 Michael Niedermayer <michaelni@gmx.at>
 *
 * msmpeg4v1 & v2 stuff by Michael Niedermayer <michaelni@gmx.at>
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

#include "avcodec.h"
#include "bitstream.h"
#include "internal.h"
#include "mpegutils.h"
#include "mpegvideo.h"
#include "msmpeg4.h"
#include "libavutil/x86/asm.h"
#include "h263.h"
#include "mpeg4video.h"
#include "msmpeg4data.h"
#include "vc1data.h"
#include "vlc.h"
#include "wmv2.h"

#define DC_VLC_BITS 9
#define V2_INTRA_CBPC_VLC_BITS 3
#define V2_MB_TYPE_VLC_BITS 7
#define MV_VLC_BITS 9
#define V2_MV_VLC_BITS 9
#define TEX_VLC_BITS 9

#define DEFAULT_INTER_INDEX 3

static inline int msmpeg4v1_pred_dc(MpegEncContext * s, int n,
                                    int32_t **dc_val_ptr)
{
    int i;

    if (n < 4) {
        i= 0;
    } else {
        i= n-3;
    }

    *dc_val_ptr= &s->last_dc[i];
    return s->last_dc[i];
}

/****************************************/
/* decoding stuff */

VLC ff_mb_non_intra_vlc[4];
static VLC v2_dc_lum_vlc;
static VLC v2_dc_chroma_vlc;
static VLC v2_intra_cbpc_vlc;
static VLC v2_mb_type_vlc;
static VLC v2_mv_vlc;
VLC ff_inter_intra_vlc;

/* This is identical to H.263 except that its range is multiplied by 2. */
static int msmpeg4v2_decode_motion(MpegEncContext * s, int pred, int f_code)
{
    int code, val, sign, shift;

    code = bitstream_read_vlc(&s->bc, v2_mv_vlc.table, V2_MV_VLC_BITS, 2);
    ff_dlog(s, "MV code %d at %d %d pred: %d\n", code, s->mb_x,s->mb_y, pred);
    if (code < 0)
        return 0xffff;

    if (code == 0)
        return pred;
    sign = bitstream_read_bit(&s->bc);
    shift = f_code - 1;
    val = code;
    if (shift) {
        val = (val - 1) << shift;
        val |= bitstream_read(&s->bc, shift);
        val++;
    }
    if (sign)
        val = -val;

    val += pred;
    if (val <= -64)
        val += 64;
    else if (val >= 64)
        val -= 64;

    return val;
}

static int msmpeg4v12_decode_mb(MpegEncContext *s, int16_t block[6][64])
{
    int cbp, code, i;

    if (s->pict_type == AV_PICTURE_TYPE_P) {
        if (s->use_skip_mb_code) {
            if (bitstream_read_bit(&s->bc)) {
                /* skip mb */
                s->mb_intra = 0;
                for(i=0;i<6;i++)
                    s->block_last_index[i] = -1;
                s->mv_dir = MV_DIR_FORWARD;
                s->mv_type = MV_TYPE_16X16;
                s->mv[0][0][0] = 0;
                s->mv[0][0][1] = 0;
                s->mb_skipped = 1;
                return 0;
            }
        }

        if(s->msmpeg4_version==2)
            code = bitstream_read_vlc(&s->bc, v2_mb_type_vlc.table,
                                      V2_MB_TYPE_VLC_BITS, 1);
        else
            code = bitstream_read_vlc(&s->bc, ff_h263_inter_MCBPC_vlc.table,
                                      INTER_MCBPC_VLC_BITS, 2);
        if(code<0 || code>7){
            av_log(s->avctx, AV_LOG_ERROR, "cbpc %d invalid at %d %d\n", code, s->mb_x, s->mb_y);
            return -1;
        }

        s->mb_intra = code >>2;

        cbp = code & 0x3;
    } else {
        s->mb_intra = 1;
        if(s->msmpeg4_version==2)
            cbp= bitstream_read_vlc(&s->bc, v2_intra_cbpc_vlc.table,
                                    V2_INTRA_CBPC_VLC_BITS, 1);
        else
            cbp= bitstream_read_vlc(&s->bc, ff_h263_intra_MCBPC_vlc.table,
                                    INTRA_MCBPC_VLC_BITS, 1);
        if(cbp<0 || cbp>3){
            av_log(s->avctx, AV_LOG_ERROR, "cbpc %d invalid at %d %d\n", cbp, s->mb_x, s->mb_y);
            return -1;
        }
    }

    if (!s->mb_intra) {
        int mx, my, cbpy;

        cbpy= bitstream_read_vlc(&s->bc, ff_h263_cbpy_vlc.table, CBPY_VLC_BITS, 1);
        if(cbpy<0){
            av_log(s->avctx, AV_LOG_ERROR, "cbpy %d invalid at %d %d\n", cbp, s->mb_x, s->mb_y);
            return -1;
        }

        cbp|= cbpy<<2;
        if(s->msmpeg4_version==1 || (cbp&3) != 3) cbp^= 0x3C;

        ff_h263_pred_motion(s, 0, 0, &mx, &my);
        mx= msmpeg4v2_decode_motion(s, mx, 1);
        my= msmpeg4v2_decode_motion(s, my, 1);

        s->mv_dir = MV_DIR_FORWARD;
        s->mv_type = MV_TYPE_16X16;
        s->mv[0][0][0] = mx;
        s->mv[0][0][1] = my;
    } else {
        if(s->msmpeg4_version==2){
            s->ac_pred = bitstream_read_bit(&s->bc);
            cbp|= bitstream_read_vlc(&s->bc, ff_h263_cbpy_vlc.table,
                                     CBPY_VLC_BITS, 1) << 2; //FIXME check errors
        } else{
            s->ac_pred = 0;
            cbp|= bitstream_read_vlc(&s->bc, ff_h263_cbpy_vlc.table,
                                     CBPY_VLC_BITS, 1) << 2; //FIXME check errors
            if(s->pict_type==AV_PICTURE_TYPE_P) cbp^=0x3C;
        }
    }

    s->bdsp.clear_blocks(s->block[0]);
    for (i = 0; i < 6; i++) {
        if (ff_msmpeg4_decode_block(s, block[i], i, (cbp >> (5 - i)) & 1, NULL) < 0)
        {
             av_log(s->avctx, AV_LOG_ERROR, "\nerror while decoding block: %d x %d (%d)\n", s->mb_x, s->mb_y, i);
             return -1;
        }
    }
    return 0;
}

static int msmpeg4v34_decode_mb(MpegEncContext *s, int16_t block[6][64])
{
    int cbp, code, i;
    uint8_t *coded_val;
    uint32_t * const mb_type_ptr = &s->current_picture.mb_type[s->mb_x + s->mb_y*s->mb_stride];

    if (s->pict_type == AV_PICTURE_TYPE_P) {
        if (s->use_skip_mb_code) {
            if (bitstream_read_bit(&s->bc)) {
                /* skip mb */
                s->mb_intra = 0;
                for(i=0;i<6;i++)
                    s->block_last_index[i] = -1;
                s->mv_dir = MV_DIR_FORWARD;
                s->mv_type = MV_TYPE_16X16;
                s->mv[0][0][0] = 0;
                s->mv[0][0][1] = 0;
                s->mb_skipped = 1;
                *mb_type_ptr = MB_TYPE_SKIP | MB_TYPE_L0 | MB_TYPE_16x16;

                return 0;
            }
        }

        code = bitstream_read_vlc(&s->bc, ff_mb_non_intra_vlc[DEFAULT_INTER_INDEX].table,
                                  MB_NON_INTRA_VLC_BITS, 3);
        if (code < 0)
            return -1;
        //s->mb_intra = (code & 0x40) ? 0 : 1;
        s->mb_intra = (~code & 0x40) >> 6;

        cbp = code & 0x3f;
    } else {
        s->mb_intra = 1;
        code = bitstream_read_vlc(&s->bc, ff_msmp4_mb_i_vlc.table, MB_INTRA_VLC_BITS, 2);
        if (code < 0)
            return -1;
        /* predict coded block pattern */
        cbp = 0;
        for(i=0;i<6;i++) {
            int val = ((code >> (5 - i)) & 1);
            if (i < 4) {
                int pred = ff_msmpeg4_coded_block_pred(s, i, &coded_val);
                val = val ^ pred;
                *coded_val = val;
            }
            cbp |= val << (5 - i);
        }
    }

    if (!s->mb_intra) {
        int mx, my;
        if(s->per_mb_rl_table && cbp){
            s->rl_table_index = bitstream_decode012(&s->bc);
            s->rl_chroma_table_index = s->rl_table_index;
        }
        ff_h263_pred_motion(s, 0, 0, &mx, &my);
        if (ff_msmpeg4_decode_motion(s, &mx, &my) < 0)
            return -1;
        s->mv_dir = MV_DIR_FORWARD;
        s->mv_type = MV_TYPE_16X16;
        s->mv[0][0][0] = mx;
        s->mv[0][0][1] = my;
        *mb_type_ptr = MB_TYPE_L0 | MB_TYPE_16x16;
    } else {
        ff_dlog(s, "I at %d %d %d %06X\n", s->mb_x, s->mb_y,
                ((cbp & 3) ? 1 : 0) +((cbp & 0x3C)? 2 : 0),
                bitstream_peek(&s->bc, 24));
        s->ac_pred = bitstream_read_bit(&s->bc);
        *mb_type_ptr = MB_TYPE_INTRA;
        if(s->inter_intra_pred){
            s->h263_aic_dir = bitstream_read_vlc(&s->bc, ff_inter_intra_vlc.table,
                                                INTER_INTRA_VLC_BITS, 1);
            ff_dlog(s, "%d%d %d %d/",
                    s->ac_pred, s->h263_aic_dir, s->mb_x, s->mb_y);
        }
        if(s->per_mb_rl_table && cbp){
            s->rl_table_index = bitstream_decode012(&s->bc);
            s->rl_chroma_table_index = s->rl_table_index;
        }
    }

    s->bdsp.clear_blocks(s->block[0]);
    for (i = 0; i < 6; i++) {
        if (ff_msmpeg4_decode_block(s, block[i], i, (cbp >> (5 - i)) & 1, NULL) < 0)
        {
            av_log(s->avctx, AV_LOG_ERROR, "\nerror while decoding block: %d x %d (%d)\n", s->mb_x, s->mb_y, i);
            return -1;
        }
    }

    return 0;
}

/* init all vlc decoding tables */
av_cold int ff_msmpeg4_decode_init(AVCodecContext *avctx)
{
    MpegEncContext *s = avctx->priv_data;
    static int done = 0;
    int i;
    MVTable *mv;

    if (ff_h263_decode_init(avctx) < 0)
        return -1;

    ff_msmpeg4_common_init(s);

    if (!done) {
        done = 1;

        for(i=0;i<NB_RL_TABLES;i++) {
            ff_rl_init(&ff_rl_table[i], ff_static_rl_table_store[i]);
        }
        INIT_VLC_RL(ff_rl_table[0], 642);
        INIT_VLC_RL(ff_rl_table[1], 1104);
        INIT_VLC_RL(ff_rl_table[2], 554);
        INIT_VLC_RL(ff_rl_table[3], 940);
        INIT_VLC_RL(ff_rl_table[4], 962);
        INIT_VLC_RL(ff_rl_table[5], 554);

        mv = &ff_mv_tables[0];
        INIT_VLC_STATIC(&mv->vlc, MV_VLC_BITS, mv->n + 1,
                    mv->table_mv_bits, 1, 1,
                    mv->table_mv_code, 2, 2, 3714);
        mv = &ff_mv_tables[1];
        INIT_VLC_STATIC(&mv->vlc, MV_VLC_BITS, mv->n + 1,
                    mv->table_mv_bits, 1, 1,
                    mv->table_mv_code, 2, 2, 2694);

        INIT_VLC_STATIC(&ff_msmp4_dc_luma_vlc[0], DC_VLC_BITS, 120,
                 &ff_table0_dc_lum[0][1], 8, 4,
                 &ff_table0_dc_lum[0][0], 8, 4, 1158);
        INIT_VLC_STATIC(&ff_msmp4_dc_chroma_vlc[0], DC_VLC_BITS, 120,
                 &ff_table0_dc_chroma[0][1], 8, 4,
                 &ff_table0_dc_chroma[0][0], 8, 4, 1118);
        INIT_VLC_STATIC(&ff_msmp4_dc_luma_vlc[1], DC_VLC_BITS, 120,
                 &ff_table1_dc_lum[0][1], 8, 4,
                 &ff_table1_dc_lum[0][0], 8, 4, 1476);
        INIT_VLC_STATIC(&ff_msmp4_dc_chroma_vlc[1], DC_VLC_BITS, 120,
                 &ff_table1_dc_chroma[0][1], 8, 4,
                 &ff_table1_dc_chroma[0][0], 8, 4, 1216);

        INIT_VLC_STATIC(&v2_dc_lum_vlc, DC_VLC_BITS, 512,
                 &ff_v2_dc_lum_table[0][1], 8, 4,
                 &ff_v2_dc_lum_table[0][0], 8, 4, 1472);
        INIT_VLC_STATIC(&v2_dc_chroma_vlc, DC_VLC_BITS, 512,
                 &ff_v2_dc_chroma_table[0][1], 8, 4,
                 &ff_v2_dc_chroma_table[0][0], 8, 4, 1506);

        INIT_VLC_STATIC(&v2_intra_cbpc_vlc, V2_INTRA_CBPC_VLC_BITS, 4,
                 &ff_v2_intra_cbpc[0][1], 2, 1,
                 &ff_v2_intra_cbpc[0][0], 2, 1, 8);
        INIT_VLC_STATIC(&v2_mb_type_vlc, V2_MB_TYPE_VLC_BITS, 8,
                 &ff_v2_mb_type[0][1], 2, 1,
                 &ff_v2_mb_type[0][0], 2, 1, 128);
        INIT_VLC_STATIC(&v2_mv_vlc, V2_MV_VLC_BITS, 33,
                 &ff_mvtab[0][1], 2, 1,
                 &ff_mvtab[0][0], 2, 1, 538);

        INIT_VLC_STATIC(&ff_mb_non_intra_vlc[0], MB_NON_INTRA_VLC_BITS, 128,
                     &ff_wmv2_inter_table[0][0][1], 8, 4,
                     &ff_wmv2_inter_table[0][0][0], 8, 4, 1636);
        INIT_VLC_STATIC(&ff_mb_non_intra_vlc[1], MB_NON_INTRA_VLC_BITS, 128,
                     &ff_wmv2_inter_table[1][0][1], 8, 4,
                     &ff_wmv2_inter_table[1][0][0], 8, 4, 2648);
        INIT_VLC_STATIC(&ff_mb_non_intra_vlc[2], MB_NON_INTRA_VLC_BITS, 128,
                     &ff_wmv2_inter_table[2][0][1], 8, 4,
                     &ff_wmv2_inter_table[2][0][0], 8, 4, 1532);
        INIT_VLC_STATIC(&ff_mb_non_intra_vlc[3], MB_NON_INTRA_VLC_BITS, 128,
                     &ff_wmv2_inter_table[3][0][1], 8, 4,
                     &ff_wmv2_inter_table[3][0][0], 8, 4, 2488);

        INIT_VLC_STATIC(&ff_msmp4_mb_i_vlc, MB_INTRA_VLC_BITS, 64,
                 &ff_msmp4_mb_i_table[0][1], 4, 2,
                 &ff_msmp4_mb_i_table[0][0], 4, 2, 536);

        INIT_VLC_STATIC(&ff_inter_intra_vlc, INTER_INTRA_VLC_BITS, 4,
                 &ff_table_inter_intra[0][1], 2, 1,
                 &ff_table_inter_intra[0][0], 2, 1, 8);
    }

    switch(s->msmpeg4_version){
    case 1:
    case 2:
        s->decode_mb= msmpeg4v12_decode_mb;
        break;
    case 3:
    case 4:
        s->decode_mb= msmpeg4v34_decode_mb;
        break;
    case 5:
        if (CONFIG_WMV2_DECODER)
            s->decode_mb= ff_wmv2_decode_mb;
    case 6:
        //FIXME + TODO VC1 decode mb
        break;
    }

    s->slice_height= s->mb_height; //to avoid 1/0 if the first frame is not a keyframe

    return 0;
}

int ff_msmpeg4_decode_picture_header(MpegEncContext * s)
{
    int code;

    if(s->msmpeg4_version==1){
        int start_code = bitstream_read(&s->bc, 32);
        if(start_code!=0x00000100){
            av_log(s->avctx, AV_LOG_ERROR, "invalid startcode\n");
            return -1;
        }

        bitstream_skip(&s->bc, 5); // frame number */
    }

    s->pict_type = bitstream_read(&s->bc, 2) + 1;
    if (s->pict_type != AV_PICTURE_TYPE_I &&
        s->pict_type != AV_PICTURE_TYPE_P){
        av_log(s->avctx, AV_LOG_ERROR, "invalid picture type\n");
        return -1;
    }
#if 0
{
    static int had_i=0;
    if(s->pict_type == AV_PICTURE_TYPE_I) had_i=1;
    if(!had_i) return -1;
}
#endif
    s->chroma_qscale= s->qscale = bitstream_read(&s->bc, 5);
    if(s->qscale==0){
        av_log(s->avctx, AV_LOG_ERROR, "invalid qscale\n");
        return -1;
    }

    if (s->pict_type == AV_PICTURE_TYPE_I) {
        code = bitstream_read(&s->bc, 5);
        if(s->msmpeg4_version==1){
            if(code==0 || code>s->mb_height){
                av_log(s->avctx, AV_LOG_ERROR, "invalid slice height %d\n", code);
                return -1;
            }

            s->slice_height = code;
        }else{
            /* 0x17: one slice, 0x18: two slices, ... */
            if (code < 0x17){
                av_log(s->avctx, AV_LOG_ERROR, "error, slice code was %X\n", code);
                return -1;
            }

            s->slice_height = s->mb_height / (code - 0x16);
        }

        switch(s->msmpeg4_version){
        case 1:
        case 2:
            s->rl_chroma_table_index = 2;
            s->rl_table_index = 2;

            s->dc_table_index = 0; //not used
            break;
        case 3:
            s->rl_chroma_table_index = bitstream_decode012(&s->bc);
            s->rl_table_index = bitstream_decode012(&s->bc);

            s->dc_table_index = bitstream_read_bit(&s->bc);
            break;
        case 4:
            ff_msmpeg4_decode_ext_header(s, (2+5+5+17+7)/8);

            if(s->bit_rate > MBAC_BITRATE) s->per_mb_rl_table = bitstream_read_bit(&s->bc);
            else                           s->per_mb_rl_table= 0;

            if(!s->per_mb_rl_table){
                s->rl_chroma_table_index = bitstream_decode012(&s->bc);
                s->rl_table_index = bitstream_decode012(&s->bc);
            }

            s->dc_table_index = bitstream_read_bit(&s->bc);
            s->inter_intra_pred= 0;
            break;
        }
        s->no_rounding = 1;
        if(s->avctx->debug&FF_DEBUG_PICT_INFO)
            av_log(s->avctx, AV_LOG_DEBUG, "qscale:%d rlc:%d rl:%d dc:%d mbrl:%d slice:%d   \n",
                s->qscale,
                s->rl_chroma_table_index,
                s->rl_table_index,
                s->dc_table_index,
                s->per_mb_rl_table,
                s->slice_height);
    } else {
        switch(s->msmpeg4_version){
        case 1:
        case 2:
            if(s->msmpeg4_version==1)
                s->use_skip_mb_code = 1;
            else
                s->use_skip_mb_code = bitstream_read_bit(&s->bc);
            s->rl_table_index = 2;
            s->rl_chroma_table_index = s->rl_table_index;
            s->dc_table_index = 0; //not used
            s->mv_table_index = 0;
            break;
        case 3:
            s->use_skip_mb_code = bitstream_read_bit(&s->bc);
            s->rl_table_index = bitstream_decode012(&s->bc);
            s->rl_chroma_table_index = s->rl_table_index;

            s->dc_table_index = bitstream_read_bit(&s->bc);

            s->mv_table_index = bitstream_read_bit(&s->bc);
            break;
        case 4:
            s->use_skip_mb_code = bitstream_read_bit(&s->bc);

            if (s->bit_rate > MBAC_BITRATE)
                s->per_mb_rl_table= bitstream_read_bit(&s->bc);
            else                           s->per_mb_rl_table= 0;

            if(!s->per_mb_rl_table){
                s->rl_table_index = bitstream_decode012(&s->bc);
                s->rl_chroma_table_index = s->rl_table_index;
            }

            s->dc_table_index = bitstream_read_bit(&s->bc);

            s->mv_table_index = bitstream_read_bit(&s->bc);
            s->inter_intra_pred= (s->width*s->height < 320*240 && s->bit_rate<=II_BITRATE);
            break;
        }

        if(s->avctx->debug&FF_DEBUG_PICT_INFO)
            av_log(s->avctx, AV_LOG_DEBUG, "skip:%d rl:%d rlc:%d dc:%d mv:%d mbrl:%d qp:%d   \n",
                s->use_skip_mb_code,
                s->rl_table_index,
                s->rl_chroma_table_index,
                s->dc_table_index,
                s->mv_table_index,
                s->per_mb_rl_table,
                s->qscale);

        if(s->flipflop_rounding){
            s->no_rounding ^= 1;
        }else{
            s->no_rounding = 0;
        }
    }
    ff_dlog(s->avctx, "%d %d %d %d %d\n", s->pict_type, s->bit_rate,
            s->inter_intra_pred, s->width, s->height);

    s->esc3_level_length= 0;
    s->esc3_run_length= 0;

    return 0;
}

int ff_msmpeg4_decode_ext_header(MpegEncContext * s, int buf_size)
{
    int left = buf_size * 8 - bitstream_tell(&s->bc);
    int length= s->msmpeg4_version>=3 ? 17 : 16;
    /* the alt_bitstream reader could read over the end so we need to check it */
    if(left>=length && left<length+8)
    {
        bitstream_skip(&s->bc, 5); /* fps */
        s->bit_rate = bitstream_read(&s->bc, 11) * 1024;
        if(s->msmpeg4_version>=3)
            s->flipflop_rounding = bitstream_read_bit(&s->bc);
        else
            s->flipflop_rounding= 0;
    }
    else if(left<length+8)
    {
        s->flipflop_rounding= 0;
        if(s->msmpeg4_version != 2)
            av_log(s->avctx, AV_LOG_ERROR, "ext header missing, %d left\n", left);
    }
    else
    {
        av_log(s->avctx, AV_LOG_ERROR, "I-frame too long, ignoring ext header\n");
    }

    return 0;
}

static int msmpeg4_decode_dc(MpegEncContext * s, int n, int *dir_ptr)
{
    int level, pred;

    if(s->msmpeg4_version<=2){
        if (n < 4) {
            level = bitstream_read_vlc(&s->bc, v2_dc_lum_vlc.table, DC_VLC_BITS, 3);
        } else {
            level = bitstream_read_vlc(&s->bc, v2_dc_chroma_vlc.table, DC_VLC_BITS, 3);
        }
        if (level < 0)
            return -1;
        level-=256;
    }else{  //FIXME optimize use unified tables & index
        if (n < 4) {
            level = bitstream_read_vlc(&s->bc, ff_msmp4_dc_luma_vlc[s->dc_table_index].table,
                                       DC_VLC_BITS, 3);
        } else {
            level = bitstream_read_vlc(&s->bc, ff_msmp4_dc_chroma_vlc[s->dc_table_index].table,
                                       DC_VLC_BITS, 3);
        }
        if (level < 0){
            av_log(s->avctx, AV_LOG_ERROR, "illegal dc vlc\n");
            return -1;
        }

        if (level == DC_MAX) {
            level = bitstream_read(&s->bc, 8);
            if (bitstream_read_bit(&s->bc))
                level = -level;
        } else if (level != 0) {
            if (bitstream_read_bit(&s->bc))
                level = -level;
        }
    }

    if(s->msmpeg4_version==1){
        int32_t *dc_val;
        pred = msmpeg4v1_pred_dc(s, n, &dc_val);
        level += pred;

        /* update predictor */
        *dc_val= level;
    }else{
        int16_t *dc_val;
        pred   = ff_msmpeg4_pred_dc(s, n, &dc_val, dir_ptr);
        level += pred;

        /* update predictor */
        if (n < 4) {
            *dc_val = level * s->y_dc_scale;
        } else {
            *dc_val = level * s->c_dc_scale;
        }
    }

    return level;
}

//#define ERROR_DETAILS
int ff_msmpeg4_decode_block(MpegEncContext * s, int16_t * block,
                              int n, int coded, const uint8_t *scan_table)
{
    int level, i, last, run, run_diff;
    int av_uninit(dc_pred_dir);
    RLTable *rl;
    RL_VLC_ELEM *rl_vlc;
    int qmul, qadd;

    if (s->mb_intra) {
        qmul=1;
        qadd=0;

        /* DC coef */
        level = msmpeg4_decode_dc(s, n, &dc_pred_dir);

        if (level < 0){
            av_log(s->avctx, AV_LOG_ERROR, "dc overflow- block: %d qscale: %d//\n", n, s->qscale);
            if(s->inter_intra_pred) level=0;
            else                    return -1;
        }
        if (n < 4) {
            rl = &ff_rl_table[s->rl_table_index];
            if(level > 256*s->y_dc_scale){
                av_log(s->avctx, AV_LOG_ERROR, "dc overflow+ L qscale: %d//\n", s->qscale);
                if(!s->inter_intra_pred) return -1;
            }
        } else {
            rl = &ff_rl_table[3 + s->rl_chroma_table_index];
            if(level > 256*s->c_dc_scale){
                av_log(s->avctx, AV_LOG_ERROR, "dc overflow+ C qscale: %d//\n", s->qscale);
                if(!s->inter_intra_pred) return -1;
            }
        }
        block[0] = level;

        run_diff = s->msmpeg4_version >= 4;
        i = 0;
        if (!coded) {
            goto not_coded;
        }
        if (s->ac_pred) {
            if (dc_pred_dir == 0)
                scan_table = s->intra_v_scantable.permutated; /* left */
            else
                scan_table = s->intra_h_scantable.permutated; /* top */
        } else {
            scan_table = s->intra_scantable.permutated;
        }
        rl_vlc= rl->rl_vlc[0];
    } else {
        qmul = s->qscale << 1;
        qadd = (s->qscale - 1) | 1;
        i = -1;
        rl = &ff_rl_table[3 + s->rl_table_index];

        if(s->msmpeg4_version==2)
            run_diff = 0;
        else
            run_diff = 1;

        if (!coded) {
            s->block_last_index[n] = i;
            return 0;
        }
        if(!scan_table)
            scan_table = s->inter_scantable.permutated;
        rl_vlc= rl->rl_vlc[s->qscale];
    }
  {
    for(;;) {
        BITSTREAM_RL_VLC(level, run, &s->bc, rl_vlc, TEX_VLC_BITS, 2);
        if (level==0) {
            int cache;
            cache = bitstream_peek(&s->bc, 32);
            /* escape */
            if (s->msmpeg4_version==1 || (cache&0x80000000)==0) {
                if (s->msmpeg4_version==1 || (cache&0x40000000)==0) {
                    /* third escape */
                    if (s->msmpeg4_version != 1)
                        bitstream_skip(&s->bc, 2);
                    if(s->msmpeg4_version<=3){
                        last = bitstream_read(&s->bc, 1);
                        run = bitstream_read(&s->bc, 6);
                        level = bitstream_read_signed(&s->bc, 8);
                    }else{
                        int sign;
                        last = bitstream_read(&s->bc, 1);
                        if(!s->esc3_level_length){
                            int ll;
                            ff_dlog(s->avctx, "ESC-3 %X at %d %d\n",
                                    bitstream_peek(&s->bc, 24), s->mb_x, s->mb_y);
                            if(s->qscale<8){
                                ll = bitstream_read(&s->bc, 3);
                                if(ll==0){
                                    ll = 8 + bitstream_read_bit(&s->bc);
                                }
                            }else{
                                ll=2;
                                while (ll < 8 && bitstream_peek(&s->bc, 1) == 0) {
                                    ll++;
                                    bitstream_skip(&s->bc, 1);
                                }
                                if (ll < 8)
                                    bitstream_skip(&s->bc, 1);
                            }

                            s->esc3_level_length= ll;
                            s->esc3_run_length = bitstream_read(&s->bc, 2) + 3;
                        }
                        run = bitstream_read(&s->bc, s->esc3_run_length);

                        sign = bitstream_read_bit(&s->bc);

                        level = bitstream_read(&s->bc, s->esc3_level_length);
                        if(sign) level= -level;
                    }

#if 0 // waste of time / this will detect very few errors
                    {
                        const int abs_level= FFABS(level);
                        const int run1= run - rl->max_run[last][abs_level] - run_diff;
                        if(abs_level<=MAX_LEVEL && run<=MAX_RUN){
                            if(abs_level <= rl->max_level[last][run]){
                                av_log(s->avctx, AV_LOG_ERROR, "illegal 3. esc, vlc encoding possible\n");
                                return DECODING_AC_LOST;
                            }
                            if(abs_level <= rl->max_level[last][run]*2){
                                av_log(s->avctx, AV_LOG_ERROR, "illegal 3. esc, esc 1 encoding possible\n");
                                return DECODING_AC_LOST;
                            }
                            if(run1>=0 && abs_level <= rl->max_level[last][run1]){
                                av_log(s->avctx, AV_LOG_ERROR, "illegal 3. esc, esc 2 encoding possible\n");
                                return DECODING_AC_LOST;
                            }
                        }
                    }
#endif
                    //level = level * qmul + (level>0) * qadd - (level<=0) * qadd ;
                    if (level>0) level= level * qmul + qadd;
                    else         level= level * qmul - qadd;
#if 0 // waste of time too :(
                    if(level>2048 || level<-2048){
                        av_log(s->avctx, AV_LOG_ERROR, "|level| overflow in 3. esc\n");
                        return DECODING_AC_LOST;
                    }
#endif
                    i+= run + 1;
                    if(last) i+=192;
#ifdef ERROR_DETAILS
                if(run==66)
                    av_log(s->avctx, AV_LOG_ERROR, "illegal vlc code in ESC3 level=%d\n", level);
                else if((i>62 && i<192) || i>192+63)
                    av_log(s->avctx, AV_LOG_ERROR, "run overflow in ESC3 i=%d run=%d level=%d\n", i, run, level);
#endif
                } else {
                    /* second escape */
                    bitstream_skip(&s->bc, 2);
                    BITSTREAM_RL_VLC(level, run, &s->bc, rl_vlc, TEX_VLC_BITS, 2);
                    i+= run + rl->max_run[run>>7][level/qmul] + run_diff; //FIXME opt indexing
                    level = (level ^ bitstream_peek_signed(&s->bc, 1)) -
                             bitstream_peek_signed(&s->bc, 1);
                    bitstream_skip(&s->bc, 1);
#ifdef ERROR_DETAILS
                if(run==66)
                    av_log(s->avctx, AV_LOG_ERROR, "illegal vlc code in ESC2 level=%d\n", level);
                else if((i>62 && i<192) || i>192+63)
                    av_log(s->avctx, AV_LOG_ERROR, "run overflow in ESC2 i=%d run=%d level=%d\n", i, run, level);
#endif
                }
            } else {
                /* first escape */
                bitstream_skip(&s->bc, 1);
                BITSTREAM_RL_VLC(level, run, &s->bc, rl_vlc, TEX_VLC_BITS, 2);
                i+= run;
                level = level + rl->max_level[run>>7][(run-1)&63] * qmul;//FIXME opt indexing
                level = (level ^ bitstream_peek_signed(&s->bc, 1)) -
                         bitstream_peek_signed(&s->bc, 1);
                bitstream_skip(&s->bc, 1);
#ifdef ERROR_DETAILS
                if(run==66)
                    av_log(s->avctx, AV_LOG_ERROR, "illegal vlc code in ESC1 level=%d\n", level);
                else if((i>62 && i<192) || i>192+63)
                    av_log(s->avctx, AV_LOG_ERROR, "run overflow in ESC1 i=%d run=%d level=%d\n", i, run, level);
#endif
            }
        } else {
            i+= run;
            level = (level ^ bitstream_peek_signed(&s->bc, 1)) -
                     bitstream_peek_signed(&s->bc, 1);
            bitstream_skip(&s->bc, 1);
#ifdef ERROR_DETAILS
                if(run==66)
                    av_log(s->avctx, AV_LOG_ERROR, "illegal vlc code level=%d\n", level);
                else if((i>62 && i<192) || i>192+63)
                    av_log(s->avctx, AV_LOG_ERROR, "run overflow i=%d run=%d level=%d\n", i, run, level);
#endif
        }
        if (i > 62){
            i-= 192;
            if(i&(~63)){
                const int left = bitstream_bits_left(&s->bc);
                if (((i + 192 == 64 && level / qmul == -1) ||
                     !(s->avctx->err_recognition & AV_EF_BITSTREAM)) &&
                    left >= 0) {
                    av_log(s->avctx, AV_LOG_ERROR, "ignoring overflow at %d %d\n", s->mb_x, s->mb_y);
                    break;
                }else{
                    av_log(s->avctx, AV_LOG_ERROR, "ac-tex damaged at %d %d\n", s->mb_x, s->mb_y);
                    return -1;
                }
            }

            block[scan_table[i]] = level;
            break;
        }

        block[scan_table[i]] = level;
    }
  }
 not_coded:
    if (s->mb_intra) {
        ff_mpeg4_pred_ac(s, block, n, dc_pred_dir);
        if (s->ac_pred) {
            i = 63; /* XXX: not optimal */
        }
    }
    if(s->msmpeg4_version>=4 && i>0) i=63; //FIXME/XXX optimize
    s->block_last_index[n] = i;

    return 0;
}

int ff_msmpeg4_decode_motion(MpegEncContext * s,
                                 int *mx_ptr, int *my_ptr)
{
    MVTable *mv;
    int code, mx, my;

    mv = &ff_mv_tables[s->mv_table_index];

    code = bitstream_read_vlc(&s->bc, mv->vlc.table, MV_VLC_BITS, 2);
    if (code < 0){
        av_log(s->avctx, AV_LOG_ERROR, "illegal MV code at %d %d\n", s->mb_x, s->mb_y);
        return -1;
    }
    if (code == mv->n) {
        mx = bitstream_read(&s->bc, 6);
        my = bitstream_read(&s->bc, 6);
    } else {
        mx = mv->table_mvx[code];
        my = mv->table_mvy[code];
    }

    mx += *mx_ptr - 32;
    my += *my_ptr - 32;
    /* WARNING : they do not do exactly modulo encoding */
    if (mx <= -64)
        mx += 64;
    else if (mx >= 64)
        mx -= 64;

    if (my <= -64)
        my += 64;
    else if (my >= 64)
        my -= 64;
    *mx_ptr = mx;
    *my_ptr = my;
    return 0;
}

AVCodec ff_msmpeg4v1_decoder = {
    .name           = "msmpeg4v1",
    .long_name      = NULL_IF_CONFIG_SMALL("MPEG-4 part 2 Microsoft variant version 1"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_MSMPEG4V1,
    .priv_data_size = sizeof(MpegEncContext),
    .init           = ff_msmpeg4_decode_init,
    .close          = ff_h263_decode_end,
    .decode         = ff_h263_decode_frame,
    .capabilities   = AV_CODEC_CAP_DRAW_HORIZ_BAND | AV_CODEC_CAP_DR1,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE
    },
};

AVCodec ff_msmpeg4v2_decoder = {
    .name           = "msmpeg4v2",
    .long_name      = NULL_IF_CONFIG_SMALL("MPEG-4 part 2 Microsoft variant version 2"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_MSMPEG4V2,
    .priv_data_size = sizeof(MpegEncContext),
    .init           = ff_msmpeg4_decode_init,
    .close          = ff_h263_decode_end,
    .decode         = ff_h263_decode_frame,
    .capabilities   = AV_CODEC_CAP_DRAW_HORIZ_BAND | AV_CODEC_CAP_DR1,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE
    },
};

AVCodec ff_msmpeg4v3_decoder = {
    .name           = "msmpeg4",
    .long_name      = NULL_IF_CONFIG_SMALL("MPEG-4 part 2 Microsoft variant version 3"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_MSMPEG4V3,
    .priv_data_size = sizeof(MpegEncContext),
    .init           = ff_msmpeg4_decode_init,
    .close          = ff_h263_decode_end,
    .decode         = ff_h263_decode_frame,
    .capabilities   = AV_CODEC_CAP_DRAW_HORIZ_BAND | AV_CODEC_CAP_DR1,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE
    },
};

AVCodec ff_wmv1_decoder = {
    .name           = "wmv1",
    .long_name      = NULL_IF_CONFIG_SMALL("Windows Media Video 7"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_WMV1,
    .priv_data_size = sizeof(MpegEncContext),
    .init           = ff_msmpeg4_decode_init,
    .close          = ff_h263_decode_end,
    .decode         = ff_h263_decode_frame,
    .capabilities   = AV_CODEC_CAP_DRAW_HORIZ_BAND | AV_CODEC_CAP_DR1,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE
    },
};

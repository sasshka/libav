/*
 * ARM NEON optimised HEVC IDCT
 * Copyright (c) 2017 Alexandra Hájková
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

#include "libavutil/attributes.h"
#include "libavutil/cpu.h"
#include "libavutil/arm/cpu.h"

#include "libavcodec/hevcdsp.h"


void ff_hevc_add_residual_4x4_8_neon(uint8_t *_dst, int16_t *coeffs,
                                     ptrdiff_t stride);
void ff_hevc_add_residual_4x4_10_neon(uint8_t *_dst, int16_t *coeffs,
                                      ptrdiff_t stride);
void ff_hevc_add_residual_8x8_8_neon(uint8_t *_dst, int16_t *coeffs,
                                     ptrdiff_t stride);
void ff_hevc_add_residual_8x8_10_neon(uint8_t *_dst, int16_t *coeffs,
                                      ptrdiff_t stride);
void ff_hevc_add_residual_16x16_8_neon(uint8_t *_dst, int16_t *coeffs,
                                       ptrdiff_t stride);
void ff_hevc_add_residual_16x16_10_neon(uint8_t *_dst, int16_t *coeffs,
                                        ptrdiff_t stride);
void ff_hevc_add_residual_32x32_8_neon(uint8_t *_dst, int16_t *coeffs,
                                       ptrdiff_t stride);
void ff_hevc_add_residual_32x32_10_neon(uint8_t *_dst, int16_t *coeffs,
                                        ptrdiff_t stride);

void ff_hevc_idct_4x4_dc_8_neon(int16_t *coeffs);
void ff_hevc_idct_8x8_dc_8_neon(int16_t *coeffs);
void ff_hevc_idct_16x16_dc_8_neon(int16_t *coeffs);
void ff_hevc_idct_32x32_dc_8_neon(int16_t *coeffs);
void ff_hevc_idct_4x4_dc_10_neon(int16_t *coeffs);
void ff_hevc_idct_8x8_dc_10_neon(int16_t *coeffs);
void ff_hevc_idct_16x16_dc_10_neon(int16_t *coeffs);
void ff_hevc_idct_32x32_dc_10_neon(int16_t *coeffs);

void ff_hevc_idct_4x4_8_neon(int16_t *coeffs, int col_limit);
void ff_hevc_idct_8x8_8_neon(int16_t *coeffs, int col_limit);
void ff_hevc_idct_16x16_8_neon(int16_t *coeffs, int col_limit);
void ff_hevc_idct_32x32_8_neon(int16_t *coeffs, int col_limit);
void ff_hevc_idct_4x4_10_neon(int16_t *coeffs, int col_limit);
void ff_hevc_idct_8x8_10_neon(int16_t *coeffs, int col_limit);
void ff_hevc_idct_16x16_10_neon(int16_t *coeffs, int col_limit);
void ff_hevc_idct_32x32_10_neon(int16_t *coeffs, int col_limit);

void put_hevc_qpel_h2_neon(int16_t *dst, ptrdiff_t dststride,
                                  uint8_t *src, ptrdiff_t srcstride,
                                  int width, int height,           
                                  int16_t* mcbuffer);               

#define QPEL_FILTER_1(src, stride)      \
    (1 * -src[x - 3 * stride] +         \
     4 *  src[x - 2 * stride] -         \
    10 *  src[x -     stride] +         \
    58 *  src[x]              +         \
    17 *  src[x +     stride] -         \
     5 *  src[x + 2 * stride] +         \
     1 *  src[x + 3 * stride])

#define QPEL_FILTER_3(src, stride)      \
    (1  * src[x - 2 * stride] -         \
     5  * src[x -     stride] +         \
    17  * src[x]              +         \
    58  * src[x + stride]     -         \
    10  * src[x + 2 * stride] +         \
     4  * src[x + 3 * stride] -         \
     1  * src[x + 4 * stride])

static void ff_put_hevc_qpel_h_4_8_neon(int16_t *dst,  ptrdiff_t dststride,                             
                                         uint8_t *src, ptrdiff_t srcstride,                              
                                         int height, int mx, int my, int16_t* mcbuffer)            
{                                                                                     
    if (mx == 1) {                                                                     
    int x, y;                                                     
                                                                  
    dststride /= sizeof(*dst);                                    
    for (y = 0; y < height; y++) {                                
        for (x = 0; x < 4; x++)                               
            dst[x] = QPEL_FILTER_1(src, 1) >> (8 - 8);
        src += srcstride;                                         
        dst += dststride;                                         
    }                                                             
    } else if (mx == 2)                                                      
        put_hevc_qpel_h2_neon(dst, dststride, src, srcstride, 4, height, mcbuffer);  
    else {
    int x, y;                                                      
                                                                   
    dststride /= sizeof(*dst);                                     
    for (y = 0; y < height; y++) {                                 
        for (x = 0; x < 4; x++)                                
            dst[x] = QPEL_FILTER_3(src, 1) >> (8 - 8); 
        src += srcstride;                                          
        dst += dststride;                                          
    }                                                              
    }
}

av_cold void ff_hevc_dsp_init_arm(HEVCDSPContext *c, int bit_depth)
{
    int cpu_flags = av_get_cpu_flags();

    if (have_neon(cpu_flags)) {
        if (bit_depth == 8) {
            c->add_residual[0] = ff_hevc_add_residual_4x4_8_neon;
            c->add_residual[1] = ff_hevc_add_residual_8x8_8_neon;
            c->add_residual[2] = ff_hevc_add_residual_16x16_8_neon;
            c->add_residual[3] = ff_hevc_add_residual_32x32_8_neon;

            c->idct_dc[0] = ff_hevc_idct_4x4_dc_8_neon;
            c->idct_dc[1] = ff_hevc_idct_8x8_dc_8_neon;
            c->idct_dc[2] = ff_hevc_idct_16x16_dc_8_neon;
            c->idct_dc[3] = ff_hevc_idct_32x32_dc_8_neon;

            c->idct[0] = ff_hevc_idct_4x4_8_neon;
            c->idct[1] = ff_hevc_idct_8x8_8_neon;
            c->idct[2] = ff_hevc_idct_16x16_8_neon;
            c->idct[3] = ff_hevc_idct_32x32_8_neon;

            c->put_hevc_qpel[0][1][0] = ff_put_hevc_qpel_h_4_8_neon;
        }
        if (bit_depth == 10) {
            c->add_residual[0] = ff_hevc_add_residual_4x4_10_neon;
            c->add_residual[1] = ff_hevc_add_residual_8x8_10_neon;
            c->add_residual[2] = ff_hevc_add_residual_16x16_10_neon;
            c->add_residual[3] = ff_hevc_add_residual_32x32_10_neon;

            c->idct_dc[0] = ff_hevc_idct_4x4_dc_10_neon;
            c->idct_dc[1] = ff_hevc_idct_8x8_dc_10_neon;
            c->idct_dc[2] = ff_hevc_idct_16x16_dc_10_neon;
            c->idct_dc[3] = ff_hevc_idct_32x32_dc_10_neon;

            c->idct[0] = ff_hevc_idct_4x4_10_neon;
            c->idct[1] = ff_hevc_idct_8x8_10_neon;
            c->idct[2] = ff_hevc_idct_16x16_10_neon;
            c->idct[3] = ff_hevc_idct_32x32_10_neon;
        }
    }
}

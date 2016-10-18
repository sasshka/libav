/*
 * ARM NEON optimised IDCT functions for HEVC decoding
 * Copyright (c)
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

#include "libavutil/arm/asm.S"

.macro transform out ss1, ss2, ss3, in0, in1, in2, in3, shift, scale, c0, c1, c2, c3
        vmull.s16   q3, \in0, \c0
        \ss1        q3, \in1, \c1
        \ss2        q3, \in2, \c2
        \ss3        q3, \in3, \c3
        .if \scale == 1
        vqrshrn.s32 \out, q3, \shift
        .else
        vmov.i32 \out, q3
        .endif
.endm

.macro tr_4x4 in0, in1, in2, in3, out0, out1, out2, out3, shift, scale
        movrel r1, trans_4x4
        vld1.s16 {d4}, [r1]

        transform \out0, vmlal.s16, vmlal.s16, vmlal.s16, \in0, \in1, \in2, \in3, \shift, \scale, d4[0], d4[1], d4[2], d4[3]
        transform \out3, vmlsl.s16, vmlal.s16, vmlsl.s16, \in0, \in1, \in2, \in3, \shift, \scale, d4[0], d4[1], d4[2], d4[3]

        transform \out1, vmlal.s16, vmlsl.s16, vmlsl.s16, \in0, \in1, \in2, \in3, \shift, \scale, d4[0], d4[3], d4[2], d4[1]
        transform \out2, vmlsl.s16, vmlsl.s16, vmlal.s16, \in0, \in1, \in2, \in3, \shift, \scale, d4[0], d4[3], d4[2], d4[1]
.endm

.macro transpose_4x4 out0, out1, out2, out3, out4, out5
        vtrn.16 \out0, \out1
        vtrn.16 \out2, \out3
        vtrn.32 \out4, \out5
.endm

.macro idct_4x4 bitdepth
function ff_hevc_idct_4x4_\bitdepth\()_neon, export=1
@r0 - coeffs
        vld1.s16 {q0, q1}, [r0, :128]

        tr_4x4 d0, d1, d2, d3, d16, d17, d18, d19 #7, 1
        transpose_4x4 d16, d17, d18, d19, q8, q9

        tr_4x4 d16, d17, d18, d19, d0, d1, d2, d3, #20 - \bitdepth, 1
        transpose_4x4 d0, d1, d2, d3, q0, q1

        vst1.s16 {d0-d3}, [r0, :128]
        bx lr
endfunc
.endm

.macro tr_8x4 in0, in1, in2, in3, out0, out1, out2, out3, shift, scale
        vmov.i16 d28, #89
        vmov.i16 d29, #75
        vmov.i16 d30, #50
        vmov.i16 d31, #18

        transform \out0, vmlal.s16, vmlal.s16, vmlal.s16, \in0, \in1, \in2, \in3, \shift, \scale, d28, d29, d30, d31
        transform \out3, vmlsl.s16, vmlsl.s16, vmlsl.s16, \in0, \in1, \in2, \in3, \shift, \scale, d29, d31, d28, d30
        transform \out1, vmlsl.s16, vmlal.s16, vmlal.s16, \in0, \in1, \in2, \in3, \shift, \scale, d30, d28, d31, d29
        transform \out2, vmlsl.s16, vmlal.s16, vmlsl.s16, \in0, \in1, \in2, \in3, \shift, \scale, d31, d30, d29, d28
.endm

.macro scale_store e, o, shift, off_1, off
        vadd.s32 q14, \e, \o
        vsub.s32 \e, \e, \o
        vqrshrn.s32 d0, q14, \shift
        vqrshrn.s32 d1, \e, \shift

        add r1, r0, \off_1
        vst1.s16 {d0}, [r1]
        add r1, r0, \off
        vst1.s16 {d1}, [r1]
.endm

.macro load in0, in1, in2, in3, off0, off1, off2, off3
        add r1, r0, \off0
        vld1.s16 {d0}, [r1]
        add r1, r0, \off1
        vld1.s16 {d1}, [r1]
        add r1, r0, \off2
        vld1.s16 {d2}, [r1]
        add r1, r0, \off3
        vld1.s16 {d3}, [r1]
.endm

.macro idct_8x8 bitdepth
function ff_hevc_idct_8x8_\bitdepth\()_neon, export=1
@r0 - coeffs
        load d0, d1, d2, d3, #0, #2*16, #4*16, #6*16

        tr_4x4 d0, d1, d2, d3, q2, q3, q8, q9, 0, 0

        load d0, d1, d2, d3, #16, #3*16, #5*16, #7*16

        tr_8x4 d0, d1, d2, d3, q10, q11, q12, q13, 0, 0
        scale_store q2, q10, #7, #0,      #7 * 16
        scale_store q3, q11, #7, #16,     #6 * 16
        scale_store q8, q12, #7, #2 * 16, #5 * 16
        scale_store q9, q13, #7, #3 * 16, #4 * 16


        bx lr
endfunc
.endm

idct_4x4 8
idct_4x4 10
idct_8x8 8
idct_8x8 10

const trans_4x4
        .short 64, 83, 64, 36
endconst

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

.macro transform tr_coeffs, out
        vmull.s16   q4,  d4, \tr_coeffs[0]
        vmlal.s16   q4,  d5, \tr_coeffs[0]
        vmlal.s16   q4,  d6, \tr_coeffs[0]
        vmlal.s16   q4,  d7, \tr_coeffs[0]
        vqrshrn.s32 \out, q4, #7
.endm

.macro tr_4x4
        transform d0, d10
        transform d1, d11
        transform d2, d12
        transform d3, d13
.endm

.macro transpose_4x4
        vtrn.16 d11, d12
        vtrn.16 d13, d14
        vtrn.32 q5,  q6
.endm

.macro idct_4x4 bitdepth
function ff_hevc_idct_4x4_\bitdepth\()_neon, export=1
@r0 - coeffs
        movrel r1, transform_4x4
        vld1.16 {d0}, [r1]
        vld1.16 {d4, d5, d6, d7}, [r0, :128]

        tr_4x4
        transpose_4x4

        tr_4x4
        transpose_4x4

        vst1.s16 {d10, d11, d12, d13}, [r0, :128]
endfunc
.endm

idct_4x4 8
idct_4x4 10

const transform_4x4
      .short 64,   83,  64,  36
      .short 64,   36, -64, -83
      .short 64,  -36, -64,  83
      .short 64,  -83,  64, -36
endconst

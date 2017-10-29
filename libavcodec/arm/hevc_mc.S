/*
 * ARM NEON optimised MC functions for HEVC decoding
 *
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

#include "libavutil/arm/asm.S"

.macro get_pixels4 bitdepth
function ff_hevc_get_pixels_4_\bitdepth\()_neon, export=1
@r0 dst, r1 dststride, r2 src, r3 srcstride
        ldr             r12, [sp] @height
        cmp             r12, #0
        bxeq            lr

1: .if \bitdepth == 8
        vld1.32         {d0[0]}, [r2], r3
        vld1.32         {d1[0]}, [r2], r3
        vld1.32         {d2[0]}, [r2], r3
        vld1.32         {d3[0]}, [r2], r3
        vshll.u8        q8, d0, #6
        vshll.u8        q9, d1, #6
        vshll.u8        q10, d2, #6
        vshll.u8        q11, d3, #6
  .else
        vld1.16         {d0}, [r2], r3
        vld1.16         {d1}, [r2], r3
        vld1.16         {d2}, [r2], r3
        vld1.16         {d3}, [r2], r3
        vshl.i16        d16, d0, #4
        vshl.i16        d18, d1, #4
        vshl.i16        d20, d2, #4
        vshl.i16        d22, d3, #4
  .endif

        vst1.16         {d16}, [r0, :64], r1
        vst1.16         {d18}, [r0, :64], r1
        vst1.16         {d20}, [r0, :64], r1
        vst1.16         {d22}, [r0, :64], r1
        subs            r12, #4
        bgt             1b

        bx              lr
endfunc
.endm

.macro get_pixels8 bitdepth
function ff_hevc_get_pixels_8_\bitdepth\()_neon, export=1
@r0 dst, r1 dststride, r2 src, r3 srcstride
        ldr             r12, [sp] @height
        cmp             r12, #0
        bxeq            lr

1: .if \bitdepth == 8
        vld1.8          {d0}, [r2], r3
        vld1.8          {d1}, [r2], r3
        vld1.8          {d2}, [r2], r3
        vld1.8          {d3}, [r2], r3
        vshll.u8        q8, d0, #6
        vshll.u8        q9, d1, #6
        vshll.u8        q10, d2, #6
        vshll.u8        q11, d3, #6
  .else
        vld1.16         {d16-d17}, [r2], r3
        vld1.16         {d18-d19}, [r2], r3
        vld1.16         {d20-d21}, [r2], r3
        vld1.16         {d22-d23}, [r2], r3
        vshl.i16        q8, q8, #4
        vshl.i16        q9, q9, #4
        vshl.i16        q10, q10, #4
        vshl.i16        q11, q11, #4
  .endif

        vst1.16         {d16-d17}, [r0, :64], r1
        vst1.16         {d18-d19}, [r0, :64], r1
        vst1.16         {d20-d21}, [r0, :64], r1
        vst1.16         {d22-d23}, [r0, :64], r1
        subs            r12, #4
        bgt             1b

        bx              lr
endfunc
.endm

.macro get_pixels12 bitdepth
function ff_hevc_get_pixels_12_\bitdepth\()_neon, export=1
@r0 - dst, r1 - dststride, r2 - src, r3 - srcstride
        ldr             r12, [sp] @height
        cmp             r12, #0
        bxeq            lr
        push            {r4-r5, lr}
        add             r4, r0, #16

1: .if \bitdepth == 8
        add             r5, r2, #8
        vld1.8          {d0}, [r2], r3
        vld1.32         {d4[0]}, [r5], r3
        vld1.8          {d1}, [r2], r3
        vld1.32         {d5[0]}, [r5], r3
        vld1.8          {d2}, [r2], r3
        vld1.32         {d6[0]}, [r5], r3
        vld1.8          {d3}, [r2], r3
        vld1.32         {d7[0]}, [r5], r3
        vshll.u8        q8, d0, #6
        vshll.u8        q12, d4, #6
        vshll.u8        q9, d1, #6
        vshll.u8        q13, d5, #6
        vshll.u8        q10, d2, #6
        vshll.u8        q14, d6, #6
        vshll.u8        q11, d3, #6
        vshll.u8        q15, d7, #6
  .else
        add             r5, r2, #16
        vld1.16         {d16-d17}, [r2], r3
        vld1.16         {d24}, [r5], r3
        vld1.16         {d18-d19}, [r2], r3
        vld1.16         {d26}, [r5], r3
        vld1.16         {d20-d21}, [r2], r3
        vld1.16         {d28}, [r5], r3
        vld1.16         {d22-d23}, [r2], r3
        vld1.16         {d30}, [r5], r3
        vshl.i16        q8, q8, #4
        vshl.i16        d24, d24, #4
        vshl.i16        q9, q9, #4
        vshl.i16        d26, d26, #4
        vshl.i16        q10, q10, #4
        vshl.i16        d28, d28, #4
        vshl.i16        q11, q11, #4
        vshl.i16        d30, d30, #4
  .endif

        vst1.16         {d16-d17}, [r0, :64], r1
        vst1.16         {d24}, [r4, :64], r1
        vst1.16         {d18-d19}, [r0, :64], r1
        vst1.16         {d26}, [r4, :64], r1
        vst1.16         {d20-d21}, [r0, :64], r1
        vst1.16         {d28}, [r4, :64], r1
        vst1.16         {d22-d23}, [r0, :64], r1
        vst1.16         {d30}, [r4, :64], r1
        subs            r12, #4
        bgt             1b

        pop             {r4-r5, pc}
endfunc
.endm

.macro get_pixels16 bitdepth
function ff_hevc_get_pixels_16_\bitdepth\()_neon, export=1
@r0 dst, r1 dststride, r2 src, r3 srcstride
        ldr             r12, [sp] @height
        cmp             r12, #0
        bxeq            lr

1: .if \bitdepth == 8
        vld1.8          {d0-d1}, [r2], r3
        vld1.8          {d2-d3}, [r2], r3
        vld1.8          {d4-d5}, [r2], r3
        vld1.8          {d6-d7}, [r2], r3
        vshll.u8        q8, d0, #6
        vshll.u8        q9, d1, #6
        vshll.u8        q10, d2, #6
        vshll.u8        q11, d3, #6
        vshll.u8        q12, d4, #6
        vshll.u8        q13, d5, #6
        vshll.u8        q14, d6, #6
        vshll.u8        q15, d7, #6
  .else
        vld1.16         {d16-d19}, [r2], r3
        vld1.16         {d20-d23}, [r2], r3
        vld1.16         {d24-d27}, [r2], r3
        vld1.16         {d28-d31}, [r2], r3
        vshl.i16        q8, q8, #4
        vshl.i16        q9, q9, #4
        vshl.i16        q10, q10, #4
        vshl.i16        q11, q11, #4
        vshl.i16        q12, q12, #4
        vshl.i16        q13, q13, #4
        vshl.i16        q14, q14, #4
        vshl.i16        q15, q15, #4
  .endif

        vst1.16         {d16-d19}, [r0, :128], r1
        vst1.16         {d20-d23}, [r0, :128], r1
        vst1.16         {d24-d27}, [r0, :128], r1
        vst1.16         {d28-d31}, [r0, :128], r1
        subs            r12, #4
        bgt             1b

        bx              lr
endfunc
.endm

.macro get_pixels24 bitdepth
function ff_hevc_get_pixels_24_\bitdepth\()_neon, export=1
@r0 dst, r1 dststride, r2 src, r3 srcstride
        ldr             r12, [sp] @height
        cmp             r12, #0
        bxeq            lr

        push            {r0-r4, lr}
        push            {r12}
        bl              ff_hevc_get_pixels_8_\bitdepth\()_neon
        pop             {r12}
        pop             {r0-r4, lr}

  .if \bitdepth == 8
        add             r2, #8
  .else
        add             r2, #16
  .endif
        add             r0, #16
        b               ff_hevc_get_pixels_16_\bitdepth\()_neon
endfunc
.endm

.macro get_pixels32 bitdepth
function ff_hevc_get_pixels_32_\bitdepth\()_neon, export=1
@r0 dst, r1 dststride, r2 src, r3 srcstride
        ldr             r12, [sp] @height
        cmp             r12, #0
        bxeq            lr

        push            {r0-r4, lr}
        push            {r12}
        bl              ff_hevc_get_pixels_16_\bitdepth\()_neon
        pop             {r12}
        pop             {r0-r4, lr}

  .if \bitdepth == 8
        add             r2, #16
  .else
        add             r2, #32
  .endif
        add             r0, #32
        b               ff_hevc_get_pixels_16_\bitdepth\()_neon
endfunc
.endm

.macro get_pixels48 bitdepth
function ff_hevc_get_pixels_48_\bitdepth\()_neon, export=1
@r0 dst, r1 dststride, r2 src, r3 srcstride
        ldr             r12, [sp] @height
        cmp             r12, #0
        bxeq            lr

        push            {r0-r4, lr}
        push            {r12}
        bl              ff_hevc_get_pixels_16_\bitdepth\()_neon
        pop             {r12}
        pop             {r0-r4, lr}

  .if \bitdepth == 8
        add             r2, #16
  .else
        add             r2, #32
  .endif
        add             r0, #32
        b               ff_hevc_get_pixels_32_\bitdepth\()_neon
endfunc
.endm

.macro get_pixels64 bitdepth
function ff_hevc_get_pixels_64_\bitdepth\()_neon, export=1
@r0 dst, r1 dststride, r2 src, r3 srcstride
        ldr             r12, [sp] @height
        cmp             r12, #0
        bxeq            lr

        push            {r0-r4, lr}
        push            {r12}
        bl              ff_hevc_get_pixels_32_\bitdepth\()_neon
        pop             {r12}
        pop             {r0-r4, lr}

  .if \bitdepth == 8
        add             r2, #32
  .else
        add             r2, #64
  .endif
        add             r0, #64
        b               ff_hevc_get_pixels_32_\bitdepth\()_neon
endfunc
.endm

get_pixels4 8
get_pixels4 10

get_pixels8 8
get_pixels8 10

get_pixels12 8
get_pixels12 10

get_pixels16 8
get_pixels16 10

get_pixels24 8
get_pixels24 10

get_pixels32 8
get_pixels32 10

get_pixels48 8
get_pixels48 10

get_pixels64 8
get_pixels64 10

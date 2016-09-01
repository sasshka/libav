;*******************************************************************************
;* SIMD-optimized IDCT functions for HEVC decoding
;* Copyright (c) 2014 Pierre-Edouard LEPERE
;* Copyright (c) 2014 James Almer
;*
;* This file is part of Libav.
;*
;* Libav is free software; you can redistribute it and/or
;* modify it under the terms of the GNU Lesser General Public
;* License as published by the Free Software Foundation; either
;* version 2.1 of the License, or (at your option) any later version.
;*
;* Libav is distributed in the hope that it will be useful,
;* but WITHOUT ANY WARRANTY; without even the implied warranty of
;* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
;* Lesser General Public License for more details.
;*
;* You should have received a copy of the GNU Lesser General Public
;* License along with Libav; if not, write to the Free Software
;* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
;******************************************************************************

%include "libavutil/x86/x86util.asm"

SECTION_RODATA

pd_64: times 4 dd 64
pd_83: times 4 dd 83
pd_36: times 4 dd 36
pd_2048: times 4 dd 2048
pd_512: times 4 dd 512
pd_89: times 4 dd 89
pd_75: times 4 dd 75
pd_50: times 4 dd 50
pd_18: times 4 dd 18
; 16x16 transformation coeffs
pd_90: times 4 dd 90
pd_87: times 4 dd 87
pd_80: times 4 dd 80
pd_70: times 4 dd 70
pd_57: times 4 dd 57
pd_43: times 4 dd 43
pd_25: times 4 dd 25
pd_9: times 4 dd 9

section .text

; void ff_hevc_idctHxW_dc_{8,10}_<opt>(int16_t *coeffs)
; %1 = HxW
; %2 = number of loops
; %3 = bitdepth
%macro IDCT_DC 3
cglobal hevc_idct_%1x%1_dc_%3, 1, 2, 1, coeff, tmp
    movsx             tmpd, word [coeffq]
    add               tmpd, (1 << (14 - %3)) + 1
    sar               tmpd, (15 - %3)
    movd               xm0, tmpd
    SPLATW              m0, xm0
    DEFINE_ARGS coeff, cnt
    mov               cntd, %2
.loop:
    mova [coeffq+mmsize*0], m0
    mova [coeffq+mmsize*1], m0
    mova [coeffq+mmsize*2], m0
    mova [coeffq+mmsize*3], m0
    add  coeffq, mmsize*8
    mova [coeffq+mmsize*-4], m0
    mova [coeffq+mmsize*-3], m0
    mova [coeffq+mmsize*-2], m0
    mova [coeffq+mmsize*-1], m0
    dec  cntd
    jg  .loop
    RET
%endmacro

; %1 = HxW
; %2 = bitdepth
%macro IDCT_DC_NL 2 ; No loop
cglobal hevc_idct_%1x%1_dc_%2, 1, 2, 1, coeff, tmp
    movsx             tmpd, word [coeffq]
    add               tmpd, (1 << (14 - %2)) + 1
    sar               tmpd, (15 - %2)
    movd                m0, tmpd
    SPLATW              m0, xm0
    mova [coeffq+mmsize*0], m0
    mova [coeffq+mmsize*1], m0
    mova [coeffq+mmsize*2], m0
    mova [coeffq+mmsize*3], m0
%if mmsize == 16
    mova [coeffq+mmsize*4], m0
    mova [coeffq+mmsize*5], m0
    mova [coeffq+mmsize*6], m0
    mova [coeffq+mmsize*7], m0
%endif
    RET
%endmacro

; pack dwords in %1 and %2 xmm registers
; to words to clip them to 16 bit, then
; unpack them back
%macro CLIP16 4
    packssdw   %3, %1, %2
    packssdw   %4, %2, %1
    pmovsxwd   %1, %3
    pmovsxwd   %2, %4
%endmacro

; add constant %2 to %1
; then shift %1 with %3
%macro ADD_SHIFT 3
    paddd %1, %2
    psrad %1, %3
%endmacro

%macro SCALE 2
    ADD_SHIFT m1, %1, %2
    ADD_SHIFT m2, %1, %2
    ADD_SHIFT m3, %1, %2
    ADD_SHIFT m4, %1, %2
%endmacro

; take m1, m2, m3, m4,
; do the 4x4 vertical IDCT
; without SCALE, store output
; back in m1, m2, m3, m4
%macro TR_4x4 0
    mova      m11, [pd_83]
    mova      m12, [pd_36]

    pslld     m1,  6            ; m1 = 64*src0
    pslld     m2,  6            ; m2 = 64*src2

    pmulld    m4,  m3, m11      ; m4 = 83*src1
    pmulld    m3,  m12          ; m3 = 36*src1

    pmulld    m6,  m5, m11      ; m6 = 83*src3
    pmulld    m5,  m12          ; m5 = 36*src3

    paddd    m7,  m1, m2        ; e0
    psubd    m8,  m1, m2        ; e1

    paddd    m9,  m4, m5        ; o0
    psubd    m10, m3, m6        ; o1

    paddd    m1, m7, m9         ; e0 + o0
    paddd    m2, m8, m10        ; e1 + o1
    psubd    m3, m8, m10        ; e1 - o1
    psubd    m4, m7, m9         ; e0 - o0
%endmacro

;    m1,  m2, m3, m4 is transposed
; to m10, m6, m7, m8
%macro TRANSPOSE_4x4 0
    punpckldq m10, m1, m2
    punpckldq m6, m3, m4
    movlhps   m10, m6

    punpckldq m7, m1, m2
    punpckldq m6, m3, m4
    movhlps   m6, m7

    punpckhdq m7, m1, m2
    punpckhdq m8, m3, m4
    movlhps   m7, m8

    punpckhdq m9, m1, m2
    punpckhdq m8, m3, m4
    movhlps   m8, m9
%endmacro

%macro C_ADD_16 1
    %assign shift (20 - %1)
    %assign c_add (1 << (shift - 1))
    %define arr_add pd_ %+ c_add
%endmacro

; %1 - bit_depth
; %2 - register add constant
; is loaded to
; shift = 20 - bit_depth
%macro C_ADD 2
    C_ADD_16 %1
    mova %2, [arr_add]
%endmacro

; load coeffs to %2, %3, %4, %5
; %1 - horizontal offset
; %6, %7, %8, %9 - vertical offsets
%macro LOAD_BLOCK 9
    pmovsxwd %2, [coeffsq + %9 + %1]
    pmovsxwd %3, [coeffsq + %6 + %1]
    pmovsxwd %4, [coeffsq + %7 + %1]
    pmovsxwd %5, [coeffsq + %8 + %1]
%endmacro

; void ff_hevc_idct_4x4__{8,10}_<opt>(int16_t *coeffs, int col_limit)
; %1 = bitdepth
%macro IDCT_4x4 1
cglobal hevc_idct_4x4_ %+ %1, 1, 14, 14, coeffs
    mova     m13, [pd_64]
    LOAD_BLOCK 0, m1, m2, m3, m5, 16, 8, 24, 0

    TR_4x4
    SCALE m13, 7
    CLIP16 m1, m2, m5, m6
    CLIP16 m3, m4, m5, m6
    TRANSPOSE_4x4

    SWAP m1, m10
    SWAP m2, m7
    SWAP m3, m6
    SWAP m5, m8

    TR_4x4
    C_ADD %1, m13
    SCALE m13, shift
    TRANSPOSE_4x4

    packssdw m10, m6
    movdqa   [coeffsq], m10
    packssdw m7, m8
    movdqa   [coeffsq + 16], m7

    RET
%endmacro

; multiply coeffs in    %5, %6,  %7,  %8
; with transform coeffs %1, %2,  %3,  %4
; store the results in  %9, %10, %11, %12
%macro O8 12
    pmulld %9, %5, %1
    pmulld %10, %6, %2
    pmulld %11, %7, %3
    pmulld %12, %8, %4
%endmacro

; store intermedite e16 coeffs on stack
; as 8x4 matrix - writes 128 bytes to stack
; from m10: e8 + o8, with %1 offset
; and  %3:  e8 - o8, with %2 offset
; %4 - shift, unused here
%macro STORE_16 6
    movu    [rsp + %1], m10
    movu    [rsp + %2], %3
%endmacro

; scale, pack (clip16) and store the residuals     0 e8[0] + o8[0] --> + %1
; 4 at one time (4 columns)                        1 e8[1] + o8[1]
; from %5: e8/16 + o8/16, with %1 offset                  ...
; and  %3: e8/16 - o8/16, with %2 offset           6 e8[1] - o8[1]
; %4 - shift                                       7 e8[0] - o8[0] --> + %2
; %6 - add
%macro STORE_8 6
    ADD_SHIFT %5, %6, %4
    ADD_SHIFT %3, %6, %4
    packssdw  %5, %3
    movq      [coeffsq + %1], %5
    movhps    [coeffsq + %2], %5
%endmacro

; %1 - horizontal offset
; %2 - shift
; %6 - vertical offset for e8 + o8
; %7 - vertical offset for e8 - o8
; %8 - register with o8 inside
; %9 - block_size
%macro E8_O8 9
    %3 m9, m10
    %4 m9, m11
    %5 m9, m12        ; o8[i + %1] for 4 rows
    paddd m10, m9, %8 ; o8 + e8
    psubd %8, m9      ; e8 - o8
    STORE_%9 %6 + %1, %7 + %1, %8, %2, m10, m13
%endmacro

; 8x4 residuals are processed and stored
; %1 - horizontal offset
; %2 - shift
; %3 - offset of the even row
; %4 - step: 1 for 8x8, 2 for 16x16, 4 for 32x32
; %5 - offset of the odd row
; %6 - block size
%macro TR_8x4 6
    ; load 4 columns of even rows
    LOAD_BLOCK %1, m1, m2, m3, m5, 2 * %4 * %3, %4 * %3, 3 * %4 * %3, 0

    TR_4x4 ; e8: m1, m2, m3, m4, for 4 columns only

    ; load 4 columns of odd rows
    LOAD_BLOCK %1, m5, m6, m7, m8, 3 * %4 * %5, 5 * %4 * %5, 7 * %4 * %5, %4 * %5

    mova m14, [pd_89]
    mova m15, [pd_75]
    mova m0,  [pd_50]

    O8 m14, m15, m0, [pd_18], m5, m6, m7, m8, m9, m10, m11, m12
    E8_O8 %1, %2, paddd, paddd, paddd, 0, %5 * 7, m1, %6

    mova m1, [pd_18]
    O8 m15, m1, m14, m0, m5, m6, m7, m8, m9, m10, m11, m12
    E8_O8 %1, %2, psubd, psubd, psubd, %5, %5 * 6, m2, %6

    O8 m0, m14, m1, m15, m5, m6, m7, m8, m9, m10, m11, m12
    E8_O8 %1, %2, psubd, paddd, paddd, %5 * 2, %5 * 5, m3, %6

    O8 m1, m0, m15, m14, m5, m6, m7, m8, m9, m10, m11, m12
    E8_O8 %1, %2, psubd, paddd, psubd, %5 * 3, %5 * 4, m4, %6
%endmacro

%macro STORE_BLOCK 9
    packssdw %2, %3
    movq     [coeffsq + %9 + %1], %2
    movhps   [coeffsq + %6 + %1], %2
    packssdw %4, %5
    movq     [coeffsq + %7 + %1], %4
    movhps   [coeffsq + %8 + %1], %4
%endmacro

; load block i and store it in m5, m11, m12, m13
; load block j to m1, m2, m3, m4, transpose it
; store block j
; swap register names with the block i and transpose the block
; store block i

; %1 - horizontal offset of the block i
; %2 - vertical offset of the block i
; %3 - width in bytes
; %4 - vertical offset for the block j
; %5 - horizontal offset for the block j
%macro SWAP_BLOCKS 5
    ; M_i
    LOAD_BLOCK %1, m5, m11, m12, m13, %2 + %3, %2 + 2 * %3, %2 + 3 * %3, %2

    ; M_j
    LOAD_BLOCK %5, m1, m2, m3, m4, %4 + %3, %4 + 2 * %3, %4 + 3 * %3, %4
    TRANSPOSE_4x4 ; m10, m6, m7, m8
    STORE_BLOCK %1, m10, m6, m7, m8, %2 + %3, %2 + 2 * %3, %2 + 3 * %3, %2

    ; transpose and store M_i
    SWAP m5, m1
    SWAP m11, m2
    SWAP m12, m3
    SWAP m13, m4
    TRANSPOSE_4x4
    STORE_BLOCK %5, m10, m6, m7, m8, %4 + %3, %4 + 2 * %3, %4 + 3 * %3, %4
%endmacro

; %1 - horizontal offset
; %2 - 2 - vertical offset of the block
; %3 - width in bytes
%macro TRANSPOSE_BLOCK 3
    LOAD_BLOCK %1, m1, m2, m3, m4, %2 + %3, %2 + 2 * %3, %2 + 3 * %3, %2
    TRANSPOSE_4x4 ; m10, m6, m7, m8
    STORE_BLOCK %1, m10, m6, m7, m8, %2 + %3, %2 + 2 * %3, %2 + 3 * %3, %2
%endmacro

%macro TRANSPOSE_8x8 0
    ; M1 M2 ^T = M1^t M3^t
    ; M3 M4      M2^t M4^t

    ; M1 4x4 block
    TRANSPOSE_BLOCK 0, 0, 16

    ; M2 and M3
    SWAP_BLOCKS 0, 64, 16, 0, 8

    ; M4
    TRANSPOSE_BLOCK 8, 64, 16
%endmacro

; void ff_hevc_idct_8x8_{8,10}_<opt>(int16_t *coeffs, int col_limit)
; %1 = bitdepth
%macro IDCT_8x8 1
cglobal hevc_idct_8x8_ %+ %1, 1, 14, 14, coeffs
    mova     m13, [pd_64]

    TR_8x4 0, 7, 32, 1, 16, 8
    TR_8x4 8, 7, 32, 1, 16, 8

    TRANSPOSE_8x8

    C_ADD %1, m13
    TR_8x4 0, shift, 32, 1, 16, 8
    TR_8x4 8, shift, 32, 1, 16, 8

    TRANSPOSE_8x8

    RET
%endmacro

; %8 -  e16 + o16 offset
; %9 - e16 - o16 offset
; %10 - shift
; %11 - add
%macro E16_O16 12
    %1 m1, m2
    %2 m1, m3
    %3 m1, m4
    %4 m1, m5
    %5 m1, m6
    %6 m1, m7
    %7 m1, m8

    movu m2, [rsp + %8]
    psubd m3, m2, m1 ; e16 - o16
    paddd m1, m2     ; o16 + e16
    STORE_%12 %8, %9, m3, %10, m1, %11
%endmacro

; %6 - width in bytes
; %7 - STORE 8/16
; %9 - step: 1 for 16x16, 2 for 32x32
%macro TR_16x4 8
    mova     m13, [pd_64]

    ; produce 8x4 matrix of e16 coeffs
    ; for 4 first rows and store it on stack (128 bytes)
    TR_8x4 %1, 7, %4, %5, %6, %8

    ; load 8 even rows
    LOAD_BLOCK %1, m9, m10, m11, m12, %9 * 3 * %6, %9 * 5 * %6, %9 * 7 * %6, %9 * %6
    LOAD_BLOCK %1, m13, m14, m15, m0, %9 * 11 * %6, %9 * 13 * %6, %9 * 15 * %6, %9 * 9 * %6

    ; multiply src coeffs with the transform
    ; coeffs and store the intermediate results on m1, ... , m8
    ; calculate the residuals from the intermediate results and store
    ; them back to [coeffsq]

    ; o16[0]
    O8 [pd_90], [pd_87], [pd_80], [pd_70], m9,  m10, m11, m12, m1, m2, m3, m4
    O8 [pd_57], [pd_43], [pd_25], [pd_9],  m13, m14, m15, m0,  m5, m6, m7, m8
    E16_O16 paddd, paddd, paddd, paddd, paddd, paddd, paddd, 0 + %1, 15 * %6 + %1, %2, %3, %7

    ; o16[1]
    O8 [pd_87], [pd_57], [pd_9],  [pd_43],  m9,  m10, m11, m12, m1, m2, m3, m4
    O8 [pd_80], [pd_90], [pd_70], [pd_25],  m13, m14, m15, m0,  m5, m6, m7, m8
    E16_O16 paddd, paddd, psubd, psubd, psubd, psubd, psubd, %6 + %1, 14 * %6 + %1, %2, %3, %7

    ; o16[2]
    O8 [pd_80], [pd_9],  [pd_70], [pd_87], m9,  m10, m11, m12, m1, m2, m3, m4
    O8 [pd_25], [pd_57], [pd_90], [pd_43], m13, m14, m15, m0,  m5, m6, m7, m8
    E16_O16 paddd, psubd, psubd, psubd, paddd, paddd, paddd, 2 * %6 + %1, 13 * %6 + %1, %2, %3, %7

    ; o16[3]
    O8 [pd_70], [pd_43], [pd_87], [pd_9],  m9,  m10, m11, m12, m1, m2, m3, m4
    O8 [pd_90], [pd_25], [pd_80], [pd_57], m13, m14, m15, m0,  m5, m6, m7, m8
    E16_O16 psubd, psubd, paddd, paddd, paddd, psubd, psubd, 3 * %6 + %1, 12 * %6 + %1, %2, %3, %7

    ; o16[4]
    O8 [pd_57], [pd_80], [pd_25], [pd_90],  m9,  m10, m11, m12, m1, m2, m3, m4
    O8 [pd_9],  [pd_87], [pd_43], [pd_70],  m13, m14, m15, m0,  m5, m6, m7, m8
    E16_O16 psubd, psubd, paddd, psubd, psubd, paddd, paddd, 4 * %6 + %1, 11 * %6 + %1, %2, %3, %7

    ; o16[5]
    O8 [pd_43], [pd_90], [pd_57], [pd_25],  m9,  m10, m11, m12, m1, m2, m3, m4
    O8 [pd_87], [pd_70], [pd_9],  [pd_80],  m13, m14, m15, m0, m5, m6, m7, m8
    E16_O16 psubd, paddd, paddd, psubd, paddd, paddd, psubd, 5 * %6 + %1, 10 * %6 + %1, %2, %3, %7

    ; o16[6]
    O8 [pd_25], [pd_70], [pd_90], [pd_80], m9,  m10, m11, m12, m1, m2, m3, m4
    O8 [pd_43], [pd_9], [pd_57],  [pd_87], m13, m14, m15, m0,  m5, m6, m7, m8
    E16_O16 psubd, paddd, psubd, paddd, paddd, psubd, paddd, 6 * %6 + %1, 9 * %6 + %1, %2, %3, %7

    ; o16[7]
    O8 [pd_9], [pd_25], [pd_43],  [pd_57], m9,  m10, m11, m12, m1, m2, m3, m4
    O8 [pd_70], [pd_80], [pd_87], [pd_90], m13, m14, m15, m0,  m5, m6, m7, m8
    E16_O16 psubd, paddd, psubd, paddd, psubd, paddd, psubd, 7 * %6 + %1, 8 * %6 + %1, %2, %3, %7
%endmacro

%macro TRANSPOSE_16x16 0
    ; M1  M2  M3  M4 ^T      m1 m5 m9  m13   M_i^T = m_i
    ; M5  M6  M7  M8    -->  m2 m6 m10 m14
    ; M9  M10 M11 M12        m3 m7 m11 m15
    ; M13 M14 M15 M16        m4 m8 m12 m16

    ; M1 4x4 block
    TRANSPOSE_BLOCK 0, 0, 32

    ; M5, M2
    SWAP_BLOCKS 0, 128, 32, 0, 8
    ; M9, M3
    SWAP_BLOCKS 0, 256, 32, 0, 16
    ; M13, M4
    SWAP_BLOCKS 0, 384, 32, 0, 24

    ;M6
    TRANSPOSE_BLOCK 8, 128, 32

    ; M10, M7
    SWAP_BLOCKS 8, 256, 32, 128, 16
    ; M14, M8
    SWAP_BLOCKS 8, 384, 32, 128, 24

    ;M11
    TRANSPOSE_BLOCK 16, 256, 32

    ; M15, M12
    SWAP_BLOCKS 16, 384, 32, 256, 24

    ;M16
    TRANSPOSE_BLOCK 24, 384, 32
%endmacro

; void ff_hevc_idct_16x16_{8,10}_<opt>(int16_t *coeffs, int col_limit)
; %1 = bitdepth
%macro IDCT_16x16 1
cglobal hevc_idct_16x16_ %+ %1, 1, 1, 15, 1024, coeffs

    TR_16x4 0, 7, [pd_64], 64, 2, 32, 8, 16, 1
    TR_16x4 8, 7, [pd_64], 64, 2, 32, 8, 16, 1
    TR_16x4 16, 7, [pd_64], 64, 2, 32, 8, 16, 1
    TR_16x4 24, 7, [pd_64], 64, 2, 32, 8, 16, 1

    TRANSPOSE_16x16

    C_ADD_16 %1
    TR_16x4 0, shift, [arr_add], 64, 2, 32, 8, 16, 1
    TR_16x4 8, shift, [arr_add], 64, 2, 32, 8, 16, 1
    TR_16x4 16, shift, [arr_add], 64, 2, 32, 8, 16, 1
    TR_16x4 24, shift, [arr_add], 64, 2, 32, 8, 16, 1

    TRANSPOSE_16x16

    RET
%endmacro

; void ff_hevc_idct_32x32_{8,10}_<opt>(int16_t *coeffs, int col_limit)
; %1 = bitdepth
%macro IDCT_32x32 1
cglobal hevc_idct_32x32_ %+ %1, 1, 1, 15, 2048, coeffs
    TR_16x4 0, 7, [pd_64], 128, 4, 64, 16, 16, 2




    RET
%endmacro

; 8-bit
INIT_MMX mmxext
IDCT_DC_NL  4,      8
IDCT_DC     8,  2,  8

INIT_XMM sse2
IDCT_DC_NL  8,      8
IDCT_DC    16,  4,  8
IDCT_DC    32, 16,  8

INIT_XMM avx
IDCT_4x4 8
IDCT_8x8 8
IDCT_16x16 8

%if HAVE_AVX2_EXTERNAL
INIT_YMM avx2
IDCT_DC    16,  2,  8
IDCT_DC    32,  8,  8
%endif ;HAVE_AVX2_EXTERNAL

; 10-bit
INIT_MMX mmxext
IDCT_DC_NL  4,     10
IDCT_DC     8,  2, 10

INIT_XMM sse2
IDCT_DC_NL  8,     10
IDCT_DC    16,  4, 10
IDCT_DC    32, 16, 10

INIT_XMM avx
IDCT_4x4 10
IDCT_8x8 10
IDCT_16x16 10

%if HAVE_AVX2_EXTERNAL
INIT_YMM avx2
IDCT_DC    16,  2, 10
IDCT_DC    32,  8, 10
%endif ;HAVE_AVX2_EXTERNAL

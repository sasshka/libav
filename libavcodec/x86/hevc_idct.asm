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

%macro CLIP16 4
    packssdw   %3, %1, %2
    packssdw   %4, %2, %1
    pmovsxwd   %1, %3
    pmovsxwd   %2, %4
%endmacro

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

%macro C_ADD 2
    %assign shift (20 - %1)
    %assign c_add (1 << (shift - 1))
    %define arr_add pd_ %+ c_add
    mova %2, [arr_add]
%endmacro

; void ff_hevc_idct_4x4__{8,10}_<opt>(int16_t *coeffs, int col_limit)
; %1 = bitdepth
%macro IDCT_4x4 1
cglobal hevc_idct_4x4_ %+ %1, 1, 14, 14, coeffs
    mova     m13, [pd_64]
    pmovsxwd m1, [coeffsq]
    pmovsxwd m2, [coeffsq + 16]
    pmovsxwd m3, [coeffsq + 8]
    pmovsxwd m5, [coeffsq + 24]

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

%macro O8 4
    pmulld m9, m5, %1
    pmulld m10, m6, %2
    pmulld m11, m7, %3
    pmulld m12, m8, %4
%endmacro

%macro STORE_RES 4
    ADD_SHIFT m10, m13, %4 ; m10: res i1, i2, i3, i4
    packssdw  m10, m10
    movq    [coeffsq + %1], m10
    ADD_SHIFT %3, m13, %4
    packssdw  %3, %3
    movq    [coeffsq + %2], %3
%endmacro

%macro E8_O8 8
    %3 m9, m10
    %4 m9, m11
    %5 m9, m12     ; o8[0 + %1] for 4 rows
    paddd m10, m9, %8 ; o8 + e8
    psubd %8, m9      ; e8 - o8
    STORE_RES %6 + %1, %7 + %1, %8, %2
%endmacro

%macro TR_8x4 2
    ; load 4 columns of even rows
    pmovsxwd m1, [coeffsq + %1]
    pmovsxwd m2, [coeffsq + 2 * 32 + %1]
    pmovsxwd m3, [coeffsq + 2 * 16 + %1]
    pmovsxwd m5, [coeffsq + 2 * 48 + %1]

    TR_4x4 ; e8: m1, m2, m3, m4, for 4 columns only

    ; load 4 columns of odd rows
    pmovsxwd m5, [coeffsq + 16 + %1]
    pmovsxwd m6, [coeffsq + 2 * 24 + %1]
    pmovsxwd m7, [coeffsq + 2 * 40 + %1]
    pmovsxwd m8, [coeffsq + 2 * 56 + %1]

    mova m14, [pd_89]
    mova m15, [pd_75]
    mova m0,  [pd_50]

    O8 m14, m15, m0, [pd_18]
    E8_O8 %1, %2, paddd, paddd, paddd, 0, 16 * 7, m1

    mova m1, [pd_18]
    O8 m15, m1, m14, m0
    E8_O8 %1, %2, psubd, psubd, psubd, 16, 16 * 6, m2

    O8 m0, m14, m1, m15
    E8_O8 %1, %2, psubd, paddd, paddd, 16 * 2, 16 * 5, m3

    O8 m1, m0, m15, m14
    E8_O8 %1, %2, psubd, paddd, psubd, 16 * 3, 16 * 4, m4
%endmacro

%macro LOAD_BLOCK 5
    pmovsxwd %2, [coeffsq + %1]
    pmovsxwd %3, [coeffsq + 16 + %1]
    pmovsxwd %4, [coeffsq + 16 * 2 + %1]
    pmovsxwd %5, [coeffsq + 16 * 3 + %1]
%endmacro

%macro STORE_BLOCK 5
    packssdw %2, %2
    movq    [coeffsq + %1], %2
    packssdw %3, %3
    movq    [coeffsq + 16 + %1], %3
    packssdw %4, %4
    movq    [coeffsq + 16 * 2 + %1], %4
    packssdw %5, %5
    movq    [coeffsq + 16 * 3 + %1], %5
%endmacro

%macro TRANSPOSE_8x8 0
 ; transpose 8x8 M1 M2 ^T = M1^t M3^t
                  ; M3 M4      M2^t M4^t
    ; M1 4x4 block
    LOAD_BLOCK 0, m1, m2, m3, m4
    TRANSPOSE_4x4 ; m10, m6, m7, m8
    STORE_BLOCK 0, m10, m6, m7, m8

    ; M3
    LOAD_BLOCK 64, m5, m11, m12, m13

    ; M2
    LOAD_BLOCK 8, m1, m2, m3, m4
    TRANSPOSE_4x4 ; m10, m6, m7, m8
    STORE_BLOCK 64, m10, m6, m7, m8

    ; transpose and store M3
    SWAP m5, m1
    SWAP m11, m2
    SWAP m12, m3
    SWAP m13, m4
    TRANSPOSE_4x4
    STORE_BLOCK 8, m10, m6, m7, m8

    ; M4
    LOAD_BLOCK 64 + 8, m1, m2, m3, m4
    TRANSPOSE_4x4 ; m10, m6, m7, m8
    STORE_BLOCK 64 + 8, m10, m6, m7, m8
%endmacro

; void ff_hevc_idct_8x8__{8,10}_<opt>(int16_t *coeffs, int col_limit)
; %1 = bitdepth
%macro IDCT_8x8 1
cglobal hevc_idct_8x8_ %+ %1, 1, 14, 14, coeffs
    mova     m13, [pd_64]

    TR_8x4 0, 7
    TR_8x4 8, 7

    TRANSPOSE_8x8

    C_ADD %1, m13
    TR_8x4 0, shift
    TR_8x4 8, shift

    TRANSPOSE_8x8

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

%if HAVE_AVX2_EXTERNAL
INIT_YMM avx2
IDCT_DC    16,  2, 10
IDCT_DC    32,  8, 10
%endif ;HAVE_AVX2_EXTERNAL

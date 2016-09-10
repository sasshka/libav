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
pd_2048: times 4 dd 2048
pd_512: times 4 dd 512

; 4x4 transform coeffs
pw_64: times 8 dw 64
pw_64_m64: times 4 dw 64, -64
pw_83_36: times 4 dw 83, 36
pw_36_m83: times 4 dw 36, -83

; 8x8 transform coeffs
pw_89_75: times 4 dw 89, 75
pw_50_18: times 4 dw 50, 18

pw_75_m18: times 4 dw 75, -18
pw_m89_m50: times 4 dw -89, -50

pw_50_m89: times 4 dw 50, -89
pw_18_75: times 4 dw 18, 75

pw_18_m50: times 4 dw 18, -50
pw_75_m89: times 4 dw 75, -89

; 16x16 transformation coeffs
pd_90: times 4 dd 90
pd_87: times 4 dd 87
pd_80: times 4 dd 80
pd_70: times 4 dd 70
pd_57: times 4 dd 57
pd_43: times 4 dd 43
pd_25: times 4 dd 25
pd_9: times 4 dd 9

; 32x32 transform coeffs
pw_90: times 8 dw 90
pw_88_85: times 4 dw 88, 85
pw_82_78: times 4 dw 82, 78
pw_73_67: times 4 dw 73, 67
pw_61_54: times 4 dw 61, 54
pw_46_38: times 4 dw 46, 38
pw_31_22: times 4 dw 31, 22
pw_13_4: times 4 dw 13, 4

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

; add constant %2 to %1
; then shift %1 with %3
%macro ADD_SHIFT 3
    paddd %1, %2
    psrad %1, %3
%endmacro

%macro SCALE 2
    ADD_SHIFT m0, %1, %2
    ADD_SHIFT m1, %1, %2
    ADD_SHIFT m2, %1, %2
    ADD_SHIFT m3, %1, %2
%endmacro

; take 16 bit input in m1 and m3
; do the 4x4 vertical IDCT
; without SCALE, store 32 bit output
; in m0, m1, m2, m3
%macro TR_4x4 0
    ; interleaves src0 with src2 to m1
    ;         and src1 with scr3 to m3
    ; src0: 00 01 02 03     m1: 00 02 01 21 02 22 03 23
    ; src1: 10 11 12 13 -->
    ; src2: 20 21 22 23     m3: 10 30 11 31 12 32 13 33
    ; src3: 30 31 32 33

    SBUTTERFLY wd, 1, 3, 0

    pmaddwd m0, m1, [pw_64] ; e0
    pmaddwd m1, [pw_64_m64] ; e1
    pmaddwd m2, m3, [pw_83_36] ; o0
    pmaddwd m3, [pw_36_m83] ; o1

    SUMSUB_BA d, 2, 0, 4
    SUMSUB_BA d, 3, 1, 4

    SWAP m0, m2
    SWAP m1, m3
    SWAP m2, m3
%endmacro

;    m0,  m1, m2, m3 is transposed
; to m5, m6, m7, m8
%macro TRANSPOSE_4x4 0
    punpckldq m5, m0, m1
    punpckldq m6, m2, m3
    movlhps   m5, m6

    punpckldq m7, m0, m1
    punpckldq m6, m2, m3
    movhlps   m6, m7

    punpckhdq m7, m0, m1
    punpckhdq m8, m2, m3
    movlhps   m7, m8

    punpckhdq m9, m0, m1
    punpckhdq m8, m2, m3
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

; %1, %2 - registers to load packed 16 bit values to
; %3, %4, %5, %6 - vertical offsets
; %7 - horizontal offset
%macro LOAD_BLOCK 7
    movq   %1, [coeffsq + %3 + %7]
    movhps %1, [coeffsq + %5 + %7]
    movq   %2, [coeffsq + %4 + %7]
    movhps %2, [coeffsq + %6 + %7]
%endmacro

; load coeffs to %2, %3, %4, %5
; %1 - horizontal offset
; %6, %7, %8, %9 - vertical offsets
%macro LOAD_BLOCK_T 9
    pmovsxwd %2, [coeffsq + %9 + %1]
    pmovsxwd %3, [coeffsq + %6 + %1]
    pmovsxwd %4, [coeffsq + %7 + %1]
    pmovsxwd %5, [coeffsq + %8 + %1]
%endmacro

; void ff_hevc_idct_4x4__{8,10}_<opt>(int16_t *coeffs, int col_limit)
; %1 = bitdepth
%macro IDCT_4x4 1
cglobal hevc_idct_4x4_ %+ %1, 1, 14, 14, coeffs
    mova m1, [coeffsq]
    mova m3, [coeffsq + 16]

    TR_4x4
    mova m9, [pd_64]
    SCALE m9, 7

    TRANSPOSE_4x4

    SWAP m0, m5
    SWAP m1, m6
    SWAP m2, m7
    SWAP m3, m8

    ; clip16
    packssdw m1, m0, m1
    packssdw m3, m2, m3

    TR_4x4
    C_ADD %1, m9
    SCALE m9, shift
    TRANSPOSE_4x4

    packssdw m5, m6
    movdqa   [coeffsq], m5
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
    movu    [rsp + %1], m7
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
; %3, %4 - transform coeffs
; %5 - vertical offset for e8 + o8
; %6 - vertical offset for e8 - o8
; %7 - register with o8 inside
; %8 - block_size
%macro E8_O8 8
    pmaddwd m6, m4, %3
    pmaddwd m7, m5, %4
    paddd m6, m7

    paddd m7, m6, %7 ; o8 + e8
    psubd %7, m6     ; e8 - o8
    STORE_%8 %5 + %1, %6 + %1, %7, %2, m7, m8
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
    LOAD_BLOCK  m1, m3, 0, 2 * %4 * %3, %4 * %3, 3 * %4 * %3, %1

    TR_4x4 ; e8: m0, m1, m2, m3, for 4 columns only

    ; load 4 columns of odd rows
    LOAD_BLOCK m4, m5, %4 * %5, 3 * %4 * %5, 5 * %4 * %5, 7 * %4 * %5, %1

    ; 00 01 02 03
    ; 10 11 12 13      m4: 10 30 11 31 12 32 13 33

    ; ...        -- >
    ;                  m5: 50 70 51 71 52 72 53 73
    ; 70 71 72 73
    SBUTTERFLY wd, 4, 5, 6

    E8_O8 %1, %2, [pw_89_75],  [pw_50_18],   0,      %5 * 7, m0, %6
    E8_O8 %1, %2, [pw_75_m18], [pw_m89_m50], %5,     %5 * 6, m1, %6
    E8_O8 %1, %2, [pw_50_m89], [pw_18_75],   %5 * 2, %5 * 5, m2, %6
    E8_O8 %1, %2, [pw_18_m50], [pw_75_m89],  %5 * 3, %5 * 4, m3, %6
%endmacro

%macro STORE_BLOCK 9
    packssdw %2, %3
    movq     [coeffsq + %9 + %1], %2
    movhps   [coeffsq + %6 + %1], %2
    packssdw %4, %5
    movq     [coeffsq + %7 + %1], %4
    movhps   [coeffsq + %8 + %1], %4
%endmacro

%macro STORE_PACKED 7
    movq     [coeffsq + %3 + %7], %1
    movhps   [coeffsq + %4 + %7], %1
    movq     [coeffsq + %5 + %7], %2
    movhps   [coeffsq + %6 + %7], %2
%endmacro

; transpose src packed in m4, m5
;                      to m3, m1
%macro TRANSPOSE_PACKED 0
    SBUTTERFLY wd, 4, 5, 8
    SBUTTERFLY dq, 4, 5, 8
%endmacro

; load block i and store it in m6, m7
; load block j to m4, m5 transpose it
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
    LOAD_BLOCK m6, m7, %2, %2 + %3, %2 + 2 * %3, %2 + 3 * %3, %1

    ; M_j
    LOAD_BLOCK m4, m5, %4, %4 + %3, %4 + 2 * %3, %4 + 3 * %3, %5
    TRANSPOSE_PACKED
    STORE_PACKED m4, m5, %2, %2 + %3, %2 + 2 * %3, %2 + 3 * %3, %1

    ; transpose and store M_i
    SWAP m6, m4
    SWAP m7, m5
    TRANSPOSE_PACKED
    STORE_PACKED m4, m5, %4, %4 + %3, %4 + 2 * %3, %4 + 3 * %3, %5
%endmacro

; %1 - horizontal offset
; %2 - 2 - vertical offset of the block
; %3 - width in bytes
%macro TRANSPOSE_BLOCK 3
    LOAD_BLOCK m4, m5, %2, %2 + %3, %2 + 2 * %3, %2 + 3 * %3, %1
    TRANSPOSE_PACKED
    STORE_PACKED m4, m5, %2, %2 + %3, %2 + 2 * %3, %2 + 3 * %3, %1
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
    mova m8, [pd_64]
    TR_8x4 0, 7, 32, 1, 16, 8
    TR_8x4 8, 7, 32, 1, 16, 8

    TRANSPOSE_8x8

    C_ADD %1, m8
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

; %1 - horizontal offset
; %6 - width in bytes
; %7 - STORE 8/16
; %9 - step: 1 for 16x16, 2 for 32x32
%macro TR_16x4 9
    mova m8, [pd_64]

    ; produce 8x4 matrix of e16 coeffs
    ; for 4 first rows and store it on stack (128 bytes)
    TR_8x4 %1, 7, %4, %5, %6, %8

    ; load 8 even rows
    LOAD_BLOCK_T %1, m9, m10, m11, m12, %9 * 3 * %6, %9 * 5 * %6, %9 * 7 * %6, %9 * %6
    LOAD_BLOCK_T %1, m13, m14, m15, m0, %9 * 11 * %6, %9 * 13 * %6, %9 * 15 * %6, %9 * 9 * %6

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

; %1, 2 - transform constants
; %3, 4 - regs with interleaved coeffs
%macro ADD 4
    pmaddwd m8, %3, %1
    pmaddwd m9, %4, %2
    paddd m10, m8
    paddd m10, m9
%endmacro

; %1 ... %8 transform coeffs
; %9 stack offset for e32
; %10, %11 offsets for storing e+o/e-o back to coeffsq
; %12 - shift
; %13 - add
%macro E32_O32 13
    pxor m10, m10
    ADD %1, %2, m0, m1
    ADD %3, %4, m2, m3
    ADD %5, %6, m4, m5
    ADD %7, %8, m6, m7

    packssdw m0, m0
    movq [coeffsq], m0
    movu m11, [rsp + %9]
    paddd m12, m10, m11 ; o32 + e32
    psubd m11, m10      ; e32 - o32
    ;STORE_8 %10, %11, m11, %12, m12, %13
%endmacro

; %1 - horizontal offset
%macro TR_32x4 3
    TR_16x4 %1, 7, [pd_64], 128, 4, 64, 16, 16, 2

    LOAD_BLOCK m0, m1,     64, 3 * 64,   5 * 64,  7 * 64, %1
    LOAD_BLOCK m2, m3, 9 * 64, 11 * 64, 13 * 64, 15 * 64, %1
    LOAD_BLOCK m4, m5, 17 * 64, 19 * 64, 21 * 64, 23 * 64, %1
    LOAD_BLOCK m6, m7, 25 * 64, 27 * 64, 29 * 64, 31 * 64, %1
    packssdw m0, m0
    mova [coeffsq], m0

    SBUTTERFLY wd, 0, 1, 8
    SBUTTERFLY wd, 2, 3, 8
    SBUTTERFLY wd, 4, 5, 8
    SBUTTERFLY wd, 6, 7, 8

    ;E32_O32 [pw_90], [pw_88_85], [pw_82_78], [pw_73_67], [pw_61_54], [pw_46_38], [pw_31_22], [pw_13_4], %1, %1,  31 * 64 + %1, %2, %3


%endmacro


; void ff_hevc_idct_32x32_{8,10}_<opt>(int16_t *coeffs, int col_limit)
; %1 = bitdepth
%macro IDCT_32x32 1
cglobal hevc_idct_32x32_ %+ %1, 1, 1, 15, 2048, coeffs
    TR_32x4 0, 7, [pd_64]

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
IDCT_32x32 8

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
IDCT_32x32 10

%if HAVE_AVX2_EXTERNAL
INIT_YMM avx2
IDCT_DC    16,  2, 10
IDCT_DC    32,  8, 10
%endif ;HAVE_AVX2_EXTERNAL

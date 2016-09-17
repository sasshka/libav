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
pw_90_87: times 4 dw 90, 87
pw_80_70: times 4 dw 80, 70
pw_57_43: times 4 dw 57, 43
pw_25_9: times 4 dw 25, 9

pw_87_57: times 4 dw 87, 57
pw_9_m43: times 4 dw 9, -43
pw_m80_m90: times 4 dw -80, -90
pw_m70_m25: times 4 dw -70, -25

pw_80_9: times 4 dw 80, 9
pw_m70_m87: times 4 dw -70, -87
pw_m25_57: times 4 dw -25, 57
pw_90_43: times 4 dw 90, 43

pw_70_m43: times 4 dw 70, -43
pw_m87_9: times 4 dw -87, 9
pw_90_25: times 4 dw 90, 25
pw_m80_m57: times 4 dw -80, -57

pw_57_m80: times 4 dw 57, -80
pw_m25_90: times 4 dw -25, 90
pw_m9_m87: times 4 dw -9, -87
pw_43_70: times 4 dw 43, 70

pw_43_m90: times 4 dw 43, -90
pw_57_25: times 4 dw 57, 25
pw_m87_70: times 4 dw -87, 70
pw_9_m80: times 4 dw 9, -80

pw_25_m70: times 4 dw 25, -70
pw_90_m80: times 4 dw 90, -80
pw_43_9: times 4 dw 43, 9
pw_m57_87: times 4 dw -57, 87

pw_9_m25: times 4 dw 9, -25
pw_43_m57: times 4 dw 43, -57
pw_70_m80: times 4 dw 70, -80
pw_87_m90: times 4 dw 87, -90

; 32x32 transform coeffs
trans_coeff32_0: times 8 dw 90
times 4 dw 88, 85
times 4 dw 82, 78
times 4 dw 73, 67
times 4 dw 61, 54
times 4 dw 46, 38
times 4 dw 31, 22
times 4 dw 13, 4

trans_coeff32_1: times 4 dw 90, 82
times 4 dw 67, 46
times 4 dw 22, -4
times 4 dw -31, -54
times 4 dw -73, -85
times 4 dw -90, -88
times 4 dw -78, -61
times 4 dw -38, -13

trans_coeff32_2: times 4 dw 88, 67
times 4 dw 31, -13
times 4 dw -54, -82
times 4 dw -90, -78
times 4 dw -46, -4
times 4 dw 38, 73
times 4 dw 90, 85
times 4 dw 61, 22

trans_coeff32_3: times 4 dw 85, 46
times 4 dw -13, -67
times 4 dw -90, -73
times 4 dw -22, 38
times 4 dw 82, 88
times 4 dw 54, -4
times 4 dw -61, -90
times 4 dw -78, -31

trans_coeff32_4: times 4 dw 82, 22
times 4 dw -54, -90
times 4 dw -61, 13
times 4 dw 78, 85
times 4 dw 31, -46
times 4 dw -90, -67
times 4 dw 4, 73
times 4 dw 88, 38

trans_coeff32_5: times 4 dw 78, -4
times 4 dw -82, -73
times 4 dw 13, 85
times 4 dw 67, -22
times 4 dw -88, -61
times 4 dw 31, 90
times 4 dw 54, -38
times 4 dw -90, -46

trans_coeff32_6: times 4 dw 73, -31
times 4 dw -90, -22
times 4 dw 78, 67
times 4 dw -38, -90
times 4 dw -13, 82
times 4 dw 61, -46
times 4 dw -88, -4
times 4 dw 85, 54

trans_coeff32_7: times 4 dw 67, -54
times 4 dw -78, 38
times 4 dw 85, -22
times 4 dw -90, 4
times 4 dw 90, 13
times 4 dw -88, -31
times 4 dw 82, 46
times 4 dw -73, -61

trans_coeff32_8: times 4 dw 61, -73
times 4 dw -46, 82
times 4 dw 31, -88
times 4 dw -13, 90
times 4 dw -4, -90
times 4 dw 22, 85
times 4 dw -38, -78
times 4 dw 54, 67

trans_coeff32_9: times 4 dw 54, -85
times 4 dw -4, 88
times 4 dw -46, -61
times 4 dw 82, 13
times 4 dw -90, 38
times 4 dw 67, -78
times 4 dw -22, 90
times 4 dw -31, -73

trans_coeff32_10: times 4 dw 46, -90
times 4 dw 38, 54
times 4 dw -90, 31
times 4 dw 61, -88
times 4 dw 22, 67
times 4 dw -85, 13
times 4 dw 73, -82
times 4 dw 4, 78

trans_coeff32_11: times 4 dw 38, -88
times 4 dw 73, -4
times 4 dw -67, 90
times 4 dw -46, -31
times 4 dw 85, -78
times 4 dw 13, 61
times 4 dw -90, 54
times 4 dw 22, -82

trans_coeff32_12: times 4 dw 31, -78
times 4 dw 90, -61
times 4 dw 4, 54
times 4 dw -88, 82
times 4 dw -38, -22
times 4 dw 73, -90
times 4 dw 67, -13
times 4 dw -46, 85

trans_coeff32_13: times 4 dw 22, -61
times 4 dw 85, -90
times 4 dw 73, -38
times 4 dw -4, 46
times 4 dw -78, 90
times 4 dw -82, 54
times 4 dw -13, -31
times 4 dw 67, -88

trans_coeff32_14: times 4 dw 13, -38
times 4 dw 61, -78
times 4 dw 88, -90
times 4 dw 85, -73
times 4 dw 54, -31
times 4 dw 4, 22
times 4 dw -46, 67
times 4 dw -82, 90

trans_coeff32_15: times 4 dw 4, -13
times 4 dw 22, -31
times 4 dw 38, -46
times 4 dw 54, -61
times 4 dw 67, -73
times 4 dw 78, -82
times 4 dw 85, -88
times 4 dw 90, -90

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

; IDCT 4x4, expects input in m0, m1
; %1 - shift
; %2 - 1/0 - SCALE and Transpose or not
%macro TR_4x4 2
    ; interleaves src0 with src2 to m0
    ;         and src1 with scr3 to m2
    ; src0: 00 01 02 03     m0: 00 02 01 21 02 22 03 23
    ; src1: 10 11 12 13 -->
    ; src2: 20 21 22 23     m1: 10 30 11 31 12 32 13 33
    ; src3: 30 31 32 33

    SBUTTERFLY wd, 0, 1, 2

    pmaddwd m2, m0, [pw_64] ; e0
    pmaddwd m3, m1, [pw_83_36] ; o0
    pmaddwd m0, [pw_64_m64] ; e1
    pmaddwd m1, [pw_36_m83] ; o1

%if %2 == 1
    %assign %%add 1 << (%1 - 1)
    mova m4, [pd_ %+ %%add]
    paddd m2 ,m4
    paddd m0, m4
%endif

    SUMSUB_BADC d, 3, 2, 1, 0, 4

%if %2 == 1
    psrad m3, %1 ; e0 + o0
    psrad m1, %1 ; e1 + o1
    psrad m2, %1 ; e0 - o0
    psrad m0, %1 ; e1 - o1
    ;clip16
    packssdw m3, m1
    packssdw m0, m2
    ; Transpose
    SBUTTERFLY wd, 3, 0, 1
    SBUTTERFLY wd, 3, 0, 1
    SWAP 3, 1, 0
%else
    SWAP 3, 0
    SWAP 3, 2
%endif
%endmacro

%macro DEFINE_BIAS 1
    %assign shift (20 - %1)
    %assign c_add (1 << (shift - 1))
    %define arr_add pd_ %+ c_add
%endmacro

; %1 - bit_depth
; %2 - register add constant
; is loaded to
; shift = 20 - bit_depth
%macro LOAD_BIAS 2
    DEFINE_BIAS %1
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

; void ff_hevc_idct_4x4__{8,10}_<opt>(int16_t *coeffs, int col_limit)
; %1 = bitdepth
%macro IDCT_4x4 1
cglobal hevc_idct_4x4_ %+ %1, 1, 14, 14, coeffs
    mova m0, [coeffsq]
    mova m1, [coeffsq + 16]

    TR_4x4 7, 1
    TR_4x4 20 - %1, 1

    mova [coeffsq], m0
    mova [coeffsq + 16], m1
    RET
%endmacro

; store intermedite e16 coeffs on stack
; as 8x4 matrix - writes 128 bytes to stack
; from m10: e8 + o8, with %1 offset
; and  %3:  e8 - o8, with %2 offset
; %4 - shift, unused here
%macro STORE_16 5
    movu    [rsp + %1], %5
    movu    [rsp + %2], %3
%endmacro

; scale, pack (clip16) and store the residuals     0 e8[0] + o8[0] --> + %1
; 4 at one time (4 columns)                        1 e8[1] + o8[1]
; from %5: e8/16 + o8/16, with %1 offset                  ...
; and  %3: e8/16 - o8/16, with %2 offset           6 e8[1] - o8[1]
; %4 - shift                                       7 e8[0] - o8[0] --> + %2
%macro STORE_8 5
    psrad    %5, %4
    psrad    %3, %4
    packssdw  %5, %3
    movq      [coeffsq + %1], %5
    movhps    [coeffsq + %2], %5
%endmacro

; %1 - horizontal offset
; %2 - shift
; %3, %4 - transform coeffs
; %5 - vertical offset for e8 + o8
; %6 - vertical offset for e8 - o8
; %7 - register with e8 inside
; %8 - block_size
%macro E8_O8 8
    pmaddwd m6, m4, %3
    pmaddwd m7, m5, %4
    paddd m6, m7

%if %8 == 8
    paddd %7, m8
%endif

    paddd m7, m6, %7 ; o8 + e8
    psubd %7, m6     ; e8 - o8
    STORE_%8 %5 + %1, %6 + %1, %7, %2, m7
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
    LOAD_BLOCK  m0, m1, 0, 2 * %4 * %3, %4 * %3, 3 * %4 * %3, %1

    TR_4x4 7, 0 ; e8: m0, m1, m2, m3, for 4 columns only

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

%macro STORE_PACKED 7
    movq     [coeffsq + %3 + %7], %1
    movhps   [coeffsq + %4 + %7], %1
    movq     [coeffsq + %5 + %7], %2
    movhps   [coeffsq + %6 + %7], %2
%endmacro

; transpose src packed in m4, m5
;                      to m3, m1
%macro TRANSPOSE 0
    SBUTTERFLY wd, 4, 5, 8
    SBUTTERFLY dq, 4, 5, 8
%endmacro

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
    TRANSPOSE
    STORE_PACKED m4, m5, %2, %2 + %3, %2 + 2 * %3, %2 + 3 * %3, %1

    ; transpose and store M_i
    SWAP m6, m4
    SWAP m7, m5
    TRANSPOSE
    STORE_PACKED m4, m5, %4, %4 + %3, %4 + 2 * %3, %4 + 3 * %3, %5
%endmacro

; %1 - horizontal offset
; %2 - 2 - vertical offset of the block
; %3 - width in bytes
%macro TRANSPOSE_BLOCK 3
    LOAD_BLOCK m4, m5, %2, %2 + %3, %2 + 2 * %3, %2 + 3 * %3, %1
    TRANSPOSE
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

    LOAD_BIAS %1, m8
    TR_8x4 0, shift, 32, 1, 16, 8
    TR_8x4 8, shift, 32, 1, 16, 8

    TRANSPOSE_8x8

    RET
%endmacro

; %1, 2 - transform constants
; %3, 4 - regs with interleaved coeffs
%macro ADD 4
    pmaddwd m8, %3, %1
    pmaddwd m9, %4, %2
    paddd   m8, m9
    paddd   m10, m8
%endmacro

; %1 ... %4 transform coeffs
; %5, %6 offsets for storing e+o/e-o back to coeffsq
; %7 - shift
; %8 - add
; %9 - block_size
%macro E16_O16 9
    pxor m10, m10
    ADD %1, %2, m0, m1
    ADD %3, %4, m2, m3

    movu m4, [rsp + %5]
%if %9 == 8
    paddd m4, %8
%endif

    paddd m5, m10, m4 ; o16 + e16
    psubd m4, m10      ; e16 - o16
    STORE_%9 %5, %6, m4, %7, m5
%endmacro

%macro TR_16x4 9
    mova m12, [pd_64]

    ; produce 8x4 matrix of e16 coeffs
    ; for 4 first rows and store it on stack (128 bytes)
    TR_8x4 %1, 7, %4, %5, %6, %8

    ; load 8 even rows
    LOAD_BLOCK m0, m1, %9 * %6, %9 * 3 * %6, %9 * 5 * %6, %9 * 7 * %6, %1
    LOAD_BLOCK m2, m3, %9 * 9 * %6, %9 * 11 * %6, %9 * 13 * %6, %9 * 15 * %6, %1

    SBUTTERFLY wd, 0, 1, 4
    SBUTTERFLY wd, 2, 3, 4

    mova m7, %3

    E16_O16 [pw_90_87], [pw_80_70], [pw_57_43], [pw_25_9],  0 + %1, 15 * %6 + %1, %2, m7, %7
    E16_O16 [pw_87_57], [pw_9_m43], [pw_m80_m90], [pw_m70_m25], %6 + %1, 14 * %6 + %1, %2, m7, %7
    E16_O16 [pw_80_9], [pw_m70_m87], [pw_m25_57], [pw_90_43], 2 * %6 + %1, 13 * %6 + %1, %2, m7, %7
    E16_O16 [pw_70_m43], [pw_m87_9], [pw_90_25], [pw_m80_m57], 3 * %6 + %1, 12 * %6 + %1, %2, m7, %7
    E16_O16 [pw_57_m80], [pw_m25_90], [pw_m9_m87], [pw_43_70], 4 * %6 + %1, 11 * %6 + %1, %2, m7, %7
    E16_O16 [pw_43_m90], [pw_57_25], [pw_m87_70], [pw_9_m80], 5 * %6 + %1, 10 * %6 + %1, %2, m7, %7
    E16_O16 [pw_25_m70], [pw_90_m80], [pw_43_9], [pw_m57_87], 6 * %6 + %1, 9 * %6 + %1, %2, m7, %7
    E16_O16 [pw_9_m25], [pw_43_m57], [pw_70_m80], [pw_87_m90], 7 * %6 + %1, 8 * %6 + %1, %2, m7, %7
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

    DEFINE_BIAS %1
    TR_16x4 0, shift, [arr_add], 64, 2, 32, 8, 16, 1
    TR_16x4 8, shift, [arr_add], 64, 2, 32, 8, 16, 1
    TR_16x4 16, shift, [arr_add], 64, 2, 32, 8, 16, 1
    TR_16x4 24, shift, [arr_add], 64, 2, 32, 8, 16, 1
    TRANSPOSE_16x16

    RET
%endmacro

; %1 - transform coeffs
; %2 - stack offset for e32
; %2, %3 offsets for storing e+o/e-o back to coeffsq
; %4 - shift
%macro E32_O32 4
    pxor m10, m10
    ADD [%1], [%1 + 16], m0, m1
    ADD [%1 + 2 * 16], [%1 + 3 * 16], m2, m3
    ADD [%1 + 4 * 16], [%1 + 5 * 16], m4, m5
    ADD [%1 + 6 * 16], [%1 + 7 * 16], m6, m7

    movu m11, [rsp + %2]
    paddd m11, m14
    paddd m12, m10, m11 ; o32 + e32
    psubd m11, m10      ; e32 - o32
    STORE_8 %2, %3, m11, %4, m12
%endmacro

; %1 - horizontal offset
; %2 - bitdepth
%macro TR_32x4 3
    TR_16x4 %1, 7, [pd_64], 128, 4, 64, 16, 16, 2

    LOAD_BLOCK m0, m1,      64,  3 * 64,  5 * 64,  7 * 64, %1
    LOAD_BLOCK m2, m3,  9 * 64, 11 * 64, 13 * 64, 15 * 64, %1
    LOAD_BLOCK m4, m5, 17 * 64, 19 * 64, 21 * 64, 23 * 64, %1
    LOAD_BLOCK m6, m7, 25 * 64, 27 * 64, 29 * 64, 31 * 64, %1

    SBUTTERFLY wd, 0, 1, 8
    SBUTTERFLY wd, 2, 3, 8
    SBUTTERFLY wd, 4, 5, 8
    SBUTTERFLY wd, 6, 7, 8

%if %3 == 1
    %assign shift 7
    mova m14, [pd_64]
%else
    LOAD_BIAS %2, m14
%endif

    E32_O32 trans_coeff32_0,           %1, 31 * 64 + %1, shift
    E32_O32 trans_coeff32_1,      64 + %1, 30 * 64 + %1, shift
    E32_O32 trans_coeff32_2,  2 * 64 + %1, 29 * 64 + %1, shift
    E32_O32 trans_coeff32_3,  3 * 64 + %1, 28 * 64 + %1, shift
    E32_O32 trans_coeff32_4,  4 * 64 + %1, 27 * 64 + %1, shift
    E32_O32 trans_coeff32_5,  5 * 64 + %1, 26 * 64 + %1, shift
    E32_O32 trans_coeff32_6,  6 * 64 + %1, 25 * 64 + %1, shift
    E32_O32 trans_coeff32_7,  7 * 64 + %1, 24 * 64 + %1, shift
    E32_O32 trans_coeff32_8,  8 * 64 + %1, 23 * 64 + %1, shift
    E32_O32 trans_coeff32_9,  9 * 64 + %1, 22 * 64 + %1, shift
    E32_O32 trans_coeff32_10, 10 * 64 + %1, 21 * 64 + %1, shift
    E32_O32 trans_coeff32_11, 11 * 64 + %1, 20 * 64 + %1, shift
    E32_O32 trans_coeff32_12, 12 * 64 + %1, 19 * 64 + %1, shift
    E32_O32 trans_coeff32_13, 13 * 64 + %1, 18 * 64 + %1, shift
    E32_O32 trans_coeff32_14, 14 * 64 + %1, 17 * 64 + %1, shift
    E32_O32 trans_coeff32_15, 15 * 64 + %1, 16 * 64 + %1, shift
%endmacro

%macro TRANSPOSE_32x32 0
    ; M0  M1 ... M7
    ; M8         M15
    ;
    ; ...
    ;
    ; M56        M63

    TRANSPOSE_BLOCK 0, 0, 64 ; M1

    SWAP_BLOCKS 0,     256, 64, 0,     8 ; M8,  M1
    SWAP_BLOCKS 0, 2 * 256, 64, 0, 2 * 8 ; M16, M2
    SWAP_BLOCKS 0, 3 * 256, 64, 0, 3 * 8 ; M24, M3
    SWAP_BLOCKS 0, 4 * 256, 64, 0, 4 * 8
    SWAP_BLOCKS 0, 5 * 256, 64, 0, 5 * 8
    SWAP_BLOCKS 0, 6 * 256, 64, 0, 6 * 8
    SWAP_BLOCKS 0, 7 * 256, 64, 0, 7 * 8

    TRANSPOSE_BLOCK 8, 256, 64 ; M9
    SWAP_BLOCKS 8, 2 * 256, 64, 256, 2 * 8 ; M17, M10
    SWAP_BLOCKS 8, 3 * 256, 64, 256, 3 * 8
    SWAP_BLOCKS 8, 4 * 256, 64, 256, 4 * 8
    SWAP_BLOCKS 8, 5 * 256, 64, 256, 5 * 8
    SWAP_BLOCKS 8, 6 * 256, 64, 256, 6 * 8
    SWAP_BLOCKS 8, 7 * 256, 64, 256, 7 * 8

    TRANSPOSE_BLOCK 2 * 8, 2 * 256, 64 ; M9
    SWAP_BLOCKS 2 * 8, 3 * 256, 64, 2 * 256, 3 * 8
    SWAP_BLOCKS 2 * 8, 4 * 256, 64, 2 * 256, 4 * 8
    SWAP_BLOCKS 2 * 8, 5 * 256, 64, 2 * 256, 5 * 8
    SWAP_BLOCKS 2 * 8, 6 * 256, 64, 2 * 256, 6 * 8
    SWAP_BLOCKS 2 * 8, 7 * 256, 64, 2 * 256, 7 * 8

    TRANSPOSE_BLOCK 3 * 8, 3 * 256, 64 ; M27
    SWAP_BLOCKS 3 * 8, 4 * 256, 64, 3 * 256, 4 * 8
    SWAP_BLOCKS 3 * 8, 5 * 256, 64, 3 * 256, 5 * 8
    SWAP_BLOCKS 3 * 8, 6 * 256, 64, 3 * 256, 6 * 8
    SWAP_BLOCKS 3 * 8, 7 * 256, 64, 3 * 256, 7 * 8

    TRANSPOSE_BLOCK 4 * 8, 4 * 256, 64 ; M36
    SWAP_BLOCKS 4 * 8, 5 * 256, 64, 4 * 256, 5 * 8
    SWAP_BLOCKS 4 * 8, 6 * 256, 64, 4 * 256, 6 * 8
    SWAP_BLOCKS 4 * 8, 7 * 256, 64, 4 * 256, 7 * 8

    TRANSPOSE_BLOCK 5 * 8, 5 * 256, 64 ; M45
    SWAP_BLOCKS 5 * 8, 6 * 256, 64, 5 * 256, 6 * 8
    SWAP_BLOCKS 5 * 8, 7 * 256, 64, 5 * 256, 7 * 8

    TRANSPOSE_BLOCK 6 * 8, 6 * 256, 64 ; M54
    SWAP_BLOCKS 6 * 8, 7 * 256, 64, 6 * 256, 7 * 8

    TRANSPOSE_BLOCK 7 * 8, 7 * 256, 64 ; M63
%endmacro

; void ff_hevc_idct_32x32_{8,10}_<opt>(int16_t *coeffs, int col_limit)
; %1 = bitdepth
%macro IDCT_32x32 1
cglobal hevc_idct_32x32_ %+ %1, 1, 1, 15, 4096, coeffs
    TR_32x4 0, %1, 1
    TR_32x4 8, %1, 1
    TR_32x4 16, %1, 1
    TR_32x4 24, %1, 1
    TR_32x4 32, %1, 1
    TR_32x4 40, %1, 1
    TR_32x4 48, %1, 1
    TR_32x4 56, %1, 1

    TRANSPOSE_32x32

    TR_32x4 0, %1, 0
    TR_32x4 8, %1, 0
    TR_32x4 16, %1, 0
    TR_32x4 24, %1, 0
    TR_32x4 32, %1, 0
    TR_32x4 40, %1, 0
    TR_32x4 48, %1, 0
    TR_32x4 56, %1, 0

    TRANSPOSE_32x32

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

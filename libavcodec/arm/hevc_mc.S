#include "libavutil/arm/asm.S"

const coeffs, align=4
        .short 10, 58, 17, 5
        .short 4, 11, 40, 0
endconst

.macro qpel_filter_2 in0, in1, in2, in3, in4, in5, in6, in7
        vaddl.u8 q15, \in0, \in1
        vaddl.u8 q14, \in2, \in3
        vshl.s16 q14, q14, #2
        vsub.s16 q15, q14, q15
        vaddl.u8 q14, \in4, \in5
        vmul.s16 q14, q14, d0[1]
        vsub.s16 q15, q15, q14
        vaddl.u8 q14, \in6, \in7
        vmul.s16 q14, q14, d0[2]
        vadd.s16 q15, q15, q14
.endm

.macro load
        @r2 - src, r3 - srcstride
        add r6, r3, r3, lsl #1
        sub r4, r2, r6
        vld1.u8  {d16}, [r4], r3 @ src - 3*stride
        vld1.u8  {d1}, [r4], r3 @ src - 2*stride
        vld1.u8  {d2}, [r4], r3 @ src - stride
        vld1.u8  {d3}, [r4], r3 @ src
        vld1.u8  {d4}, [r4], r3 @ src + stride
        vld1.u8  {d5}, [r4], r3 @ src + 2*stride
        vld1.u8  {d6}, [r4], r3 @ src + 3*stride
.endm

function put_hevc_qpel_h1_neon, export=1
        ldr r12, [sp, #4] @height
        push {r4-r6}

        load

        movrel r6, coeffs
        vld1.s16 {d0}, [r6]
endfunc

function put_hevc_qpel_h2_neon, export=1
        ldr r12, [sp, #4] @height
        push {r4-r6}

        load
        vld1.u8  {d7}, [r4], r3 @ src + 4*stride

        movrel r6, coeffs + 16
        vld1.s16 {d0}, [r6]

1:      qpel_filter_2 d16, d7, d1, d6, d2, d5, d3, d4
        mov r5, r0
        vst1.s16 d30, [r5], r1
        vld1.u8  {d16}, [r4], r3
        qpel_filter_2 d7, d1, d6, d2, d5, d3, d4, d16
        vst1.s16 d30, [r5], r1
        vld1.u8  {d7}, [r4], r3
        qpel_filter_2 d1, d6, d2, d5, d3, d4, d16, d7
        vst1.s16 d30, [r5], r1
        vld1.u8  {d1}, [r4], r3
        qpel_filter_2 d6, d2, d5, d3, d4, d16, d7, d1
        vst1.s16 d30, [r5], r1
        subs r12, #4
        bne 2f

        vld1.u8  {d6}, [r4], r3
        qpel_filter_2 d2, d5, d3, d4, d16, d7, d1, d6
        vst1.s16 d30, [r5], r1
        vld1.u8  {d2}, [r4], r3
        qpel_filter_2 d5, d3, d4, d16, d7, d1, d6, d2
        vst1.s16 d30, [r5], r1
        vld1.u8  {d5}, [r4], r3
        qpel_filter_2 d3, d4, d16, d7, d1, d6, d2, d5
        vst1.s16 d30, [r5], r1
        vld1.u8  {d3}, [r4], r3
        qpel_filter_2 d4, d16, d7, d1, d6, d2, d5, d3
        vst1.s16 d30, [r5], r1

        vld1.u8  {d4}, [r4], r3
        subs r12, #4
        bne 1b
       2:  pop {r4-r6}
        bx lr
endfunc


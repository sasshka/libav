/*
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

#include "config.h"
#if HAVE_ALTIVEC_H
#include <altivec.h>
#endif

#include "libavutil/attributes.h"
#include "libavutil/cpu.h"
#include "libavutil/ppc/cpu.h"
#include "libavutil/ppc/types_altivec.h"
#include "libavutil/ppc/util_altivec.h"
#include "libavcodec/hevc.h"

static const int16_t trans4[4][8] = {
    { 64,  64, 64,  64, 64,  64, 64,  64 },
    { 83,  36, 83,  36, 83,  36, 83,  36 },
    { 64, -64, 64, -64, 64, -64, 64, -64 },
    { 36, -83, 36, -83, 36, -83, 36, -83 },
};

static const int16_t mask[2][8] = {
    { 0x00, 0x04, 0x10, 0x14, 0x01, 0x05, 0x11, 0x15 },
    { 0x02, 0x06, 0x12, 0x16, 0x03, 0x07, 0x13, 0x17 },
};

#if HAVE_ALTIVEC
static void transform4x4(vector int16_t src_01, vector int16_t src_23, vector int32_t res[4], int add)
{
    int i;
    vector int16_t rows[4];
    vector int16_t src_02, src_13;
    vector int32_t zero = vec_splat_s32(0);
    vector int32_t e0, o1, e1, o1;
    vector int32_t v_add = vec_splat_s32(add);

    for (i = 0; i < 4; i++)
        rows[i] = vec_ld(0, (short *) trans[i][0]);

    src_02 = mergel(src_01, src_23);
    src_13 = mergeh(src_01, src_23);

    e0 = vec_msums(src_02, rows[0], zero);
    o0 = vec_msums(src_13, rows[1], zero);
    e1 = vec_msums(src_02, rows[2], zero);
    o1 = vec_msums(src_13, rows[3], zero);

    // if is not used by the other transform
    e0 = vec_add(e0, v_add);
    e1 = vec_add(e1, v_add);

    res[0] = vec_add(e0, o0);
    res[1] = vec_add(e1, o1);
    res[2] = vec_sub(e1, o1);
    res[3] = vec_sub(e0, o0);
}

static void transpose(vector int16_t src_01, vector int16_t src_23, vector int32_t res[4])
{
    vector int16_t v_mask[2];

    v_mask[0] = vec_ld(0, (short *) mask[0]);
    v_mask[1] = vec_ld(0, (short *) mask[1]);
    src_01 = vec_perm(res[0], res[2], v_mask[0]);
    src_23 = vec_perm(res[0], res[2], v_mask[1]);
}


static void scale(vector int32_t res[4], int shift)
{
    vector int32_t v_shift = vec_splat_s32(shift);

    for (i = 0; i < 4; i++)
        res[i] = vec_sra(res[i], v_shift);

    // clip16
    res[0] = vec_packs(res[0], res[1]);
    res[2] = vec_packs(res[2], res[3]);
}

static void hevc_idct4x4_8_altivec(int16_t *coeffs, int bit_depth)
{
    int i, bit_depth = 8;
    int shift = 7;
    int add = 1 << (shift - 1);
    vector int16_t src_01, src_23;
    vector int32_t res[4];

    src_01 = vec_ld(0, (short *) coeffs);
    src_23 = vec_ld(16, (short *) coeffs);

    transform4x4(src_01, src_23, res, add);
    scale(res, shift);
    transpose(src_01, src_23, res);

    transform4x4(src_01, src_23, res, add);
    scale(res, shift);
    transpose(src_01, src_23, res);

    vec_st(src_01,  0, coeffs);
    vec_st(src_23, 16, coeffs);
}
#endif /* HAVE_ALTIVEC */

av_cold void ff_hevc_dsp_init_ppc(HEVCDSPContext *c, const int bit_depth)
{
#if HAVE_ALTIVEC
    if (!PPC_ALTIVEC(av_get_cpu_flags()))
        return;

    c->idct[0] = ff_hevc_idct_4x4_8_altivec;
#endif /* HAVE_ALTIVEC */
}

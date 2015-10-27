/*
 * Copyright (c) 2004 Gildas Bazin
 * Copyright (c) 2010 Mans Rullgard <mans@mansr.com>
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
 *
 * The functions idct_perform32_fixed, qmf_32_subbands_fixed, idct_perform64_fixed,
 * qmf_64_subbands_fixed, lfe_interpolation_fir_fixed and the auxiliary functions
 * they are using (mod*, sub*, clp*) are adapted from libdcadec,
 * https://github.com/foo86/dcadec/tree/master/libdcadec.
 */

#include <stdio.h>
#include "config.h"

#include "libavutil/attributes.h"
#include "libavutil/intreadwrite.h"

#include "dcadsp.h"
#include "dcamath.h"
#include "dcadata.h"

static void decode_hf_c(float dst[DCA_SUBBANDS][8],
                        const int32_t vq_num[DCA_SUBBANDS],
                        const int8_t hf_vq[1024][32], intptr_t vq_offset,
                        int32_t scale[DCA_SUBBANDS][2],
                        intptr_t start, intptr_t end)
{
    int i, l;

    for (l = start; l < end; l++) {
        /* 1 vector -> 32 samples but we only need the 8 samples
         * for this subsubframe. */
        const int8_t *ptr = &hf_vq[vq_num[l]][vq_offset];
        float fscale = scale[l][0] * (1 / 16.0);
        for (i = 0; i < 8; i++)
            dst[l][i] = ptr[i] * fscale;
    }
}

static inline void dca_lfe_fir(float *out, const float *in, const float *coefs,
                               int decifactor)
{
    float *out2    = out + 2 * decifactor - 1;
    int num_coeffs = 256 / decifactor;
    int j, k;

    /* One decimated sample generates 2*decifactor interpolated ones */
    for (k = 0; k < decifactor; k++) {
        float v0 = 0.0;
        float v1 = 0.0;
        for (j = 0; j < num_coeffs; j++, coefs++) {
            v0 += in[-j]                 * *coefs;
            v1 += in[j + 1 - num_coeffs] * *coefs;
        }
        *out++  = v0;
        *out2-- = v1;
        //printf("out %lf out2 %lf\n", v0, v1);
    }
}

static void dca_qmf_32_subbands(float samples_in[32][8], int sb_act,
                                SynthFilterContext *synth, FFTContext *imdct,
                                float synth_buf_ptr[512],
                                int *synth_buf_offset, float synth_buf2[32],
                                const float window[512], float *samples_out,
                                float raXin[32], float scale)
{
    int i;
    int subindex;

    for (i = sb_act; i < 32; i++)
        raXin[i] = 0.0;

    /* Reconstructed channel sample index */
    for (subindex = 0; subindex < 8; subindex++) {
        /* Load in one sample from each subband and clear inactive subbands */
        for (i = 0; i < sb_act; i++) {
            unsigned sign = (i - 1) & 2;
            uint32_t v    = AV_RN32A(&samples_in[i][subindex]) ^ sign << 30;
            AV_WN32A(&raXin[i], v);
        }

        synth->synth_filter_float(imdct, synth_buf_ptr, synth_buf_offset,
                                  synth_buf2, window, samples_out, raXin,
                                  scale);
        samples_out += 32;
    }
}

static void dca_lfe_fir0_c(float *out, const float *in, const float *coefs)
{
    dca_lfe_fir(out, in, coefs, 32);
}

static void dca_lfe_fir1_c(float *out, const float *in, const float *coefs)
{
    dca_lfe_fir(out, in, coefs, 64);
}

av_cold void ff_dcadsp_init(DCADSPContext *s)
{
    s->lfe_fir[0]      = dca_lfe_fir0_c;
    s->lfe_fir[1]      = dca_lfe_fir1_c;
    s->qmf_32_subbands = dca_qmf_32_subbands;
    s->decode_hf       = decode_hf_c;

    if (ARCH_ARM)
        ff_dcadsp_init_arm(s);
    if (ARCH_X86)
        ff_dcadsp_init_x86(s);
}

static void sum_a(const int * restrict input, int * restrict output, int len)
{
    int i;

    for (i = 0; i < len; i++)
        output[i] = input[2 * i] + input[2 * i + 1];
}

static void sum_b(const int * restrict input, int * restrict output, int len)
{
    int i;

    output[0] = input[0];
    for (i = 1; i < len; i++)
        output[i] = input[2 * i] + input[2 * i - 1];
}

static void sum_c(const int * restrict input, int * restrict output, int len)
{
    int i;

    for (i = 0; i < len; i++)
        output[i] = input[2 * i];
}

static void sum_d(const int * restrict input, int * restrict output, int len)
{
    int i;

    output[0] = input[1];
    for (i = 1; i < len; i++)
        output[i] = input[2 * i - 1] + input[2 * i + 1];
}

static void clp_v(int *input, int len)
{
    int i;

    for (i = 0; i < len; i++)
        input[i] = dca_clip23(input[i]);
}

static void dct_a(const int * restrict input, int * restrict output)
{
    int i, j;
    static const int cos_mod[8][8] = {
        { 8348215,  8027397,  7398092,  6484482,  5321677,  3954362,  2435084,   822227 },
        { 8027397,  5321677,   822227, -3954362, -7398092, -8348215, -6484482, -2435084 },
        { 7398092,   822227, -6484482, -8027397, -2435084,  5321677,  8348215,  3954362 },
        { 6484482, -3954362, -8027397,   822227,  8348215,  2435084, -7398092, -5321677 },
        { 5321677, -7398092, -2435084,  8348215,  -822227, -8027397,  3954362,  6484482 },
        { 3954362, -8348215,  5321677,  2435084, -8027397,  6484482,   822227, -7398092 },
        { 2435084, -6484482,  8348215, -7398092,  3954362,   822227, -5321677,  8027397 },
        {  822227, -2435084,  3954362, -5321677,  6484482, -7398092,  8027397, -8348215 }
    };

    for (i = 0; i < 8; i++) {
        int64_t res = INT64_C(0);
        for (j = 0; j < 8; j++)
            res += (int64_t)cos_mod[i][j] * input[j];
        output[i] = dca_norm(res, 23);
    }
}

static void dct_b(const int * restrict input, int * restrict output)
{
    int i, j;
    static const int cos_mod[8][7] = {
        {  8227423,  7750063,  6974873,  5931642,  4660461,  3210181,  1636536 },
        {  6974873,  3210181, -1636536, -5931642, -8227423, -7750063, -4660461 },
        {  4660461, -3210181, -8227423, -5931642,  1636536,  7750063,  6974873 },
        {  1636536, -7750063, -4660461,  5931642,  6974873, -3210181, -8227423 },
        { -1636536, -7750063,  4660461,  5931642, -6974873, -3210181,  8227423 },
        { -4660461, -3210181,  8227423, -5931642, -1636536,  7750063, -6974873 },
        { -6974873,  3210181,  1636536, -5931642,  8227423, -7750063,  4660461 },
        { -8227423,  7750063, -6974873,  5931642, -4660461,  3210181, -1636536 }
    };

    for (i = 0; i < 8; i++) {
        int64_t res = (int64_t)input[0] * (1 << 23);
        for (j = 0; j < 7; j++)
            res += (int64_t)cos_mod[i][j] * input[1 + j];
        output[i] = dca_norm(res, 23);
    }
}

static void mod_a(const int * restrict input, int * restrict output)
{
    int i, k;
    static const int cos_mod[16] = {
        4199362,   4240198,   4323885,   4454708,
        4639772,   4890013,   5221943,   5660703,
        -6245623,  -7040975,  -8158494,  -9809974,
        -12450076, -17261920, -28585092, -85479984
    };

    for (i = 0; i < 8; i++)
        output[i] = dca_norm((int64_t)cos_mod[i] * (input[i] + input[8 + i]), 23);

    for (i = 8, k = 7; i < 16; i++, k--)
        output[i] = dca_norm((int64_t)cos_mod[i] * (input[k] - input[8 + k]), 23);
}

static void mod_b(int * restrict input, int * restrict output)
{
    int i, k;
    static const int cos_mod[8] = {
        4214598,  4383036,  4755871,  5425934,
        6611520,  8897610, 14448934, 42791536
    };

    for (i = 0; i < 8; i++)
        input[8 + i] = dca_norm((int64_t)cos_mod[i] * input[8 + i], 23);

    for (i = 0; i < 8; i++)
        output[i] = input[i] + input[8 + i];

    for (i = 8, k = 7; i < 16; i++, k--)
        output[i] = input[k] - input[8 + k];
}

static void mod_c(const int * restrict input, int * restrict output)
{
    int i, k;
    static const int cos_mod[32] = {
        1048892,  1051425,   1056522,   1064244,
        1074689,  1087987,   1104313,   1123884,
        1146975,  1173922,   1205139,   1241133,
        1282529,  1330095,   1384791,   1447815,
        -1520688, -1605358,  -1704360,  -1821051,
        -1959964, -2127368,  -2332183,  -2587535,
        -2913561, -3342802,  -3931480,  -4785806,
        -6133390, -8566050, -14253820, -42727120
    };

    for (i = 0; i < 16; i++)
        output[i] = dca_norm((int64_t)cos_mod[i] * (input[i] + input[16 + i]), 23);

    for (i = 16, k = 15; i < 32; i++, k--)
        output[i] = dca_norm((int64_t)cos_mod[i] * (input[k] - input[16 + k]), 23);
}

void idct_perform32_fixed(int * restrict input, int * restrict output)
{
    int mag = 0;
    int shift, round;
    int i;

    for (i = 0; i < 32; i++)
        mag += abs(input[i]);

    shift = mag > 0x400000 ? 2 : 0;
    round = shift > 0 ? 1 << (shift - 1) : 0;

    for (i = 0; i < 32; i++)
        input[i] = (input[i] + round) >> shift;

    sum_a(input, output +  0, 16);
    sum_b(input, output + 16, 16);
    clp_v(output, 32);

    sum_a(output +  0, input +  0, 8);
    sum_b(output +  0, input +  8, 8);
    sum_c(output + 16, input + 16, 8);
    sum_d(output + 16, input + 24, 8);
    clp_v(input, 32);

    dct_a(input +  0, output +  0);
    dct_b(input +  8, output +  8);
    dct_b(input + 16, output + 16);
    dct_b(input + 24, output + 24);
    clp_v(output, 32);

    mod_a(output +  0, input +  0);
    mod_b(output + 16, input + 16);
    clp_v(input, 32);

    mod_c(input, output);

    for (i = 0; i < 32; i++)
        output[i] = dca_clip23(output[i] * (1 << shift));
}

void qmf_32_subbands_fixed(int subband_samples[32][8], int **subband_samples_hi, int *history,
                           int *pcm_samples, int nb_samples, int swich)
{
    const int32_t *filter_coeff;
    int input[32];
    int output[32];
    int sample;

    // Select filter
    if (!swich)
        filter_coeff = ff_dca_fir_32bands_nonperfect_fixed;
    else
        filter_coeff = ff_dca_fir_32bands_perfect_fixed;

    for (sample = 0; sample < nb_samples; sample++) {
        int i, j, k;

        // Load in one sample from each subband
        for (i = 0; i < 32; i++) {
            input[i] = subband_samples[i][sample];
        }

        // Inverse DCT
        idct_perform32_fixed(input, output);

        // Store history
        for (i = 0, k = 31; i < 16; i++, k--) {
            history[     i] = dca_clip23(output[i] - output[k]);
            history[16 + i] = dca_clip23(output[i] + output[k]);
        }

        // One subband sample generates 32 interpolated ones
        for (i = 0; i < 16; i++) {
            // Clear accumulation
            int64_t res = INT64_C(0);

            // Accumulate
            for (j = 32; j < 512; j += 64)
                res += (int64_t)history[16 + i + j] * filter_coeff[i + j];
            res = dca_round(res, 21);
            for (j =  0; j < 512; j += 64)
                res += (int64_t)history[     i + j] * filter_coeff[i + j];

            // Save interpolated samples
            pcm_samples[sample * 32 + i] = dca_clip23(dca_norm(res, 21)); // * (1.0f / (1 << 24));

        }

        for (i = 16, k = 15; i < 32; i++, k--) {
            // Clear accumulation
            int64_t res = INT64_C(0);

            // Accumulate
            for (j = 32; j < 512; j += 64)
                res += (int64_t)history[16 + k + j] * filter_coeff[i + j];
            res = dca_round(res, 21);
            for (j =  0; j < 512; j += 64)
                res += (int64_t)history[     k + j] * filter_coeff[i + j];

            // Save interpolated samples
            pcm_samples[sample * 32 + i] = dca_clip23(dca_norm(res, 21)); // * (1.0f / (1 << 24));
        }

        // Shift history
        for (i = 511; i >= 32; i--)
            history[i] = history[i - 32];
    }
}

static void mod64_a(const int * restrict input, int * restrict output)
{
    int i, k;
    static const int cos_mod[32] = {
        4195568,   4205700,   4226086,    4256977,
        4298755,   4351949,   4417251,    4495537,
        4587901,   4695690,   4820557,    4964534,
        5130115,   5320382,   5539164,    5791261,
        -6082752,  -6421430,  -6817439,   -7284203,
        -7839855,  -8509474,  -9328732,  -10350140,
        -11654242, -13371208, -15725922,  -19143224,
        -24533560, -34264200, -57015280, -170908480
    };

    for (i = 0; i < 16; i++)
        output[i] = dca_norm((int64_t)cos_mod[i] * (input[i] + input[16 + i]), 23);

    for (i = 16, k = 15; i < 32; i++, k--)
        output[i] = dca_norm((int64_t)cos_mod[i] * (input[k] - input[16 + k]), 23);
}

static void mod64_b(int * restrict input, int * restrict output)
{
    int i, k;
    static const int cos_mod[16] = {
        4199362,  4240198,  4323885,  4454708,
        4639772,  4890013,  5221943,  5660703,
        6245623,  7040975,  8158494,  9809974,
        12450076, 17261920, 28585092, 85479984
    };

    for (i = 0; i < 16; i++)
        input[16 + i] = dca_norm((int64_t)cos_mod[i] * input[16 + i], 23);

    for (i = 0; i < 16; i++)
        output[i] = input[i] + input[16 + i];

    for (i = 16, k = 15; i < 32; i++, k--)
        output[i] = input[k] - input[16 + k];
}

static void mod64_c(const int * restrict input, int * restrict output)
{
    int i, k;
    static const int cos_mod[64] = {
        741511,    741958,    742853,    744199,
        746001,    748262,    750992,    754197,
        757888,    762077,    766777,    772003,
        777772,    784105,    791021,    798546,
        806707,    815532,    825054,    835311,
        846342,    858193,    870912,    884554,
        899181,    914860,    931667,    949686,
        969011,    989747,   1012012,   1035941,
        -1061684,  -1089412,  -1119320,  -1151629,
        -1186595,  -1224511,  -1265719,  -1310613,
        -1359657,  -1413400,  -1472490,  -1537703,
        -1609974,  -1690442,  -1780506,  -1881904,
        -1996824,  -2128058,  -2279225,  -2455101,
        -2662128,  -2909200,  -3208956,  -3579983,
        -4050785,  -4667404,  -5509372,  -6726913,
        -8641940, -12091426, -20144284, -60420720
    };

    for (i = 0; i < 32; i++)
        output[i] = dca_norm((int64_t)cos_mod[i] * (input[i] + input[32 + i]), 23);

    for (i = 32, k = 31; i < 64; i++, k--)
        output[i] = dca_norm((int64_t)cos_mod[i] * (input[k] - input[32 + k]), 23);
}

void idct_perform64_fixed(int * restrict input, int * restrict output)
{
    int mag = 0;
    int shift;
    int round;
    int i;

    for (i = 0; i < 64; i++)
        mag += abs(input[i]);

    shift = mag > 0x400000 ? 2 : 0;
    round = shift > 0 ? 1 << (shift - 1) : 0;

    for (i = 0; i < 64; i++)
        input[i] = (input[i] + round) >> shift;

    sum_a(input, output +  0, 32);
    sum_b(input, output + 32, 32);
    clp_v(output, 64);

    sum_a(output +  0, input +  0, 16);
    sum_b(output +  0, input + 16, 16);
    sum_c(output + 32, input + 32, 16);
    sum_d(output + 32, input + 48, 16);
    clp_v(input, 64);

    sum_a(input +  0, output +  0, 8);
    sum_b(input +  0, output +  8, 8);
    sum_c(input + 16, output + 16, 8);
    sum_d(input + 16, output + 24, 8);
    sum_c(input + 32, output + 32, 8);
    sum_d(input + 32, output + 40, 8);
    sum_c(input + 48, output + 48, 8);
    sum_d(input + 48, output + 56, 8);
    clp_v(output, 64);

    dct_a(output +  0, input +  0);
    dct_b(output +  8, input +  8);
    dct_b(output + 16, input + 16);
    dct_b(output + 24, input + 24);
    dct_b(output + 32, input + 32);
    dct_b(output + 40, input + 40);
    dct_b(output + 48, input + 48);
    dct_b(output + 56, input + 56);
    clp_v(input, 64);

    mod_a(input +  0, output +  0);
    mod_b(input + 16, output + 16);
    mod_b(input + 32, output + 32);
    mod_b(input + 48, output + 48);
    clp_v(output, 64);

    mod64_a(output +  0, input +  0);
    mod64_b(output + 32, input + 32);
    clp_v(input, 64);

    mod64_c(input, output);

    for (i = 0; i < 64; i++)
        output[i] = dca_clip23(output[i] * (1 << shift));
}

void qmf_64_subbands_fixed(int subband_samples[64][8], int **subband_samples_hi, int *history,
                           int *pcm_samples, int nb_samples)
{
    int output[64];
    int sample;

    // Interpolation begins
    for (sample = 0; sample < nb_samples; sample++) {
        int i, j, k;

        // Load in one sample from each subband
        int input[64];
        if (subband_samples_hi) {
            // Full 64 subbands, first 32 are residual coded
            for (i =  0; i < 32; i++)
                input[i] = subband_samples[i][sample] + subband_samples_hi[i][sample];
            for (i = 32; i < 64; i++)
                input[i] = subband_samples_hi[i][sample];
        } else {
            // Only first 32 subbands
            for (i =  0; i < 32; i++)
                input[i] = subband_samples[i][sample];
            for (i = 32; i < 64; i++)
                input[i] = 0;
        }

        // Inverse DCT
        idct_perform64_fixed(input, output);

        // Store history
        for (i = 0, k = 63; i < 32; i++, k--) {
            history[     i] = dca_clip23(output[i] - output[k]);
            history[32 + i] = dca_clip23(output[i] + output[k]);
        }

        // One subband sample generates 64 interpolated ones
        for (i = 0; i < 32; i++) {
            // Clear accumulation
            int64_t res = INT64_C(0);

            // Accumulate
            for (j = 64; j < 1024; j += 128)
                res += (int64_t)history[32 + i + j] * ff_dca_band_fir_x96[i + j];
            res = dca_round(res, 20);
            for (j =  0; j < 1024; j += 128)
                res += (int64_t)history[     i + j] * ff_dca_band_fir_x96[i + j];

            // Save interpolated samples
            pcm_samples[sample * 64 + i] = dca_clip23(dca_norm(res, 20));
        }

        for (i = 32, k = 31; i < 64; i++, k--) {
            // Clear accumulation
            int64_t res = INT64_C(0);

            // Accumulate
            for (j = 64; j < 1024; j += 128)
                res += (int64_t)history[32 + k + j] * ff_dca_band_fir_x96[i + j];
            res = dca_round(res, 20);
            for (j =  0; j < 1024; j += 128)
                res += (int64_t)history[     k + j] * ff_dca_band_fir_x96[i + j];

            // Save interpolated samples
            pcm_samples[sample * 64 + i] = dca_clip23(dca_norm(res, 20));
        }

        // Shift history
        for (i = 1023; i >= 64; i--)
            history[i] = history[i - 64];
    }
}

void lfe_interpolation_fir_fixed(int *pcm_samples, int *lfe_samples,
                                 int nb_samples, int synth_x96)
{
    int dec_factor = 64;
    int i, j, k;

    // Interpolation
    for (i = 0; i < nb_samples; i++) {
        // One decimated sample generates 64 or 128 interpolated ones
        for (j = 0; j < dec_factor; j++) {
            // Clear accumulation
            int64_t res = INT64_C(0);

            // Accumulate
            for (k = 0; k < 512 / dec_factor; k++)
                res += (int64_t)ff_dca_lfe_fir_64_fixed[k * dec_factor + j] *
                        lfe_samples[i - k];

            // Save interpolated samples
            pcm_samples[(i * dec_factor + j) << synth_x96] = dca_clip23(dca_norm(res, 23));
            //printf("lfe chanptr  idx %d\n", (i * dec_factor + j) << synth_x96
            //printf("pcm_samples[(i * dec_factor + j] %d\n", pcm_samples[(i * dec_factor + j)]);
        }
    }
}

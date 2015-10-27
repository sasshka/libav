/*
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
 * qmf_64_subbands_fixed and the auxiliary functions they are using are adapted
 * from libdcadec, https://github.com/foo86/dcadec/tree/master/libdcadec.
 */

#ifndef AVCODEC_DCADSP_H
#define AVCODEC_DCADSP_H

#include "avfft.h"
#include "synth_filter.h"

#define DCA_SUBBANDS 32

typedef struct DCADSPContext {
    void (*lfe_fir[2])(void *out, const float *in, const float *coefs);
    void (*qmf_32_subbands)(float samples_in[32][8], int sb_act,
                            SynthFilterContext *synth, FFTContext *imdct,
                            float synth_buf_ptr[512],
                            int *synth_buf_offset, float synth_buf2[32],
                            const float window[512], float *samples_out,
                            float raXin[32], float scale);
    void (*decode_hf)(float dst[DCA_SUBBANDS][8],
                      const int32_t vq_num[DCA_SUBBANDS],
                      const int8_t hf_vq[1024][32], intptr_t vq_offset,
                      int32_t scale[DCA_SUBBANDS][2],
                      intptr_t start, intptr_t end);
} DCADSPContext;

void ff_dcadsp_init(DCADSPContext *s);
void ff_dcadsp_init_arm(DCADSPContext *s);
void ff_dcadsp_init_x86(DCADSPContext *s);

void idct_perform32_fixed(int * restrict input, int * restrict output);
void qmf_32_subbands_fixed(int subband_samples[32][8], int **subband_samples_hi,
                           int *history, int *pcm_samples, int nb_samples, int swich);
void idct_perform64_fixed(int * restrict input, int * restrict output);
void qmf_64_subbands_fixed(int subband_samples[64][8], int **subband_samples_hi,
                           int *history, int *pcm_samples, int nb_samples);
void lfe_interpolation_fir_fixed(int *pcm_samples, int *lfe_samples,
                                 int nb_samples, int synth_x96);

#endif /* AVCODEC_DCADSP_H */

/*
 * DCA ExSS extension
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

#include "libavutil/common.h"
#include "libavutil/log.h"

#include "bitstream.h"
#include "dca.h"
#include "dca_syncwords.h"

/* extensions that reside in core substream */
#define DCA_CORE_EXTS (DCA_EXT_XCH | DCA_EXT_XXCH | DCA_EXT_X96)

/* these are unconfirmed but should be mostly correct */
enum DCAExSSSpeakerMask {
    DCA_EXSS_FRONT_CENTER          = 0x0001,
    DCA_EXSS_FRONT_LEFT_RIGHT      = 0x0002,
    DCA_EXSS_SIDE_REAR_LEFT_RIGHT  = 0x0004,
    DCA_EXSS_LFE                   = 0x0008,
    DCA_EXSS_REAR_CENTER           = 0x0010,
    DCA_EXSS_FRONT_HIGH_LEFT_RIGHT = 0x0020,
    DCA_EXSS_REAR_LEFT_RIGHT       = 0x0040,
    DCA_EXSS_FRONT_HIGH_CENTER     = 0x0080,
    DCA_EXSS_OVERHEAD              = 0x0100,
    DCA_EXSS_CENTER_LEFT_RIGHT     = 0x0200,
    DCA_EXSS_WIDE_LEFT_RIGHT       = 0x0400,
    DCA_EXSS_SIDE_LEFT_RIGHT       = 0x0800,
    DCA_EXSS_LFE2                  = 0x1000,
    DCA_EXSS_SIDE_HIGH_LEFT_RIGHT  = 0x2000,
    DCA_EXSS_REAR_HIGH_CENTER      = 0x4000,
    DCA_EXSS_REAR_HIGH_LEFT_RIGHT  = 0x8000,
};

/**
 * Return the number of channels in an ExSS speaker mask (HD)
 */
static int dca_exss_mask2count(int mask)
{
    /* count bits that mean speaker pairs twice */
    return av_popcount(mask) +
           av_popcount(mask & (DCA_EXSS_CENTER_LEFT_RIGHT      |
                               DCA_EXSS_FRONT_LEFT_RIGHT       |
                               DCA_EXSS_FRONT_HIGH_LEFT_RIGHT  |
                               DCA_EXSS_WIDE_LEFT_RIGHT        |
                               DCA_EXSS_SIDE_LEFT_RIGHT        |
                               DCA_EXSS_SIDE_HIGH_LEFT_RIGHT   |
                               DCA_EXSS_SIDE_REAR_LEFT_RIGHT   |
                               DCA_EXSS_REAR_LEFT_RIGHT        |
                               DCA_EXSS_REAR_HIGH_LEFT_RIGHT));
}

/**
 * Skip mixing coefficients of a single mix out configuration (HD)
 */
static void dca_exss_skip_mix_coeffs(BitstreamContext *bc, int channels, int out_ch)
{
    int i;

    for (i = 0; i < channels; i++) {
        int mix_map_mask = bitstream_read(bc, out_ch);
        int num_coeffs = av_popcount(mix_map_mask);
        bitstream_skip(bc, num_coeffs * 6);
    }
}

/**
 * Parse extension substream asset header (HD)
 */
static int dca_exss_parse_asset_header(DCAContext *s)
{
    int header_pos = bitstream_tell(&s->bc);
    int header_size;
    int channels = 0;
    int embedded_stereo = 0;
    int embedded_6ch    = 0;
    int drc_code_present;
    int extensions_mask = 0;
    int i, j;

    if (bitstream_bits_left(&s->bc) < 16)
        return AVERROR_INVALIDDATA;

    /* We will parse just enough to get to the extensions bitmask with which
     * we can set the profile value. */

    header_size = bitstream_read(&s->bc, 9) + 1;
    bitstream_skip(&s->bc, 3); // asset index

    if (s->static_fields) {
        if (bitstream_read_bit(&s->bc))
            bitstream_skip(&s->bc, 4); // asset type descriptor
        if (bitstream_read_bit(&s->bc))
            bitstream_skip(&s->bc, 24); // language descriptor

        if (bitstream_read_bit(&s->bc)) {
            /* How can one fit 1024 bytes of text here if the maximum value
             * for the asset header size field above was 512 bytes? */
            int text_length = bitstream_read(&s->bc, 10) + 1;
            if (bitstream_bits_left(&s->bc) < text_length * 8)
                return AVERROR_INVALIDDATA;
            bitstream_skip(&s->bc, text_length * 8); // info text
        }

        bitstream_skip(&s->bc, 5); // bit resolution - 1
        bitstream_skip(&s->bc, 4); // max sample rate code
        channels = bitstream_read(&s->bc, 8) + 1;

        s->one2one_map_chtospkr = bitstream_read_bit(&s->bc);
        if (s->one2one_map_chtospkr) {
            int spkr_remap_sets;
            int spkr_mask_size = 16;
            int num_spkrs[7];

            if (channels > 2)
                embedded_stereo = bitstream_read_bit(&s->bc);
            if (channels > 6)
                embedded_6ch = bitstream_read_bit(&s->bc);

            if (bitstream_read_bit(&s->bc)) {
                spkr_mask_size = (bitstream_read(&s->bc, 2) + 1) << 2;
                bitstream_skip(&s->bc, spkr_mask_size); // spkr activity mask
            }

            spkr_remap_sets = bitstream_read(&s->bc, 3);

            for (i = 0; i < spkr_remap_sets; i++) {
                /* std layout mask for each remap set */
                num_spkrs[i] = dca_exss_mask2count(bitstream_read(&s->bc, spkr_mask_size));
            }

            for (i = 0; i < spkr_remap_sets; i++) {
                int num_dec_ch_remaps = bitstream_read(&s->bc, 5) + 1;
                if (bitstream_bits_left(&s->bc) < 0)
                    return AVERROR_INVALIDDATA;

                for (j = 0; j < num_spkrs[i]; j++) {
                    int remap_dec_ch_mask = bitstream_read(&s->bc, num_dec_ch_remaps);
                    int num_dec_ch = av_popcount(remap_dec_ch_mask);
                    bitstream_skip(&s->bc, num_dec_ch * 5); // remap codes
                }
            }
        } else {
            bitstream_skip(&s->bc, 3); // representation type
        }
    }

    drc_code_present = bitstream_read_bit(&s->bc);
    if (drc_code_present)
        bitstream_read(&s->bc, 8); // drc code

    if (bitstream_read_bit(&s->bc))
        bitstream_skip(&s->bc, 5); // dialog normalization code

    if (drc_code_present && embedded_stereo)
        bitstream_read(&s->bc, 8); // drc stereo code

    if (s->mix_metadata && bitstream_read_bit(&s->bc)) {
        bitstream_skip(&s->bc, 1); // external mix
        bitstream_skip(&s->bc, 6); // post mix gain code

        if (bitstream_read(&s->bc, 2) != 3) // mixer drc code
            bitstream_skip(&s->bc, 3); // drc limit
        else
            bitstream_skip(&s->bc, 8); // custom drc code

        if (bitstream_read_bit(&s->bc)) // channel specific scaling
            for (i = 0; i < s->num_mix_configs; i++)
                bitstream_skip(&s->bc, s->mix_config_num_ch[i] * 6); // scale codes
        else
            bitstream_skip(&s->bc, s->num_mix_configs * 6); // scale codes

        for (i = 0; i < s->num_mix_configs; i++) {
            if (bitstream_bits_left(&s->bc) < 0)
                return AVERROR_INVALIDDATA;
            dca_exss_skip_mix_coeffs(&s->bc, channels, s->mix_config_num_ch[i]);
            if (embedded_6ch)
                dca_exss_skip_mix_coeffs(&s->bc, 6, s->mix_config_num_ch[i]);
            if (embedded_stereo)
                dca_exss_skip_mix_coeffs(&s->bc, 2, s->mix_config_num_ch[i]);
        }
    }

    switch (bitstream_read(&s->bc, 2)) {
    case 0:
        extensions_mask = bitstream_read(&s->bc, 12);
        break;
    case 1:
        extensions_mask = DCA_EXT_EXSS_XLL;
        break;
    case 2:
        extensions_mask = DCA_EXT_EXSS_LBR;
        break;
    case 3:
        extensions_mask = 0; /* aux coding */
        break;
    }

    /* not parsed further, we were only interested in the extensions mask */

    if (bitstream_bits_left(&s->bc) < 0)
        return AVERROR_INVALIDDATA;

    if (bitstream_tell(&s->bc) - header_pos > header_size * 8) {
        av_log(s->avctx, AV_LOG_WARNING, "Asset header size mismatch.\n");
        return AVERROR_INVALIDDATA;
    }
    bitstream_skip(&s->bc, header_pos + header_size * 8 - bitstream_tell(&s->bc));

    if (extensions_mask & DCA_EXT_EXSS_XLL)
        s->profile = FF_PROFILE_DTS_HD_MA;
    else if (extensions_mask & (DCA_EXT_EXSS_XBR | DCA_EXT_EXSS_X96 |
                                DCA_EXT_EXSS_XXCH))
        s->profile = FF_PROFILE_DTS_HD_HRA;

    if (!(extensions_mask & DCA_EXT_CORE))
        av_log(s->avctx, AV_LOG_WARNING, "DTS core detection mismatch.\n");
    if ((extensions_mask & DCA_CORE_EXTS) != s->core_ext_mask)
        av_log(s->avctx, AV_LOG_WARNING,
               "DTS extensions detection mismatch (%d, %d)\n",
               extensions_mask & DCA_CORE_EXTS, s->core_ext_mask);

    return 0;
}

/**
 * Parse extension substream header (HD)
 */
void ff_dca_exss_parse_header(DCAContext *s)
{
    int asset_size[8];
    int ss_index;
    int blownup;
    int num_audiop = 1;
    int num_assets = 1;
    int active_ss_mask[8];
    int i, j;
    int start_pos;
    int hdrsize;
    uint32_t mkr;

    if (bitstream_bits_left(&s->bc) < 52)
        return;

    start_pos = bitstream_tell(&s->bc) - 32;

    bitstream_skip(&s->bc, 8); // user data
    ss_index = bitstream_read(&s->bc, 2);

    blownup = bitstream_read_bit(&s->bc);
    hdrsize = bitstream_read(&s->bc,  8 + 4 * blownup) + 1; // header_size
    bitstream_skip(&s->bc, 16 + 4 * blownup); // hd_size

    s->static_fields = bitstream_read_bit(&s->bc);
    if (s->static_fields) {
        bitstream_skip(&s->bc, 2); // reference clock code
        bitstream_skip(&s->bc, 3); // frame duration code

        if (bitstream_read_bit(&s->bc))
            bitstream_skip(&s->bc, 36); // timestamp

        /* a single stream can contain multiple audio assets that can be
         * combined to form multiple audio presentations */

        num_audiop = bitstream_read(&s->bc, 3) + 1;
        if (num_audiop > 1) {
            avpriv_request_sample(s->avctx,
                                  "Multiple DTS-HD audio presentations");
            /* ignore such streams for now */
            return;
        }

        num_assets = bitstream_read(&s->bc, 3) + 1;
        if (num_assets > 1) {
            avpriv_request_sample(s->avctx, "Multiple DTS-HD audio assets");
            /* ignore such streams for now */
            return;
        }

        for (i = 0; i < num_audiop; i++)
            active_ss_mask[i] = bitstream_read(&s->bc, ss_index + 1);

        for (i = 0; i < num_audiop; i++)
            for (j = 0; j <= ss_index; j++)
                if (active_ss_mask[i] & (1 << j))
                    bitstream_skip(&s->bc, 8); // active asset mask

        s->mix_metadata = bitstream_read_bit(&s->bc);
        if (s->mix_metadata) {
            int mix_out_mask_size;

            bitstream_skip(&s->bc, 2); // adjustment level
            mix_out_mask_size  = (bitstream_read(&s->bc, 2) + 1) << 2;
            s->num_mix_configs =  bitstream_read(&s->bc, 2) + 1;

            for (i = 0; i < s->num_mix_configs; i++) {
                int mix_out_mask        = bitstream_read(&s->bc, mix_out_mask_size);
                s->mix_config_num_ch[i] = dca_exss_mask2count(mix_out_mask);
            }
        }
    }

    for (i = 0; i < num_assets; i++)
        asset_size[i] = bitstream_read(&s->bc, 16 + 4 * blownup) + 1;

    for (i = 0; i < num_assets; i++) {
        if (dca_exss_parse_asset_header(s))
            return;
    }

    if (num_assets > 0) {
        j = bitstream_tell(&s->bc);
        if (start_pos + hdrsize * 8 > j)
            bitstream_skip(&s->bc, start_pos + hdrsize * 8 - j);

        for (i = 0; i < num_assets; i++) {
            int end_pos;
            start_pos = bitstream_tell(&s->bc);
            end_pos   = start_pos + asset_size[i] * 8;
            mkr       = bitstream_read(&s->bc, 32);

            /* parse extensions that we know about */
            switch (mkr) {
            case DCA_SYNCWORD_XLL:
                if (s->xll_disable) {
                    av_log(s->avctx, AV_LOG_DEBUG,
                           "DTS-XLL: ignoring XLL extension\n");
                    break;
                }
                av_log(s->avctx, AV_LOG_DEBUG,
                       "DTS-XLL: decoding XLL extension\n");
                if (ff_dca_xll_decode_header(s)        == 0 &&
                    ff_dca_xll_decode_navi(s, end_pos) == 0)
                    s->exss_ext_mask |= DCA_EXT_EXSS_XLL;
                break;
            case DCA_SYNCWORD_XBR:
            case DCA_SYNCWORD_XXCH:
            default:
                av_log(s->avctx, AV_LOG_VERBOSE,
                       "DTS-ExSS: unknown marker = 0x%08"PRIx32"\n", mkr);
            }

            /* skip to end of block */
            j = bitstream_tell(&s->bc);
            if (j > end_pos)
                av_log(s->avctx, AV_LOG_ERROR,
                       "DTS-ExSS: Processed asset too long.\n");
            if (j < end_pos)
                bitstream_skip(&s->bc, end_pos - j);
        }
    }
}

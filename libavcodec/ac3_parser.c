/*
 * AC-3 parser
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2003 Michael Niedermayer
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

#include "libavutil/channel_layout.h"
#include "bitstream.h"
#include "parser.h"
#include "ac3_parser.h"
#include "ac3_parser_internal.h"
#include "aac_ac3_parser.h"

#define AC3_HEADER_SIZE 7

#if CONFIG_AC3_PARSER

static const uint8_t eac3_blocks[4] = {
    1, 2, 3, 6
};

/**
 * Table for center mix levels
 * reference: Section 5.4.2.4 cmixlev
 */
static const uint8_t center_levels[4] = { 4, 5, 6, 5 };

/**
 * Table for surround mix levels
 * reference: Section 5.4.2.5 surmixlev
 */
static const uint8_t surround_levels[4] = { 4, 6, 7, 6 };


int ff_ac3_parse_header(BitstreamContext *bcp, AC3HeaderInfo *hdr)
{
    BitstreamContext bc = *bcp;
    int frame_size_code;

    memset(hdr, 0, sizeof(*hdr));

    hdr->sync_word = bitstream_read(&bc, 16);
    if(hdr->sync_word != 0x0B77)
        return AAC_AC3_PARSE_ERROR_SYNC;

    /* read ahead to bsid to distinguish between AC-3 and E-AC-3 */
    hdr->bitstream_id = bitstream_peek(&bc, 29) & 0x1F;
    if(hdr->bitstream_id > 16)
        return AAC_AC3_PARSE_ERROR_BSID;

    hdr->num_blocks = 6;

    /* set default mix levels */
    hdr->center_mix_level   = 5;  // -4.5dB
    hdr->surround_mix_level = 6;  // -6.0dB

    /* set default dolby surround mode */
    hdr->dolby_surround_mode = AC3_DSURMOD_NOTINDICATED;

    if(hdr->bitstream_id <= 10) {
        /* Normal AC-3 */
        // bitsream_prefetch(&bc, 29); 29 bits already present due _peek
        hdr->crc1    = bitstream_read_cache(&bc, 16);
        hdr->sr_code = bitstream_read_cache(&bc,  2);
        if(hdr->sr_code == 3)
            return AAC_AC3_PARSE_ERROR_SAMPLE_RATE;

        frame_size_code = bitstream_read_cache(&bc, 6);
        if(frame_size_code > 37)
            return AAC_AC3_PARSE_ERROR_FRAME_SIZE;

        bitstream_skip(&bc, 5); // skip bsid, already got it

        bitstream_prefetch(&bc, 11);
        hdr->bitstream_mode = bitstream_read_cache(&bc, 3);
        hdr->channel_mode   = bitstream_read_cache(&bc, 3);

        if(hdr->channel_mode == AC3_CHMODE_STEREO) {
            hdr->dolby_surround_mode = bitstream_read_cache(&bc, 2);
        } else {
            if((hdr->channel_mode & 1) && hdr->channel_mode != AC3_CHMODE_MONO)
                hdr->center_mix_level   = center_levels[bitstream_read_cache(&bc, 2)];
            if(hdr->channel_mode & 4)
                hdr->surround_mix_level = surround_levels[bitstream_read_cache(&bc, 2)];
        }
        hdr->lfe_on = bitstream_read_cache(&bc, 1);

        hdr->sr_shift = FFMAX(hdr->bitstream_id, 8) - 8;
        hdr->sample_rate = ff_ac3_sample_rate_tab[hdr->sr_code] >> hdr->sr_shift;
        hdr->bit_rate = (ff_ac3_bitrate_tab[frame_size_code>>1] * 1000) >> hdr->sr_shift;
        hdr->channels = ff_ac3_channels_tab[hdr->channel_mode] + hdr->lfe_on;
        hdr->frame_size = ff_ac3_frame_size_tab[frame_size_code][hdr->sr_code] * 2;
        hdr->frame_type = EAC3_FRAME_TYPE_AC3_CONVERT; //EAC3_FRAME_TYPE_INDEPENDENT;
        hdr->substreamid = 0;
    } else {
        /* Enhanced AC-3 */
        hdr->crc1 = 0;
        // bitsream_prefetch(&bc, 29); 29 bits already present due _peek
        hdr->frame_type = bitstream_read_cache(&bc, 2);
        if(hdr->frame_type == EAC3_FRAME_TYPE_RESERVED)
            return AAC_AC3_PARSE_ERROR_FRAME_TYPE;

        hdr->substreamid = bitstream_read_cache(&bc, 3);

        hdr->frame_size = (bitstream_read_cache(&bc, 11) + 1) << 1;
        if(hdr->frame_size < AC3_HEADER_SIZE)
            return AAC_AC3_PARSE_ERROR_FRAME_SIZE;

        hdr->sr_code = bitstream_read_cache(&bc, 2);
        if (hdr->sr_code == 3) {
            int sr_code2 = bitstream_read_cache(&bc, 2);
            if(sr_code2 == 3)
                return AAC_AC3_PARSE_ERROR_SAMPLE_RATE;
            hdr->sample_rate = ff_ac3_sample_rate_tab[sr_code2] / 2;
            hdr->sr_shift = 1;
        } else {
            hdr->num_blocks = eac3_blocks[bitstream_read_cache(&bc, 2)];
            hdr->sample_rate = ff_ac3_sample_rate_tab[hdr->sr_code];
            hdr->sr_shift = 0;
        }

        hdr->channel_mode = bitstream_read_cache(&bc, 3);
        hdr->lfe_on       = bitstream_read_cache(&bc, 1);

        hdr->bit_rate = (uint32_t)(8.0 * hdr->frame_size * hdr->sample_rate /
                        (hdr->num_blocks * 256.0));
        hdr->channels = ff_ac3_channels_tab[hdr->channel_mode] + hdr->lfe_on;
    }
    hdr->channel_layout = avpriv_ac3_channel_layout_tab[hdr->channel_mode];
    if (hdr->lfe_on)
        hdr->channel_layout |= AV_CH_LOW_FREQUENCY;

    *bcp = bc;

    return 0;
}

int av_ac3_parse_header(const uint8_t *buf, size_t size,
                        uint8_t *bitstream_id, uint16_t *frame_size)
{
    BitstreamContext bc;
    AC3HeaderInfo hdr;
    int err;

    bitstream_init8(&bc, buf, size);
    err = ff_ac3_parse_header(&bc, &hdr);
    if (err < 0)
        return AVERROR_INVALIDDATA;

    *bitstream_id = hdr.bitstream_id;
    *frame_size   = hdr.frame_size;

    return 0;
}

static int ac3_sync(uint64_t state, AACAC3ParseContext *hdr_info,
        int *need_next_header, int *new_frame_start)
{
    int err;
    union {
        uint64_t u64;
        uint8_t  u8[8 + AV_INPUT_BUFFER_PADDING_SIZE];
    } tmp = { av_be2ne64(state) };
    AC3HeaderInfo hdr;
    BitstreamContext bc;

    bitstream_init(&bc, tmp.u8 + 8 - AC3_HEADER_SIZE, 54);
    err = ff_ac3_parse_header(&bc, &hdr);

    if(err < 0)
        return 0;

    hdr_info->sample_rate = hdr.sample_rate;
    hdr_info->bit_rate = hdr.bit_rate;
    hdr_info->channels = hdr.channels;
    hdr_info->channel_layout = hdr.channel_layout;
    hdr_info->samples = hdr.num_blocks * 256;
    hdr_info->service_type = hdr.bitstream_mode;
    if (hdr.bitstream_mode == 0x7 && hdr.channels > 1)
        hdr_info->service_type = AV_AUDIO_SERVICE_TYPE_KARAOKE;
    if(hdr.bitstream_id>10)
        hdr_info->codec_id = AV_CODEC_ID_EAC3;
    else if (hdr_info->codec_id == AV_CODEC_ID_NONE)
        hdr_info->codec_id = AV_CODEC_ID_AC3;

    *need_next_header = (hdr.frame_type != EAC3_FRAME_TYPE_AC3_CONVERT);
    *new_frame_start  = (hdr.frame_type != EAC3_FRAME_TYPE_DEPENDENT);
    return hdr.frame_size;
}

static av_cold int ac3_parse_init(AVCodecParserContext *s1)
{
    AACAC3ParseContext *s = s1->priv_data;
    s->header_size = AC3_HEADER_SIZE;
    s->sync = ac3_sync;
    return 0;
}


AVCodecParser ff_ac3_parser = {
    .codec_ids      = { AV_CODEC_ID_AC3, AV_CODEC_ID_EAC3 },
    .priv_data_size = sizeof(AACAC3ParseContext),
    .parser_init    = ac3_parse_init,
    .parser_parse   = ff_aac_ac3_parse,
    .parser_close   = ff_parse_close,
};

#else

int av_ac3_parse_header(const uint8_t *buf, size_t size,
                        uint8_t *bitstream_id, uint16_t *frame_size)
{
    return AVERROR(ENOSYS);
}
#endif

/*
 * Audio and Video frame extraction
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2003 Michael Niedermayer
 * Copyright (c) 2009 Alex Converse
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

#include "aac_ac3_parser.h"
#include "aacadtsdec.h"
#include "bitstream.h"
#include "mpeg4audio.h"

int avpriv_aac_parse_header(BitstreamContext *bcc, AACADTSHeaderInfo *hdr)
{
    int size, rdb, ch, sr;
    int aot, crc_abs;

    if (bitstream_read(bcc, 12) != 0xfff)
        return AAC_AC3_PARSE_ERROR_SYNC;

    bitstream_skip(bcc, 1);            /* id */
    bitstream_skip(bcc, 2);            /* layer */
    crc_abs = bitstream_read_bit(bcc); /* protection_absent */
    aot     = bitstream_read(bcc, 2);  /* profile_objecttype */
    sr      = bitstream_read(bcc, 4);  /* sample_frequency_index */
    if (!avpriv_mpeg4audio_sample_rates[sr])
        return AAC_AC3_PARSE_ERROR_SAMPLE_RATE;
    bitstream_skip(bcc, 1);            /* private_bit */
    ch = bitstream_read(bcc, 3);       /* channel_configuration */

    bitstream_skip(bcc, 1);            /* original/copy */
    bitstream_skip(bcc, 1);            /* home */

    /* adts_variable_header */
    bitstream_skip(bcc, 1);             /* copyright_identification_bit */
    bitstream_skip(bcc, 1);             /* copyright_identification_start */
    size = bitstream_read(bcc, 13);     /* aac_frame_length */
    if (size < AAC_ADTS_HEADER_SIZE)
        return AAC_AC3_PARSE_ERROR_FRAME_SIZE;

    bitstream_skip(bcc, 11);            /* adts_buffer_fullness */
    rdb = bitstream_read(bcc, 2);       /* number_of_raw_data_blocks_in_frame */

    hdr->object_type    = aot + 1;
    hdr->chan_config    = ch;
    hdr->crc_absent     = crc_abs;
    hdr->num_aac_frames = rdb + 1;
    hdr->sampling_index = sr;
    hdr->sample_rate    = avpriv_mpeg4audio_sample_rates[sr];
    hdr->samples        = (rdb + 1) * 1024;
    hdr->bit_rate       = size * 8 * hdr->sample_rate / hdr->samples;

    return size;
}

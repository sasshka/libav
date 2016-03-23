/*
 * HEVC Supplementary Enhancement Information messages
 *
 * Copyright (C) 2012 - 2013 Guillaume Martres
 * Copyright (C) 2012 - 2013 Gildas Cocherel
 * Copyright (C) 2013 Vittorio Giovara
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

#include "bitstream.h"
#include "golomb.h"
#include "hevc.h"

enum HEVC_SEI_TYPE {
    SEI_TYPE_BUFFERING_PERIOD                     = 0,
    SEI_TYPE_PICTURE_TIMING                       = 1,
    SEI_TYPE_PAN_SCAN_RECT                        = 2,
    SEI_TYPE_FILLER_PAYLOAD                       = 3,
    SEI_TYPE_USER_DATA_REGISTERED_ITU_T_T35       = 4,
    SEI_TYPE_USER_DATA_UNREGISTERED               = 5,
    SEI_TYPE_RECOVERY_POINT                       = 6,
    SEI_TYPE_SCENE_INFO                           = 9,
    SEI_TYPE_FULL_FRAME_SNAPSHOT                  = 15,
    SEI_TYPE_PROGRESSIVE_REFINEMENT_SEGMENT_START = 16,
    SEI_TYPE_PROGRESSIVE_REFINEMENT_SEGMENT_END   = 17,
    SEI_TYPE_FILM_GRAIN_CHARACTERISTICS           = 19,
    SEI_TYPE_POST_FILTER_HINT                     = 22,
    SEI_TYPE_TONE_MAPPING_INFO                    = 23,
    SEI_TYPE_FRAME_PACKING                        = 45,
    SEI_TYPE_DISPLAY_ORIENTATION                  = 47,
    SEI_TYPE_SOP_DESCRIPTION                      = 128,
    SEI_TYPE_ACTIVE_PARAMETER_SETS                = 129,
    SEI_TYPE_DECODING_UNIT_INFO                   = 130,
    SEI_TYPE_TEMPORAL_LEVEL0_INDEX                = 131,
    SEI_TYPE_DECODED_PICTURE_HASH                 = 132,
    SEI_TYPE_SCALABLE_NESTING                     = 133,
    SEI_TYPE_REGION_REFRESH_INFO                  = 134,
    SEI_TYPE_MASTERING_DISPLAY_INFO               = 137,
    SEI_TYPE_CONTENT_LIGHT_LEVEL_INFO             = 144,
};

static int decode_nal_sei_decoded_picture_hash(HEVCContext *s)
{
    int cIdx, i;
    BitstreamContext *bc = &s->HEVClc.bc;
    uint8_t hash_type = bitstream_read(bc, 8);

    for (cIdx = 0; cIdx < 3; cIdx++) {
        if (hash_type == 0) {
            s->is_md5 = 1;
            for (i = 0; i < 16; i++)
                s->md5[cIdx][i] = bitstream_read(bc, 8);
        } else if (hash_type == 1) {
            // picture_crc = bitstream_read(bc, 16);
            bitstream_skip(bc, 16);
        } else if (hash_type == 2) {
            // picture_checksum = bitstream_read(bc, 32);
            bitstream_skip(bc, 32);
        }
    }
    return 0;
}

static int decode_nal_sei_frame_packing_arrangement(HEVCContext *s)
{
    BitstreamContext *bc = &s->HEVClc.bc;

    get_ue_golomb(bc);                  // frame_packing_arrangement_id
    s->sei_frame_packing_present = !bitstream_read_bit(bc);

    if (s->sei_frame_packing_present) {
        s->frame_packing_arrangement_type = bitstream_read(bc, 7);
        s->quincunx_subsampling           = bitstream_read_bit(bc);
        s->content_interpretation_type    = bitstream_read(bc, 6);

        // the following skips spatial_flipping_flag frame0_flipped_flag
        // field_views_flag current_frame_is_frame0_flag
        // frame0_self_contained_flag frame1_self_contained_flag
        bitstream_skip(bc, 6);

        if (!s->quincunx_subsampling && s->frame_packing_arrangement_type != 5)
            bitstream_skip(bc, 16);  // frame[01]_grid_position_[xy]
        bitstream_skip(bc, 8);       // frame_packing_arrangement_reserved_byte
        bitstream_skip(bc, 1);       // frame_packing_arrangement_persistance_flag
    }
    bitstream_skip(bc, 1);           // upsampled_aspect_ratio_flag
    return 0;
}

static int decode_nal_sei_display_orientation(HEVCContext *s)
{
    BitstreamContext *bc = &s->HEVClc.bc;

    s->sei_display_orientation_present = !bitstream_read_bit(bc);

    if (s->sei_display_orientation_present) {
        s->sei_hflip = bitstream_read_bit(bc);     // hor_flip
        s->sei_vflip = bitstream_read_bit(bc);     // ver_flip

        s->sei_anticlockwise_rotation = bitstream_read(bc, 16);
        bitstream_skip(bc, 1);     // display_orientation_persistence_flag
    }

    return 0;
}

static int decode_nal_sei_prefix(HEVCContext *s, int type, int size)
{
    BitstreamContext *bc = &s->HEVClc.bc;

    switch (type) {
    case 256:  // Mismatched value from HM 8.1
        return decode_nal_sei_decoded_picture_hash(s);
    case SEI_TYPE_FRAME_PACKING:
        return decode_nal_sei_frame_packing_arrangement(s);
    case SEI_TYPE_DISPLAY_ORIENTATION:
        return decode_nal_sei_display_orientation(s);
    default:
        av_log(s->avctx, AV_LOG_DEBUG, "Skipped PREFIX SEI %d\n", type);
        bitstream_skip(bc, 8 * size);
        return 0;
    }
}

static int decode_nal_sei_suffix(HEVCContext *s, int type, int size)
{
    BitstreamContext *bc = &s->HEVClc.bc;

    switch (type) {
    case SEI_TYPE_DECODED_PICTURE_HASH:
        return decode_nal_sei_decoded_picture_hash(s);
    default:
        av_log(s->avctx, AV_LOG_DEBUG, "Skipped SUFFIX SEI %d\n", type);
        bitstream_skip(bc, 8 * size);
        return 0;
    }
}

static int decode_nal_sei_message(HEVCContext *s)
{
    BitstreamContext *bc = &s->HEVClc.bc;

    int payload_type = 0;
    int payload_size = 0;
    int byte = 0xFF;
    av_log(s->avctx, AV_LOG_DEBUG, "Decoding SEI\n");

    while (byte == 0xFF) {
        byte          = bitstream_read(bc, 8);
        payload_type += byte;
    }
    byte = 0xFF;
    while (byte == 0xFF) {
        byte          = bitstream_read(bc, 8);
        payload_size += byte;
    }
    if (s->nal_unit_type == NAL_SEI_PREFIX) {
        return decode_nal_sei_prefix(s, payload_type, payload_size);
    } else { /* nal_unit_type == NAL_SEI_SUFFIX */
        return decode_nal_sei_suffix(s, payload_type, payload_size);
    }
    return 0;
}

static int more_rbsp_data(BitstreamContext *bc)
{
    return bitstream_bits_left(bc) > 0 && bitstream_peek(bc, 8) != 0x80;
}

int ff_hevc_decode_nal_sei(HEVCContext *s)
{
    do {
        decode_nal_sei_message(s);
    } while (more_rbsp_data(&s->HEVClc.bc));
    return 0;
}

/*
 * exp golomb vlc stuff
 * Copyright (c) 2003 Michael Niedermayer <michaelni@gmx.at>
 * Copyright (c) 2004 Alex Beregszaszi
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

/**
 * @file
 * @brief
 *     exp golomb vlc stuff
 * @author Michael Niedermayer <michaelni@gmx.at> and Alex Beregszaszi
 */

#ifndef AVCODEC_GOLOMB_H
#define AVCODEC_GOLOMB_H

#include <stdint.h>

#include "bitstream.h"
#include "put_bits.h"

#define INVALID_VLC           0x80000000

extern const uint8_t ff_golomb_vlc_len[512];
extern const uint8_t ff_ue_golomb_vlc_code[512];
extern const  int8_t ff_se_golomb_vlc_code[512];
extern const uint8_t ff_ue_golomb_len[256];

extern const uint8_t ff_interleaved_golomb_vlc_len[256];
extern const uint8_t ff_interleaved_ue_golomb_vlc_code[256];
extern const  int8_t ff_interleaved_se_golomb_vlc_code[256];
extern const uint8_t ff_interleaved_dirac_golomb_vlc_code[256];

/**
 * read unsigned exp golomb code.
 */
static inline int get_ue_golomb(BitstreamContext *bb)
{
    unsigned int buf;
    int ret;

    buf = bitstream_peek(bb, 9);

    if (buf >= (1 << 4)) {
        bitstream_skip(bb, ff_golomb_vlc_len[buf]);

        ret = ff_ue_golomb_vlc_code[buf];
        return ret;
    } else {
        int buf2 = bitstream_peek(bb, 32);
        int log = 2 * av_log2(buf2) - 31;
        buf2 >>= log;
        buf2--;
        bitstream_skip(bb, 32 - log);

        return buf2;
    }
}

/**
 * Read an unsigned Exp-Golomb code in the range 0 to UINT32_MAX-1.
 */
static inline unsigned get_ue_golomb_long(BitstreamContext *bb)
{
    unsigned buf, log;

    buf = bitstream_peek(bb, 32);
    log = 31 - av_log2(buf);
    bitstream_skip(bb, log);

    return bitstream_read(bb, log + 1) - 1;
}

/**
 * read unsigned exp golomb code, constraint to a max of 31.
 * the return value is undefined if the stored value exceeds 31.
 */
static inline int get_ue_golomb_31(BitstreamContext *bb)
{
    unsigned int buf;

    buf = bitstream_peek(bb, 32);

    buf >>= 32 - 9;
    bitstream_skip(bb, ff_golomb_vlc_len[buf]);

    return ff_ue_golomb_vlc_code[buf];
}

static inline unsigned svq3_get_ue_golomb(BitstreamContext *bb)
{
    uint32_t buf;

    buf = bitstream_peek(bb, 32);

    if (buf & 0xAA800000) {
        buf >>= 32 - 8;
        bitstream_skip(bb, ff_interleaved_golomb_vlc_len[buf]);

        return ff_interleaved_ue_golomb_vlc_code[buf];
    } else {
        unsigned ret = 1;

        do {
            buf >>= 32 - 8;
            bitstream_skip(bb, FFMIN(ff_interleaved_golomb_vlc_len[buf], 8));

            if (ff_interleaved_golomb_vlc_len[buf] != 9) {
                ret <<= (ff_interleaved_golomb_vlc_len[buf] - 1) >> 1;
                ret  |= ff_interleaved_dirac_golomb_vlc_code[buf];
                break;
            }
            ret = (ret << 4) | ff_interleaved_dirac_golomb_vlc_code[buf];
            buf = bitstream_peek(bb, 32);
        } while (bitstream_bits_left(bb) > 0);

        return ret - 1;
    }
}

/**
 * read unsigned truncated exp golomb code.
 */
static inline int get_te0_golomb(BitstreamContext *bb, int range)
{
    assert(range >= 1);

    if (range == 1)
        return 0;
    else if (range == 2)
        return bitstream_read_bit(bb) ^ 1;
    else
        return get_ue_golomb(bb);
}

/**
 * read unsigned truncated exp golomb code.
 */
static inline int get_te_golomb(BitstreamContext *bb, int range)
{
    assert(range >= 1);

    if (range == 2)
        return bitstream_read_bit(bb) ^ 1;
    else
        return get_ue_golomb(bb);
}

/**
 * read signed exp golomb code.
 */
static inline int get_se_golomb(BitstreamContext *bb)
{
    unsigned int buf;

    buf = bitstream_peek(bb, 9);

    if (buf >= (1 << 4)) {
        bitstream_skip(bb, ff_golomb_vlc_len[buf]);

        return ff_se_golomb_vlc_code[buf];
    } else {
        int buf2 = bitstream_peek(bb, 32);
        int log = 2 * av_log2(buf2) - 31;
        buf2 >>= log;

        bitstream_skip(bb, 32 - log);

        if (buf2 & 1)
            buf2 = -(buf2 >> 1);
        else
            buf2 = (buf2 >> 1);

        return buf2;
    }
}

static inline int get_se_golomb_long(BitstreamContext *bb)
{
    unsigned int buf = get_ue_golomb_long(bb);

    if (buf & 1)
        buf = (buf + 1) >> 1;
    else
        buf = -(buf >> 1);

    return buf;
}

static inline int svq3_get_se_golomb(BitstreamContext *bb)
{
    unsigned int buf;

    buf = bitstream_peek(bb, 32);

    if (buf & 0xAA800000) {
        buf >>= 32 - 8;
        bitstream_skip(bb, ff_interleaved_golomb_vlc_len[buf]);

        return ff_interleaved_se_golomb_vlc_code[buf];
    } else {
        int log;
        bitstream_skip(bb, 8);
        buf |= 1 | bitstream_peek(bb, 24);

        if ((buf & 0xAAAAAAAA) == 0)
            return INVALID_VLC;

        for (log = 31; (buf & 0x80000000) == 0; log--)
            buf = (buf << 2) - ((buf << log) >> (log - 1)) + (buf >> 30);

        bitstream_skip(bb, 63 - 2 * log - 8);

        return (signed) (((((buf << log) >> log) - 1) ^ -(buf & 0x1)) + 1) >> 1;
    }
}

static inline int dirac_get_se_golomb(BitstreamContext *bb)
{
    uint32_t ret = svq3_get_ue_golomb(bb);

    if (ret) {
        uint32_t buf;
        buf = bitstream_read_signed(bb, 1);
        ret = (ret ^ buf) - buf;
    }

    return ret;
}

/**
 * read unsigned golomb rice code (ffv1).
 */
static inline int get_ur_golomb(BitstreamContext *bb, int k, int limit,
                                int esc_len)
{
    unsigned int buf;
    int log;

    buf = bitstream_peek(bb, 32);

    log = av_log2(buf);

    if (log > 31 - limit) {
        buf >>= log - k;
        buf  += (30 - log) << k;
        bitstream_skip(bb, 32 + k - log);

        return buf;
    } else {
        bitstream_skip(bb, limit);
        buf = bitstream_read(bb, esc_len);

        return buf + limit - 1;
    }
}

/**
 * read unsigned golomb rice code (jpegls).
 */
static inline int get_ur_golomb_jpegls(BitstreamContext *bb, int k, int limit,
                                       int esc_len)
{
    unsigned int buf;
    int log;

    buf = bitstream_peek(bb, 32);

    log = av_log2(buf);

    if (log - k >= 1 && 32 - log < limit) {
        buf >>= log - k;
        buf  += (30 - log) << k;
        bitstream_skip(bb, 32 + k - log);

        return buf;
    } else {
        int i;
        for (i = 0; i < limit && bitstream_peek(bb, 1) == 0 && bitstream_bits_left(bb) > 0; i++) {
            bitstream_skip(bb, 1);
        }
        bitstream_skip(bb, 1);

        if (i < limit - 1) {
            if (k) {
                buf = bitstream_read(bb, k);
            } else {
                buf = 0;
            }

            return buf + (i << k);
        } else if (i == limit - 1) {
            buf = bitstream_read(bb, esc_len);

            return buf + 1;
        } else
            return -1;
    }
}

/**
 * read signed golomb rice code (ffv1).
 */
static inline int get_sr_golomb(BitstreamContext *bb, int k, int limit,
                                int esc_len)
{
    int v = get_ur_golomb(bb, k, limit, esc_len);

    v++;
    if (v & 1)
        return v >> 1;
    else
        return -(v >> 1);

//    return (v>>1) ^ -(v&1);
}

/**
 * read signed golomb rice code (flac).
 */
static inline int get_sr_golomb_flac(BitstreamContext *bb, int k, int limit,
                                     int esc_len)
{
    int v = get_ur_golomb_jpegls(bb, k, limit, esc_len);
    return (v >> 1) ^ -(v & 1);
}

/**
 * read unsigned golomb rice code (shorten).
 */
static inline unsigned int get_ur_golomb_shorten(BitstreamContext *bb, int k)
{
    return get_ur_golomb_jpegls(bb, k, INT_MAX, 0);
}

/**
 * read signed golomb rice code (shorten).
 */
static inline int get_sr_golomb_shorten(BitstreamContext *bb, int k)
{
    int uvar = get_ur_golomb_jpegls(bb, k + 1, INT_MAX, 0);
    if (uvar & 1)
        return ~(uvar >> 1);
    else
        return uvar >> 1;
}

#ifdef TRACE

static inline int get_ue(BitstreamContext *s, const char *file, const char *func,
                         int line)
{
    int show = bitstream_peek(s, 24);
    int pos  = bitstream_tell(s);
    int i    = get_ue_golomb(s);
    int len  = bitstream_tell(s) - pos;
    int bits = show >> (24 - len);

    av_log(NULL, AV_LOG_DEBUG, "%5d %2d %3d ue  @%5d in %s %s:%d\n",
           bits, len, i, pos, file, func, line);

    return i;
}

static inline int get_se(BitstreamContext *s, const char *file, const char *func,
                         int line)
{
    int show = bitstream_peek(s, 24);
    int pos  = bitstream_tell(s);
    int i    = get_se_golomb(s);
    int len  = bitstream_tell(s) - pos;
    int bits = show >> (24 - len);

    av_log(NULL, AV_LOG_DEBUG, "%5d %2d %3d se  @%5d in %s %s:%d\n",
           bits, len, i, pos, file, func, line);

    return i;
}

static inline int get_te(BitstreamContext *s, int r, char *file, const char *func,
                         int line)
{
    int show = bitstream_peek(s, 24);
    int pos  = bitstream_tell(s);
    int i    = get_te0_golomb(s, r);
    int len  = bitstream_tell(s) - pos;
    int bits = show >> (24 - len);

    av_log(NULL, AV_LOG_DEBUG, "%5d %2d %3d te  @%5d in %s %s:%d\n",
           bits, len, i, pos, file, func, line);

    return i;
}

#define get_ue_golomb(a) get_ue(a, __FILE__, __PRETTY_FUNCTION__, __LINE__)
#define get_se_golomb(a) get_se(a, __FILE__, __PRETTY_FUNCTION__, __LINE__)
#define get_te_golomb(a, r)  get_te(a, r, __FILE__, __PRETTY_FUNCTION__, __LINE__)
#define get_te0_golomb(a, r) get_te(a, r, __FILE__, __PRETTY_FUNCTION__, __LINE__)

#endif /* TRACE */

/**
 * write unsigned exp golomb code.
 */
static inline void set_ue_golomb(PutBitContext *pb, int i)
{
    assert(i >= 0);

#if 0
    if (i = 0) {
        put_bits(pb, 1, 1);
        return;
    }
#endif
    if (i < 256)
        put_bits(pb, ff_ue_golomb_len[i], i + 1);
    else {
        int e = av_log2(i + 1);
        put_bits(pb, 2 * e + 1, i + 1);
    }
}

/**
 * write truncated unsigned exp golomb code.
 */
static inline void set_te_golomb(PutBitContext *pb, int i, int range)
{
    assert(range >= 1);
    assert(i <= range);

    if (range == 2)
        put_bits(pb, 1, i ^ 1);
    else
        set_ue_golomb(pb, i);
}

/**
 * write signed exp golomb code. 16 bits at most.
 */
static inline void set_se_golomb(PutBitContext *pb, int i)
{
#if 0
    if (i <= 0)
        i = -2 * i;
    else
        i = 2 * i - 1;
#elif 1
    i = 2 * i - 1;
    if (i < 0)
        i ^= -1;    //FIXME check if gcc does the right thing
#else
    i  = 2 * i - 1;
    i ^= (i >> 31);
#endif
    set_ue_golomb(pb, i);
}

/**
 * write unsigned golomb rice code (ffv1).
 */
static inline void set_ur_golomb(PutBitContext *pb, int i, int k, int limit,
                                 int esc_len)
{
    int e;

    assert(i >= 0);

    e = i >> k;
    if (e < limit)
        put_bits(pb, e + k + 1, (1 << k) + (i & ((1 << k) - 1)));
    else
        put_bits(pb, limit + esc_len, i - limit + 1);
}

/**
 * write unsigned golomb rice code (jpegls).
 */
static inline void set_ur_golomb_jpegls(PutBitContext *pb, int i, int k,
                                        int limit, int esc_len)
{
    int e;

    assert(i >= 0);

    e = (i >> k) + 1;
    if (e < limit) {
        while (e > 31) {
            put_bits(pb, 31, 0);
            e -= 31;
        }
        put_bits(pb, e, 1);
        if (k)
            put_sbits(pb, k, i);
    } else {
        while (limit > 31) {
            put_bits(pb, 31, 0);
            limit -= 31;
        }
        put_bits(pb, limit, 1);
        put_bits(pb, esc_len, i - 1);
    }
}

/**
 * write signed golomb rice code (ffv1).
 */
static inline void set_sr_golomb(PutBitContext *pb, int i, int k, int limit,
                                 int esc_len)
{
    int v;

    v  = -2 * i - 1;
    v ^= (v >> 31);

    set_ur_golomb(pb, v, k, limit, esc_len);
}

/**
 * write signed golomb rice code (flac).
 */
static inline void set_sr_golomb_flac(PutBitContext *pb, int i, int k,
                                      int limit, int esc_len)
{
    int v;

    v  = -2 * i - 1;
    v ^= (v >> 31);

    set_ur_golomb_jpegls(pb, v, k, limit, esc_len);
}

#endif /* AVCODEC_GOLOMB_H */

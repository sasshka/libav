/*
 * copyright (c) 2016 Alexandra Hájková
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
 * bitstream reader API header.
 */

#ifndef AVCODEC_BITSTREAM_H
#define AVCODEC_BITSTREAM_H

#include <stdint.h>

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"

#include "mathops.h"
#include "vlc.h"

typedef struct BitstreamContext {
    uint64_t bits;      // stores bits read from the buffer
    const uint8_t *buffer, *buffer_end;
    const uint8_t *ptr; // pointer to the position inside a buffer
    unsigned bits_left; // number of bits left in bits field
    unsigned size_in_bits;
} BitstreamContext;

/**
 * Return number of bits already read.
 */
static inline int bitstream_tell(const BitstreamContext *s)
{
    return (s->ptr - s->buffer) * 8 - s->bits_left;
}

static inline void refill_64(BitstreamContext *s)
{
    if (av_unlikely(s->ptr >= s->buffer_end || !s->buffer))
        return;

#ifdef BITSTREAM_READER_LE
    s->bits      = AV_RL64(s->ptr);
#else
    s->bits      = AV_RB64(s->ptr);
#endif
    s->ptr      += 8;
    s->bits_left = 64;
}

static inline uint64_t get_val(BitstreamContext *s, unsigned int n)
{
    uint64_t ret;

#ifdef BITSTREAM_READER_LE
    ret = s->bits & ((UINT64_C(1) << n) - 1);
    s->bits >>= n;
#else
    ret = s->bits >> (64 - n);
    s->bits <<= n;
#endif
    s->bits_left -= n;

    return ret;
}

/**
 * Return one bit from the buffer.
 */
static inline unsigned int bitstream_read_bit(BitstreamContext *s)
{
    if (av_unlikely(!s->bits_left))
        refill_64(s);

    return get_val(s, 1);
}

/**
 * Return n bits from the buffer, n has to be in the 0-63 range.
 */
static inline uint64_t bitstream_read_63(BitstreamContext *s, unsigned int n)
{
    uint64_t ret = 0;
#ifdef BITSTREAM_READER_LE
    uint64_t left = 0;
#endif

    if (av_unlikely(!n))
        return 0;

    if (av_unlikely(n > s->bits_left)) {
        n -= s->bits_left;
#ifdef BITSTREAM_READER_LE
        left = s->bits_left;
#endif
        ret = get_val(s, s->bits_left);
        refill_64(s);
    }

#ifdef BITSTREAM_READER_LE
    ret = get_val(s, n) << left | ret;
#else
    ret = get_val(s, n) | ret << n;
#endif

    return ret;
}

static inline void refill_32(BitstreamContext *s)
{
    if (av_unlikely(s->ptr >= s->buffer_end || !s->buffer))
        return;

#ifdef BITSTREAM_READER_LE
    s->bits       = (uint64_t)AV_RL32(s->ptr) << s->bits_left | s->bits;
#else
    s->bits       = s->bits | (uint64_t)AV_RB32(s->ptr) << (32 - s->bits_left);
#endif
    s->ptr       += 4;
    s->bits_left += 32;
}

/**
 * Return n bits from the buffer, n has to be in the 0-32  range.
 */
static inline uint32_t bitstream_read(BitstreamContext *s, unsigned int n)
{
    if (av_unlikely(!n))
        return 0;

    if (av_unlikely(n > s->bits_left)) {
        refill_32(s);
        if (av_unlikely(s->bits_left < 32))
            s->bits_left = n;
    }

    return get_val(s, n);
}

static inline unsigned int show_val(BitstreamContext *s, unsigned int n)
{
    int ret;

#ifdef BITSTREAM_READER_LE
    ret = s->bits & ((UINT64_C(1) << n) - 1);
#else
    ret = s->bits >> (64 - n);
#endif

    return ret;
}

/**
 * Return n bits from the buffer but do not change the buffer state.
 * n has to be in the 0-32 range.
 */
static inline unsigned int bitstream_peek(BitstreamContext *s, unsigned int n)
{
    if (av_unlikely(n > s->bits_left))
        refill_32(s);

    return show_val(s, n);
}

static inline void skip_remaining(BitstreamContext *s, unsigned int n)
{
#ifdef BITSTREAM_READER_LE
    s->bits >>= n;
#else
    s->bits <<= n;
#endif
    s->bits_left -= n;
}

/**
 * Skip n bits in the buffer.
 */
static inline void bitstream_skip(BitstreamContext *s, unsigned int n)
{
    if (n <= s->bits_left)
        skip_remaining(s, n);
    else {
        n -= s->bits_left;
        skip_remaining(s, s->bits_left);
        if (n >= 64) {
            int skip = n / 8;

            n -= skip * 8;
            s->ptr += skip;
        }
        refill_64(s);
        if (n)
            skip_remaining(s, n);
    }
}

/**
 * Read MPEG-1 dc-style VLC (sign bit + mantissa with no MSB).
 * If MSB not set it is negative.
 * @param n length in bits
 */
static inline int bitstream_read_xbits(BitstreamContext *s, unsigned int n)
{
    int sign;
    int32_t cache;

    cache = bitstream_peek(s, 32);
    sign = ~cache >> 31;
    bitstream_skip(s, n);

    return ((((uint32_t)(sign ^ cache)) >> (32 - n)) ^ sign) - sign;
}

/**
 * Return n bits from the buffer as a signed integer.
 * n has to be in the 0-32 range.
 */
static inline int32_t bitstream_read_signed(BitstreamContext *s, unsigned int n)
{
    return sign_extend(bitstream_read(s, n), n);
}

/**
 * Return n bits from the buffer as a signed integer,
 * do not change the buffer state.
 * n has to be in the 0-32 range.
 */
static inline int bitstream_peek_signed(BitstreamContext *s, unsigned int n)
{
    return sign_extend(bitstream_peek(s, n), n);
}

/**
 * Seek to the given bit position.
 */
static inline void bitstream_seek(BitstreamContext *s, unsigned pos)
{
    s->ptr       = s->buffer;
    s->bits      = 0;
    s->bits_left = 0;

    bitstream_skip(s, pos);
}

/**
 * Initialize BitstreamContext.
 * @param buffer bitstream buffer, must be AV_INPUT_BUFFER_PADDING_SIZE bytes
 *        larger than the actual read bits because some optimized bitstream
 *        readers read 32 or 64 bits at once and could read over the end
 * @param bit_size the size of the buffer in bits
 * @return 0 on success, AVERROR_INVALIDDATA if the buffer_size would overflow.
 */
static inline int bitstream_init(BitstreamContext *s, const uint8_t *buffer, unsigned int bit_size)
{
    int buffer_size;
    int ret = 0;

    if (bit_size > INT_MAX - 7 || !buffer) {
        buffer = s->buffer = s->ptr = NULL;
        s->bits_left = 0;
        return AVERROR_INVALIDDATA;
    }

    buffer_size     = (bit_size + 7) >> 3;

    s->buffer       = buffer;
    s->buffer_end   = buffer + buffer_size;
    s->ptr          = s->buffer;
    s->size_in_bits = bit_size;
    s->bits_left    = 0;
    s->bits         = 0;

    refill_64(s);

    return ret;
}

/**
 * Return buffer size in bits.
 */
static inline int bitstream_tell_size(BitstreamContext *s)
{
    return s->size_in_bits;
}

/**
 * Initialize BitstreamContext.
 * @param buffer bitstream buffer, must be AV_INPUT_BUFFER_PADDING_SIZE bytes
 *        larger than the actual read bits because some optimized bitstream
 *        readers read 32 or 64 bits at once and could read over the end
 * @param byte_size the size of the buffer in bytes
 * @return 0 on success, AVERROR_INVALIDDATA if the buffer_size would overflow
 */
static inline int bitstream_init8(BitstreamContext *s, const uint8_t *buffer,
                                  unsigned int byte_size)
{
    if (byte_size > INT_MAX / 8)
        return AVERROR_INVALIDDATA;
    return bitstream_init(s, buffer, byte_size * 8);
}

/**
 * Skip bits to a byte boundary.
 */
static inline const uint8_t *bitstream_align(BitstreamContext *s)
{
    unsigned int n = -bitstream_tell(s) & 7;
    if (n)
        bitstream_skip(s, n);
    return s->buffer + (bitstream_tell(s) >> 3);
}

/**
 * Return the LUT element for the given bitstream configuration.
 */
static inline int set_idx(BitstreamContext *s, int code, int *n, int *nb_bits, VLC_TYPE (*table)[2])
{
    unsigned int idx;

    *nb_bits = -*n;
    idx = bitstream_peek(s, *nb_bits) + code;
    *n = table[idx][1];

    return table[idx][0];
}

/**
 * Parse a vlc code.
 * @param bits is the number of bits which will be read at once, must be
 *             identical to nb_bits in init_vlc()
 * @param max_depth is the number of times bits bits must be read to completely
 *                  read the longest vlc code
 *                  = (max_vlc_length + bits - 1) / bits
 * If the vlc code is invalid and max_depth=1, then no bits will be removed.
 * If the vlc code is invalid and max_depth>1, then the number of bits removed
 * is undefined.
 */
static av_always_inline int bitstream_read_vlc(BitstreamContext *s, VLC_TYPE (*table)[2],
                                               int bits, int max_depth)
{
    int nb_bits;
    unsigned idx = bitstream_peek(s, bits);
    int code     = table[idx][0];
    int n        = table[idx][1];

    if (max_depth > 1 && n < 0) {
        bitstream_skip(s, bits);
        code = set_idx(s, code, &n, &nb_bits, table);
        if (max_depth > 2 && n < 0) {
            bitstream_skip(s, nb_bits);
            code = set_idx(s, code, &n, &nb_bits, table);
        }
    }
    bitstream_skip(s, n);

    return code;
}

#define BITSTREAM_RL_VLC(level, run, bb, table, bits, max_depth) \
    do {                                                         \
        int n, nb_bits;                                          \
        unsigned int index;                                      \
                                                                 \
        index = bitstream_peek(bb, bits);                        \
        level = table[index].level;                              \
        n     = table[index].len;                                \
                                                                 \
        if (max_depth > 1 && n < 0) {                            \
            bitstream_skip(bb, bits);                            \
                                                                 \
            nb_bits = -n;                                        \
                                                                 \
            index = bitstream_peek(bb, nb_bits) + level;         \
            level = table[index].level;                          \
            n     = table[index].len;                            \
            if (max_depth > 2 && n < 0) {                        \
                bitstream_skip(bb, nb_bits);                     \
                nb_bits = -n;                                    \
                                                                 \
                index = bitstream_peek(bb, nb_bits) + level;     \
                level = table[index].level;                      \
                n     = table[index].len;                        \
            }                                                    \
        }                                                        \
        run = table[index].run;                                  \
        bitstream_skip(bb, n);                                   \
    } while (0)

/**
 * Return decoded truncated unary code for the values 0, 1, 2.
 */
static inline int bitstream_decode012(BitstreamContext *bb)
{
    if (!bitstream_read_bit(bb))
        return 0;
    else
        return bitstream_read_bit(bb) + 1;
}

/**
 * Return decoded truncated unary code for the values 2, 1, 0.
 */
static inline int bitstream_decode210(BitstreamContext *bb)
{
    if (bitstream_read_bit(bb))
        return 0;
    else
        return 2 - bitstream_read_bit(bb);
}

/**
 * Return the number of the bits left in a buffer.
 */
static inline int bitstream_bits_left(BitstreamContext *bb)
{
    int ret;

    ret = (bb->buffer - bb->ptr) * 8 + bb->size_in_bits + bb->bits_left;
    return ret;
}

#endif /* AVCODEC_BITSTREAM_H */

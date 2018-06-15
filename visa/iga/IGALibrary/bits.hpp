/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

#ifndef IGA_BITS_HPP
#define IGA_BITS_HPP

#include <cstdint>

namespace iga
{
    // macros are for initializers
    //   other users should use the functions where possible
    // TODO: someday constexpr will make this obsolete, but we want to avoid
    //       static constructors (initializers) in large tables
#define BITFIELD_MASK32_UNSHIFTED(OFF,LEN) \
    ((LEN) == 32 ? 0xFFFFFFFF : ((1<<(LEN))-1))
#define BITFIELD_MASK32(OFF,LEN) \
    (BITFIELD_MASK32_UNSHIFTED(OFF,LEN) << (OFF))

    template <typename T>
    static T getFieldMaskUnshifted(int len)
    {
        return len == 8*sizeof(T) ? ((T)-1) : ((T)1 << len) - 1;
    }
    template <typename T>
    static T getFieldMask(int off, int len)
    {
        return getFieldMaskUnshifted<T>(len) << (off % (8*sizeof(T)));
    }
    // static uint64_t getFieldMask(int off, int len)
    // {
    //     uint64_t mask = len == 64 ?
    //         0xFFFFFFFFFFFFFFFFull : ((1ull << len) - 1);
    //     return mask << off % 64;
    // }
    template <typename T>
    static T getBits(T bits, int off, int len) {
        T mask = len == 8*sizeof(T) ? ((T)-1) : (1ull << len) - 1;
        return ((bits >> off) & mask);
    }
    template <typename T>
    static bool testBit(T bits, int off) {
        return getBits<T>(bits, off, 1) != 0;
    }
    template <typename T>
    static T getSignedBits(T bits, int off, int len) {
        T mask = len == 8*sizeof(T) ? ((T)-1) : (1ull << len) - 1;
        T val = ((bits >> off) & mask);
        if (val & ((T)1 << (len - 1))) {
            // sign extend high bit if set
            val |= ~getFieldMaskUnshifted<T>(len);
        }
        return val;
    }
    template <typename T>
    static T getBits(const T *bits, int off, int len)
    {
        T w = (off < 8*sizeof(T)) ? *bits : *(bits+1);
        return getBits<T>(w, off % (8*sizeof(T)), len);
    }
    template <typename T>
    static T getSignedBits(const T *bits, int off, int len)
    {
        T w = (off < 8*sizeof(T)) ? *bits : *(bits+1);
        return getSignedBits<T>(w, off % (8*sizeof(T)), len);
    }
    static uint64_t getBits(const void *bits, int off, int len)
    {
        return getBits((const uint64_t *)bits, off, len);
    }
    static int64_t getSignedBits(const void *bits, int off, int len)
    {
        return getSignedBits((const uint64_t *)bits, off, len);
    }
    static bool setBits(uint64_t *qws, int off, int len, uint64_t val) {
        uint64_t shiftedVal = val << off % 64; // shift into position
        uint64_t mask = getFieldMask<uint64_t>(off, len);
        if (shiftedVal & ~mask) {
            // either val has bits greater than what this field represents
            // or below the shift (e.g. smaller value than we accept)
            // e.g. Dst.Subreg[4:3] just has the high two bits of
            // the subregister and can't represent small values.
            //
            // Generally, this indicates an internal IGA problem:
            //  e.g. something we need to catch in the parser, IR checker or
            //       some other higher level (i.e. we're looking at bad IR)
            return false;
        }
        qws[off / 64] |= shiftedVal;
        return true;
    }

    template <typename T>
    static bool setBits(T &bits, int off, int len, T val) {
        T mask = getFieldMaskUnshifted<T>(len);
        if (val > mask)
            return false;
        bits = (bits & ~(mask<<off)) | (val << off);
        return true;
    }

    // finds the high bit index set
    // returns -1 given 0 (i.e. if no bits set)
    //
    // STL really needs this
    // gcc has __builtin_clzll, but let's ignore the #ifdef nonsense
    static int findLeadingOne(uint64_t v) {
        static const uint64_t MASKS[] {
            0xFFFFFFFF00000000ull,
            0xFFFF0000,
            0xFF00,
            0xF0,
            0xC,
            0x2
        };

        // checks the top 32 (add 32 if it's there), also shift the bottom
        // check the top 16 of that result
        // ...
        // the mask could also be generated, but we expect it to unroll
        int index = 0;
        for (int i = 0, offset = 32;
            i < sizeof(MASKS)/sizeof(MASKS[0]);
            i++, offset>>=1)
        {
            if (v & MASKS[i]) {
                v >>= offset;
                index += offset;
            }
        }

        return index;
    }

} // namespace iga
#endif /* IGA_BITS_HPP */
/*========================== begin_copyright_notice ============================

Copyright (c) 2020-2021 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

============================= end_copyright_notice ===========================*/

#ifndef _IGA_BACKEND_MESSAGES_MESSAGEDECODER_HPP_
#define _IGA_BACKEND_MESSAGES_MESSAGEDECODER_HPP_

#include "../../IR/Messages.hpp"
#include "../../Frontend/IRToString.hpp"
#include "../../asserts.hpp"
#include "../Native/Field.hpp"

#include <algorithm>
#include <functional>
#include <sstream>
#include <tuple>
#include <vector>

namespace iga {
    using DescFieldFormatter =
        std::function<void (std::stringstream &,uint32_t)>;
    static inline void NO_DECODE(std::stringstream &,uint32_t) { }


    struct MessageDecoder {
        // inputs
        const Platform         platf;
        const SFID             sfid;
        const SendDesc         desc, exDesc;
        const RegRef           indDesc;

        // outputs
        DecodeResult          &result;

        const int DEFAULT_EXEC_SIZE, BITS_PER_REGISTER;

        MessageDecoder(
            Platform _platform,
            SFID _sfid,
            SendDesc _exDesc,
            SendDesc _desc,
            RegRef _indDesc,
            DecodeResult &_result)
            : platf(_platform)
            , sfid(_sfid)
            //
            , desc(_desc)
            , exDesc(_exDesc)
            , indDesc(_indDesc)
            //
            , result(_result)
            //
            , DEFAULT_EXEC_SIZE(16)
            , BITS_PER_REGISTER(256)
        {
            result.info.op = SendOp::INVALID;
            result.info.cachingL3 = result.info.cachingL1 = CacheOpt::DEFAULT;
            result.info.elemSizeBitsRegFile = result.info.elemSizeBitsMemory = 0;
            result.info.channelsEnabled = result.info.elemsPerAddr = 0;
            result.info.execWidth = 0;
            result.info.attributeSet = 0;
            result.info.addrType = AddrType::FLAT;
            result.info.surfaceId = 0;
            result.info.immediateOffset = 0;
            result.info.docs = nullptr;
            //
            // syntax.sfid = _sfid;
            result.syntax.controls = "." + ToSyntax(_sfid);
            //
            decodePayloadSizes();
        }

        Platform platform() const {
            return platf;
        }

        bool platformInRange(Platform lo, Platform hi) const {
            return platform() >= lo && platform() <= hi;
        }

        void setDoc(const char *doc) {
            setDoc(doc, doc);
        }
        void setDoc(const char *preXE, const char *pstXE) {
            result.info.docs = platform() < Platform::XE ?
                preXE : pstXE;
        }

        /////////////////////////////////////////////////////////////
        // diagnostics
        template <
            typename T1,
            typename T2 = const char *,
            typename T3 = const char *>
        void addDiag(
                DiagnosticList &dl,
                int off, int len,
                T1 t1,
                T2 t2 = "",
                T3 t3 = "")
        {
            std::stringstream ss;
            ss << t1 << t2 << t3;
            dl.emplace_back(DescField(off,len), ss.str());
        }
        template <typename T1,
            typename T2 = const char *, typename T3 = const char *>
        void warning(int off, int len, T1 t1, T2 t2 = "", T3 t3 = "") {
            addDiag(result.warnings, off, len, t1, t2, t3);
        }
        template <typename T1,
            typename T2 = const char *, typename T3 = const char *>
        void error(int off, int len, T1 t1, T2 t2 = "", T3 t3 = "") {
            addDiag(result.errors, off, len, t1, t2, t3);
        }

        // TODO: phase out
        // offsets 32 to 64 fetch exDesc
        uint32_t getDescBits(int off, int len) const {
            uint32_t bits = desc.imm;
            if (off >= 32) {
                off -= 32;
                bits = exDesc.imm;
            }
            uint32_t mask = len == 32 ? 0xFFFFFFFF : ((1 << len) - 1);
            return (int)((bits >> off) & mask);
        }

        // TODO: phase out
        uint32_t getDescBit(int off) const {
            return getDescBits(off, 1) != 0;
        }

        uint32_t decodeExDescField(
            const char *fieldName,
            int off,
            int len,
            DescFieldFormatter fmtMeaning = NO_DECODE)
        {
            auto val = getDescBits(off+32, len);
            std::stringstream ss;
            fmtMeaning(ss, val);
            addField(fieldName, off+32, len, val, ss.str());
            return val;
        }
        uint32_t decodeDescField(
            const char *fieldName,
            int off,
            int len,
            DescFieldFormatter fmtMeaning = NO_DECODE)
        {
            auto val = getDescBits(off, len);
            std::stringstream ss;
            fmtMeaning(ss, val);
            addField(fieldName, off, len, val, ss.str());
            return val;
        }
        uint32_t decodeDescBitField(
            const char *fieldName,
            int off,
            const char *zero,
            const char *one)
        {
            return decodeDescField(fieldName, off, 1,
                [&] (std::stringstream &ss, uint32_t val) {
                    ss << (val ? one : zero);
                });
        }
        uint32_t decodeDescBitField(
            const char *fieldName,
            int off,
            const char *one)
        {
            return decodeDescBitField(fieldName, off, "", one);
        }

        // normally use getDescBitsField, but in cases where you've already
        // decoded, the meaning and just want to record the result
        void addField(
            const char *fieldName,
            int off,
            int len,
            uint32_t val,
            std::string meaning)
        {
            Fragment f(fieldName, off, len);
            for (const auto &fvs : result.fields) {
                const auto &f1 = std::get<0>(fvs);
                if (f1.overlaps(f)) {
                    // uncomment for debugging
                    // std::stringstream ss;
                    // ss << "overlapped fields: " << f1.name << " and " << f.name;
                    // IGA_ASSERT_FALSE(ss.str().c_str());
                    return; // replicated access (don't record again)
                }
            }
            result.fields.emplace_back(f, val, meaning);
        }

        ///////////////////////////////////////////////////////////////////////////
        // decoder helpers
        bool decodeExpected(
            int off,
            int len,
            const char *fieldName,
            uint32_t expected)
        {
            auto val = getDescBits(off, len);
            if (val != expected) {
                warning(off, len, "field should be ", expected);
            }
            addField(fieldName, off, len, val, "");
            return val == expected;
        }

        // decodes MLen, RLen, and XLen if present
        // (Src0.Length, Dst.Length, Src1.Length)
        void decodePayloadSizes();

        ///////////////////////////////////////////////////////////////////////////
        // the most generic setter
        void setScatterGatherOpX(
            std::string msgSym,
            std::string msgImpl,
            SendOp op,
            AddrType addrType,
            SendDesc surfaceId,
            CacheOpt l1,
            CacheOpt l3,
            int addrSize,
            int bitsPerElemReg, int bitsPerElemMem,
            int elemsPerAddr,
            int simd,
            int extraAttrs = 0)
        {
            MessageInfo &mi = result.info;
            mi.symbol = msgSym;
            mi.description = msgImpl;
            mi.op = op;
            mi.cachingL1 = l1;
            mi.cachingL3 = l3;
            mi.addrType = addrType;
            mi.surfaceId = surfaceId;
            mi.attributeSet |= extraAttrs;
            mi.addrSizeBits = addrSize;
            mi.elemSizeBitsRegFile = bitsPerElemReg;
            mi.elemSizeBitsMemory = bitsPerElemMem;
            mi.elemsPerAddr = elemsPerAddr;
            mi.channelsEnabled = 0;
            mi.execWidth = simd;
        }

        void setScatterGatherOp(
            std::string msgSym,
            std::string msgDesc,
            SendOp op,
            AddrType addrType,
            SendDesc surfaceId,
            int addrSize,
            int bitsPerElem,
            int elemsPerAddr,
            int simd,
            int extraAttrs = 0)
        {
            setScatterGatherOpX(
                msgSym,
                msgDesc,
                op,
                addrType,
                surfaceId,
                CacheOpt::DEFAULT,
                CacheOpt::DEFAULT,
                addrSize,
                bitsPerElem, bitsPerElem,
                elemsPerAddr,
                simd,
                extraAttrs);
        }

        // for miscellaneous stuff such as fences and whatnot
        //
        // treat the payloads as full register units and set the op to SIMD1
        void setSpecialOpX(
            std::string msgSym,
            std::string msgDesc,
            SendOp op,
            AddrType addrType,
            SendDesc surfaceId,
            int mlen,
            int rlen,
            int extraAttrs = 0)
        {
            MessageInfo &mi = result.info;
            mi.symbol = msgSym;
            mi.description = msgDesc;
            mi.op = op;
            mi.cachingL1 = CacheOpt::DEFAULT;
            mi.cachingL3 = CacheOpt::DEFAULT;
            mi.addrType = addrType;
            mi.surfaceId = surfaceId;
            mi.addrSizeBits = mlen*BITS_PER_REGISTER;
            // e.g. SIMD16 platforms are 256b (two full registers)
            mi.elemSizeBitsRegFile = rlen*BITS_PER_REGISTER;
            mi.elemSizeBitsMemory = mi.elemSizeBitsRegFile;
            mi.channelsEnabled = 0;
            mi.elemsPerAddr = 1;
            mi.execWidth = 1;
            mi.attributeSet = extraAttrs | MessageInfo::VALID;
        }

    }; // MessageDecoder


    ///////////////////////////////////////////////////////////////////////////
    // shared by MessageDecoderHDC, MessageDecoderOther
    struct MessageDecoderLegacy : MessageDecoder {
        static const int SLM_BTI = 0xFE;
        static const int COHERENT_BTI = 0xFF;
        static const int NONCOHERENT_BTI = 0xFD;

        MessageDecoderLegacy(
            Platform _platform,
            SFID _sfid,
            SendDesc _exDesc,
            SendDesc _desc,
            RegRef _indDesc,
            DecodeResult &_result)
            : MessageDecoder(
                _platform, _sfid, _exDesc, _desc, _indDesc, _result)
        {
        }

        // from legacy encodings
        int decodeBTI(int addrBits) {
            int bti = (int)getDescBits(0, 8);
            std::stringstream ss;
            ss << "surface " << bti;
            if (bti == SLM_BTI) {
                ss << " (SLM)";
            } else if (bti == COHERENT_BTI) {
                if (addrBits == 64)
                    ss << " A64 ";
                else
                    ss << " A32 ";
                ss << " (coherent stateless)";
            } else if (bti == NONCOHERENT_BTI) {
                if (addrBits == 64)
                    ss << " A64";
                else
                    ss << " A32";
                ss << " (incoherent stateless)";
            } else if (bti == 0xFC) {
                ss << " (SSO)";
            }
            addField("BTI", 0, 8, bti, ss.str());
            return bti;
        }

        /////////////////////////////////////////////////////
        // "header" decoding
        bool decodeMDC_H() { // optional
            return decodeDescBitField(
                "Header",
                19,
                "absent",
                "included") != 0;
        }
        void decodeMDC_HF() {
            if (getDescBit(19) != 0)
                warning(19, 1,
                    "this message forbids a header (and it's included)");
        }
        void decodeMDC_HR() {
            if (!decodeMDC_H())
                warning(19, 1,
                    "this message requires a header (and it's absent)");
        }
        bool decodeMDC_H2() {
            return decodeDescBitField(
                "DualHeader", 19, "absent",
                "included (two register header)") != 0;
        }

        ///////////////////////////////////////////////////////////////////////
        // some shared decoder helpers
        int decodeMDC_SM2(int off) {
            // yeah SM2 is really 1 bit (2 means two values)
            int bits =
                decodeDescBitField("SimdMode:MDC_SM2", off, "SIMD8", "SIMD16");
            return bits ? 16 : 8;
        }
    }; // MessageDecoderLegacy

    // MessageDecoderHDC.cpp
    void decodeDescriptorsHDC(
        Platform platform, SFID sfid,
        SendDesc exDesc, SendDesc desc, RegRef indDesc,
        DecodeResult &result);

    // MessageDecoderOther.cpp
    void decodeDescriptorsOther(
        Platform platform, SFID sfid,
        SendDesc exDesc, SendDesc desc, RegRef indDesc,
        DecodeResult &result);


} // iga::

#endif // _IGA_BACKEND_MESSAGES_MESSAGEDECODER_HPP_

/*========================== begin_copyright_notice ============================

Copyright (c) 2017-2021 Intel Corporation

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

#ifndef IGA_MODELS_HPP
#define IGA_MODELS_HPP

#include "../asserts.hpp"
#include "../IR/Types.hpp"
#include "OpSpec.hpp"

#include <cstddef>
#include <cstdint>

namespace iga
{

    struct RegInfo {
        // the register name corresponding to this platform
        RegName     regName;
        // the lexical identifier for this register
        const char *syntax;
        // a description of this register
        // e.g. "State Register"
        const char *desc;

        // For GRF this is nothing.
        // For ARF this is RegNum[7:4].
        // The value is unshifted.  I.e. only 4 bits.
        //
        // RegNum[3:0] usually holds the register number itself for
        // the particular ARF.  E.g. acc1 has 0001b there.
        // The exception is mme, which maps to acc2-9 on some platforms and
        // other accumulators elsewhere.
        uint32_t    regNum7_4;
        // The amount to add to the register number given to set
        // RegNum[3:0].  For most this will be 0, but a MMR will be offset
        // within the ACC space (since they are shared)
        int         regNumBase;

        // platform where this was first introduced
        Platform    platIntrd;
        // platform where this was last used
        Platform    platLast;
        // access granularity (in bytes)
        int         accGran;

        // number of registers
        // Zero 0 means no reg. number and the register has only 1
        // e.g. "ce" instead of "ce0"
        int         numRegs;
        // The number of bytes in each subregister
        // Certain registers are kind of wonky and have uneven sized registers
        int         numBytesPerReg[16];

        bool isRegNumberValid(int reg) const {
            // wonky because null and sp have "0" registers (meaning 1 implied)
            // so reg==0 is alway valid for everyone
            return reg == 0 ||
                (reg >= 0 && reg < numRegs); // otherwise: one of several registers
        }
        bool isSubRegByteOffsetValid(
            int regNum, int subregByte, int grfSize) const
        {
            int regBytes = 0;
            // special handling null register that we may use sub-register number for other
            // purposes. At those cases the sub register number of null should not excceed GRF bound.
            if (regName == RegName::ARF_NULL) {
                regBytes = grfSize;
            } else {
                regBytes = regName == RegName::GRF_R ? grfSize :
                    numBytesPerReg[regNum];
            }
            return subregByte < regBytes;
        }

        bool supportsRegioning() const {
            // needs to be more liberal than before
            // context save and restore seems to region some of the
            // non-regionable registers
            return regName == RegName::ARF_NULL || hasSubregs();
        }
        bool supportedOn(Platform p) const {
            return platIntrd <= p && p <= platLast;
        }
        bool hasRegNum() const {
            return numRegs > 0; // e.g. "cr0" or "r13" VS "ce" or "null"
        }
        bool hasSubregs() const {
            switch (regName) {
            case RegName::ARF_IP:
            case RegName::ARF_CE:
            case RegName::ARF_NULL:
                return false;
            default:
                return true;
            }
        }

        int getNumReg() const { return numRegs; }

        bool encode(int reg, uint8_t &regNumBits) const;
        bool decode(uint8_t regNumBits, int &reg) const;
    }; // RegInfo

    // returns the table for all platforms.
    // Most users should try and use one of the Model::lookupXXX methods
    const RegInfo *GetRegisterSpecificationTable(int &len);

    // See IRChecker.cpp / checkDst (these ARFs need {Switch})
    static inline bool arfNeedsSwitch(RegName rn) {
        // ARFs without a scoreboard need {Switch} before the write
        //  Registers with scoreboard and no switch required -
        //    Accumulator/address register/flag register/notify register
        //  Registers without scoreboard and switch required -
        //    Control Register/State Register/Stack Pointer/Timestamp/Pause/IP register
        //  CE is read only.
        //  FC does not require switch. It is read/write only in CSR SIP Routine.
        //  TDR does not require switch. It is read/write only in CSR SIP Routine.

        switch (rn) {
        case RegName::ARF_CR:
        case RegName::ARF_DBG:
        case RegName::ARF_IP:
        case RegName::ARF_SP:
        case RegName::ARF_SR:
        case RegName::ARF_TM:
            return true;
        default:
            return false;
        }
    }

    static inline bool IsRegisterScaled(RegName regName)
    {
        switch (regName)
        {
        case RegName::GRF_R:
        case RegName::ARF_NULL:
        case RegName::ARF_A:
        case RegName::ARF_ACC:
        case RegName::ARF_MME:
        case RegName::ARF_TM:
        case RegName::ARF_CR:
        case RegName::ARF_SP:
        case RegName::ARF_F:
        case RegName::ARF_N:
        case RegName::ARF_DBG:
        case RegName::ARF_SR:
        case RegName::ARF_TDR:
            return true;
        case RegName::ARF_FC:
        default:
            return false;
        }
    }

    // helper function to translate byte offset to subReg Num
    static inline uint8_t BytesOffsetToSubReg(
        uint32_t offset, RegName regName, Type type)
    {
        if (!IsRegisterScaled(regName) || type == Type::INVALID) {
            return (uint8_t)offset;
        }
        auto tsh = TypeSizeShiftsOffsetToSubreg(type);
        return (uint8_t)((offset << std::get<0>(tsh)) >> std::get<1>(tsh));
    }

    // helper functions to translate subReg number to Offset in binary
    static inline uint32_t SubRegToBytesOffset(
        int subRegNum, RegName regName, Type type)
    {
        if (!IsRegisterScaled(regName) || type == Type::INVALID) {
            return subRegNum;
        }
        auto tsh = TypeSizeShiftsOffsetToSubreg(type);
        // NOTE: flipped tuple access (1 <-> 0) since we are unscaling
        return (subRegNum << std::get<1>(tsh)) >> std::get<0>(tsh);
    }

    static inline uint8_t WordsOffsetToSubReg(
        uint32_t offset, RegName regName, Type type)
    {
        // for the non-scaled registers (e.g. fc), the sub-register in binary
        // and in asm is the same value
        if (!IsRegisterScaled(regName) || type == Type::INVALID) {
            return (uint8_t)offset;
        }
        return BytesOffsetToSubReg(offset * 2, regName, type);
    }

    // reutrn if the given subreg num is Word aligned or not and
    // the corresponding word offset
    static inline std::pair<bool,uint32_t> SubRegToWordsOffset(
        int subRegNum, RegName regName, Type type)
    {
        // for the non-scaled registers (e.g. fc), the sub-register in binary
        // and in asm is the same value
        if (!IsRegisterScaled(regName) || type == Type::INVALID) {
            return std::make_pair(true, subRegNum);;
        }
        uint32_t byteoff = SubRegToBytesOffset(subRegNum, regName, type);
        return std::make_pair((byteoff % 2 == 0), byteoff / 2);
    }

    // enables abstract iteration of all OpSpecs in the Model
    // see Model::ops()
    class OpSpecTableIterator {
        Op curr;
        const OpSpec *const opsArray;
        void advanceToNextValid() {
            advance(); // advance at least one
            while (curr <= Op::LAST_OP && !currValid()) {
                advance();
            }
        }
        bool currValid() const { return opsArray[(int)curr].isValid(); }
        void advance() {
            if (curr <= Op::LAST_OP) {
                curr = (Op)((int)curr + 1);
            }
        }
    public:
        OpSpecTableIterator(const OpSpec *const ops, Op from)
            : curr(from), opsArray(ops)
        {
            if (!currValid()) { // if FIRST_OP is bogus, go to next
                advanceToNextValid();
            }
        }
        bool operator==(const OpSpecTableIterator &rhs) const {
            return curr == rhs.curr;
        }
        bool operator!=(const OpSpecTableIterator &rhs) const {
            return !(*this == rhs);
        }
        // OpSpecTableIterator  operator++(int); // post-increment
        OpSpecTableIterator&  operator++() { // pre-increment
            advanceToNextValid();
            return *this;
        }
        const OpSpec *operator *() const {
            return opsArray + (int)curr;
        }
    };
    class OpSpecTableWalker {
    private:
        const OpSpec *const opsArray;
        OpSpecTableIterator end_itr;
    public:
        OpSpecTableWalker(const OpSpec *const ops)
            : opsArray(ops)
            , end_itr(ops, (Op)((int)Op::LAST_OP + 1))
        {
        }
        OpSpecTableIterator begin() const {
            return OpSpecTableIterator(opsArray, Op::FIRST_OP);
        }
        const OpSpecTableIterator &end() const { return end_itr; }
    };


    // error info if we fail to resolve the OpSpec
    // only valid if `decoodeOpSpec` returns nullptr
    struct OpSpecMissInfo
    {
        uint64_t     opcode;  // the opcode bits we tried to lookup
                              // for subfunctions (e.g. math.*), this'll be
                              // the subfunction bits we were looking for
    };


    // Corresponds to a platform model (e.g. GEN9)
    // Has methods to lookup the various operations (OpSpec's) by name
    // opcode value (7 bit encoding), and enumeration value (Op).
    struct Model {
        Platform             platform;

        // the table of supported ops for this model indexed by iga::Op
        const OpSpec        *const opsArray;
        //
        // file ext (e.g. "12p1" for TGL)
        ModelString          extension;
        //
        // various platform names we match for this model (e.g. "TGL")
        static const size_t  MAX_NAMES = 6;
        ModelString          names[MAX_NAMES];

        constexpr Model(
            Platform p,
            const OpSpec *const opsArr,
            ModelString ext,
            ModelString name0,
            ModelString name1 = ModelString(),
            ModelString name2 = ModelString(),
            ModelString name3 = ModelString(),
            ModelString name4 = ModelString(),
            ModelString name5 = ModelString())
            : platform(p), opsArray(opsArr), extension(ext)
            , names{name0, name1, name2, name3, name4, name5}
        {
        }

        /*
         * Enables iteration of all valid ops in the table in a for all loop
         * E.g. one would write:
         *   iga::Model model = ...
         *   for (const OpSpec *os : model.ops()) {
         *      IGA_ASSERT(os->isValid(), "all ops walked will be valid");
         *   }
         */
        OpSpecTableWalker ops() const {return OpSpecTableWalker(opsArray);}

        const OpSpec&        lookupOpSpec(Op op) const;
        const OpSpec&        lookupOpSpecByCode(unsigned opcode) const;
        const OpSpec&        lookupOpSpecFromBits(const void *bits, OpSpecMissInfo &missInfo) const;
        const RegInfo*       lookupArfRegInfoByRegNum(uint8_t regNum7_0) const;
        const RegInfo*       lookupRegInfoByRegName(RegName name) const;


        static const Model  *LookupModel(Platform platform);


        bool supportsHwDeps() const
        {
            return platform <= Platform::GEN11;
        }
        // send is unary (sends is binary)
        bool supportsUnarySend() const {
            return supportsHwDeps();
        }
        // sends merged with send (send is binary)
        bool supportsUnifiedSend() const {
            return !supportsHwDeps();
        }
        // registers in control flow is stored in src1 for
        // certain instructions
        bool supportsSrc1CtrlFlow() const {
            return supportsUnarySend();
        }

        // the wait instruction exists
        bool supportsWait() const { return supportsHwDeps(); }

        // ImplAcc must be Align16
        bool supportsAlign16ImplicitAcc() const {
            return platform <= Platform::GEN10;
        }
        // If the GED_ACCESS_MODE is supported
        bool supportsAccessMode() const { return supportsAlign16ImplicitAcc(); }

        // {NoSrcDepSet} allowed
        bool supportNoSrcDepSet() const {
            return platform >= Platform::GEN9 && !supportsUnifiedSend();
        }
        // {NoPreempt} allowed
        bool supportsNoPreempt() const {
            return platform >= Platform::GEN10 && !supportsUnifiedSend();
        }
        // implies that:
        //  - branches don't have types
        //  - the pc is always relative to pre-inc (even jmpi)
        bool supportsSimplifiedBranches() const {
            return platform >= Platform::XE;
        }

        bool supportsAlign16() const { return platform <= Platform::GEN10; }
        bool supportsAlign16MacroOnly() const { return platform == Platform::GEN10; }
        bool supportsAlign16Ternary() const { return platform < Platform::GEN10; }
        bool supportsAlign16MacroInst() const { return platform <= Platform::GEN10; }

        uint32_t getNumGRF()      const;
        uint32_t getNumFlagReg()  const;
        uint32_t getGRFByteSize() const;

        uint32_t getRegCount(RegName rn) const;
        uint32_t getBytesPerReg(RegName rn) const;

        /// getSWSBEncodeMode - get the default swsb encoding mode derived from platform
        SWSB_ENCODE_MODE getSWSBEncodeMode() const {
            if (platform == Platform::XE)
                return SWSB_ENCODE_MODE::SingleDistPipe;
            return SWSB_ENCODE_MODE::SWSBInvalidMode;
        }

        // Get the max number of swsb id
        uint32_t getMaxSWSBTokenNum() const {
            switch(getSWSBEncodeMode()) {
            case SWSB_ENCODE_MODE::SingleDistPipe:
                return 16;
            default:
                break;
            }

            return 16;
        }

        uint32_t getSWSBMaxValidDistance() const {
            return 7;
        }

    }; // class Model

    ///////////////////////////////////////////////////////////////////////////
    // In rare cases where one must iterate all models
    extern const Model * const ALL_MODELS[];
    extern const size_t ALL_MODELS_LEN;
} // namespace iga::*

#endif // IGA_MODELS_HPP

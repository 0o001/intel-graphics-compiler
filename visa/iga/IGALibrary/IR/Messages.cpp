/*========================== begin_copyright_notice ============================

Copyright (C) 2020-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include "Messages.hpp"
#include "../Backend/Messages/MessageDecoder.hpp"
#include "../Frontend/IRToString.hpp"

#include <sstream>
#include <string>

using namespace iga;


///////////////////////////////////////////////////////////////////////////////
static void deducePayloadSizes(
    PayloadLengths &lens,
    Platform p, SFID sfid, ExecSize execSize, uint32_t _desc)
{
    SendDesc desc(_desc), exDesc(0);

    const auto result = tryDecode(p, sfid, execSize, exDesc, desc, nullptr);
    if (!result) {
        return;
    }
    const MessageInfo &mi = result.info;

    const int REG_SIZE_BITS =
            256;
    const int FULL_EXEC_SIZE =
            16;

    int execElems = static_cast<int>(execSize);
    if (execElems < FULL_EXEC_SIZE/2)
        execElems = FULL_EXEC_SIZE/2;

    auto opIsGroup = [&] (SendOp opGroup) {
        return (static_cast<int>(result.info.op) &
            static_cast<int>(opGroup)) != 0;
    };
    auto isVectorMessageSimd1 = [&] () {
        bool isSimd1 = mi.isTransposed() ||
            mi.op == SendOp::LOAD_STRIDED ||
            mi.op == SendOp::STORE_STRIDED;
        return isSimd1;
    };

    // messages of the form:
    //    send*  null  reg  null ...
    // with src0 len of 1
    auto message = [&] (int dstLen = 0, int src0Len = 1, int src1Len = 0) {
        lens.dstLen = dstLen;
        lens.src0Len = src0Len;
        lens.src1Len = src1Len;
    };

    auto numAddrRegsForVector = [&] () {
        if (isVectorMessageSimd1()) {
            return 1;
        } else {
            int alen = execElems*mi.addrSizeBits/REG_SIZE_BITS;
            if (execElems*mi.addrSizeBits % REG_SIZE_BITS)
                alen++;
            return alen;
        }
    };

    auto handleVectorMessage = [&] (int atomicParams = -1) {
        int dlen = -1;
        if (mi.isTransposed()) {
            dlen = mi.elemSizeBitsRegFile*mi.elemsPerAddr/REG_SIZE_BITS;
            if ((mi.elemSizeBitsRegFile*mi.elemsPerAddr) % REG_SIZE_BITS != 0)
                dlen++;
        } else {
            dlen = execElems*mi.elemSizeBitsRegFile/REG_SIZE_BITS;
            if ((execElems*mi.elemSizeBitsRegFile) % REG_SIZE_BITS != 0)
                dlen++;
            dlen *= mi.elemsPerAddr;
        }
        //
        const auto &opInfo = lookupSendOp(mi.op);
        lens.src0Len = numAddrRegsForVector();
        if (opInfo.isLoad()) {
            lens.dstLen = dlen;
            lens.src1Len = 0;
        } else if (opInfo.isStore()) {
            lens.dstLen = 0;
            lens.src1Len = dlen;
        } else if (opInfo.isAtomic()) {
            lens.dstLen = dlen;
            lens.src1Len = atomicParams;
        } else {
            IGA_ASSERT_FALSE("invalid message type");
        }
    };
    lens.uvrlod = mi.hasAttr(MessageInfo::Attr::TYPED);

    switch (mi.op) {
    ///////////////////////////////////////////////////////////////////////
    // easy and common cases
    case SendOp::LOAD:
    case SendOp::LOAD_STRIDED:
    case SendOp::LOAD_QUAD:
    case SendOp::STORE:
    case SendOp::STORE_STRIDED:
    case SendOp::STORE_QUAD:
        // normal vector message
        handleVectorMessage();
        break;
    ///////////////////////////////////////////////////////////////////////
    // slightly harder
    case SendOp::LOAD_STATUS:
        // returns one status register only
        // cheat so num addr regs and src1 are set;
        // then stomping the return value to reflect the 32b return value
        handleVectorMessage();
        lens.dstLen = 1;
        break;
    case SendOp::READ_STATE:
        // src0 is U, V, R, LOD 32b each (128b total)
        // dst is 1 reg (with room to spare)
        message(1, 1, 0);
        break;
    case SendOp::ATOMIC_LOAD:
    case SendOp::ATOMIC_IINC:
    case SendOp::ATOMIC_IDEC:
    case SendOp::ATOMIC_IPDEC:
        handleVectorMessage(0);
        break;
    case SendOp::ATOMIC_STORE:
    case SendOp::ATOMIC_AND:
    case SendOp::ATOMIC_XOR:
    case SendOp::ATOMIC_OR:
    case SendOp::ATOMIC_IADD:
    case SendOp::ATOMIC_ISUB:
    case SendOp::ATOMIC_IRSUB:
    case SendOp::ATOMIC_SMIN:
    case SendOp::ATOMIC_SMAX:
    case SendOp::ATOMIC_UMIN:
    case SendOp::ATOMIC_UMAX:
    case SendOp::ATOMIC_FADD:
    case SendOp::ATOMIC_FSUB:
    case SendOp::ATOMIC_FMIN:
    case SendOp::ATOMIC_FMAX:
        handleVectorMessage(1);
        break;
    case SendOp::ATOMIC_ICAS:
    case SendOp::ATOMIC_FCAS:
        handleVectorMessage(2);
        break;
    ///////////////////////////////////////////////////////////////////////
    // barrier is 1, 1, 0 until XeHP
    case SendOp::BARRIER:
        message(p < Platform::XE_HP ? 1 : 0, 1, 0);
        break;
    // other control messages that are 0, 1, 0
    case SendOp::MONITOR:
    case SendOp::UNMONITOR:
    case SendOp::SIGNAL:
    case SendOp::EOT:
        message();
        break;
    ///////////////////////////////////////////////////////////////////////
    // control messages that return a register
    case SendOp::FENCE:
        message(1, 1, 0);
        break;
    case SendOp::WAIT:
        message(1, 1, 0);
        break;
    default:
        break;
    }
}


PayloadLengths::PayloadLengths(
    Platform p, SFID sfid, ExecSize execSize, uint32_t desc)
{
    deducePayloadSizes(*this, p, sfid, execSize, desc);
}


PayloadLengths::PayloadLengths(
    Platform p, ExecSize execSize, uint32_t desc, uint32_t exDesc)
{
    IGA_ASSERT(p <= Platform::GEN11, "wrong constructor for platform");
    SFID sfid = sfidFromEncoding(p, exDesc);
    deducePayloadSizes(*this, p, sfid, execSize, desc);
}


SFID iga::sfidFromEncoding(Platform p, uint32_t sfidBits)
{
    SFID sfid = SFID::INVALID;
    switch (sfidBits & 0xF) {
    case 0x0: sfid = SFID::NULL_; break;
    case 0x2: sfid = SFID::SMPL; break;
    case 0x3: sfid = SFID::GTWY; break;
    case 0x4: sfid = SFID::DC2;  break;
    case 0x5: sfid = SFID::RC;   break;
    case 0x6: sfid = SFID::URB;  break;
    case 0x7:
        sfid = SFID::TS;
        break;
    case 0x8:
        sfid = SFID::VME;
        break;
    case 0x9:
        sfid = SFID::DCRO;
        break;
    case 0xA: sfid = SFID::DC0;  break;
    case 0xB: sfid = SFID::PIXI; break;
    case 0xC: sfid = SFID::DC1;  break;
    case 0xD:
        sfid = SFID::CRE;
        break;
    default:
        sfid = SFID::INVALID;
    }
    return sfid;
}


///////////////////////////////////////////////////////////////////////////////
std::string MessageSyntax::str(
    std::string execInfo,
    std::string dataReg,
    std::string addrReg,
    std::string atmoicArgReg) const
{
    std::stringstream ss;

    auto emitAddr =
        [&]() {
            if (addrReg.empty())
                return;
            ss << " " << surface;
            ss << "[";
            ss << scale;
            ss << addrReg;
            ss << immOffset;
            ss << "]";
        };
    auto emitData =
        [&]() {
            if (!dataReg.empty()) {
                ss << " " << dataReg;
            }
        };
    auto emitExtraArg =
        [&]() {
            if (!atmoicArgReg.empty())
                ss << " " << atmoicArgReg;
        };

    ss << mnemonic << controls;
    if (!execInfo.empty())
        ss << " " << execInfo;

    if (isLoad()) {
        emitData();
        emitAddr();
    } else if (isStore()) {
        emitAddr();
        emitData();
    } else if (isAtomic()) {
        emitData();
        emitAddr();
        emitExtraArg();
    } else if (isControl()) {
        // not sure the cleanest way to do this
        emitData();
        emitAddr();
        emitExtraArg();
    }

    return ss.str();
}

std::string MessageSyntax::sym() const
{
    std::stringstream ss;
    ss << mnemonic << controls;
    if (!surface.empty()) {
        ss << "." << surface;
    }
    if (!scale.empty() || !immOffset.empty()) {
        if (surface.empty())
            ss << ".flat";
        ss << "[" << scale << "A" << immOffset << "]";
    }
    return ss.str();
}


static constexpr SendOpDefinition SEND_OPS[] {
#define DEFINE_SEND_OP(ENUM, MNE, DESC, ATTRS) \
    {SendOp::ENUM, MNE, DESC, ATTRS},
#include "EnumSendOpInfo.hpp"
#undef DEFINE_SEND_OP
};

std::string iga::ToSyntax(SendOp op)
{
    const auto &opInfo = lookupSendOp(op);
    if (opInfo.isValid()) {
        return opInfo.mnemonic;
    } else {
        return iga::fmtHex(int(op), 2) + "?";
    }
}

std::string iga::ToSymbol(CacheOpt op)
{
#define MK_CASE(X) case CacheOpt::X: return #X
    switch (op) {
    MK_CASE(INVALID);
    MK_CASE(DEFAULT);
    MK_CASE(READINVALIDATE);
    MK_CASE(CACHED);
    MK_CASE(UNCACHED);
    default:
        std::stringstream ss;
        ss << "0x" << std::hex << (int)op << "?";
        return ss.str();
    }
#undef MK_CASE
}

std::string iga::ToSymbol(AddrType op)
{
#define MK_CASE(X) case AddrType::X: return #X
    switch (op) {
    MK_CASE(INVALID);
    MK_CASE(FLAT);
    MK_CASE(BTI);
    default:
        std::stringstream ss;
        ss << "0x" << std::hex << (int)op << "?";
        return ss.str();
    }
#undef MK_CASE
}




static bool isHDC(SFID sfid)
{
    switch (sfid) {
    case SFID::DCRO:
    case SFID::DC0:
    case SFID::DC1:
    case SFID::DC2:
        return true;
    default:
        return false;
    }
}



///////////////////////////////////////////////////////////////////////////////
// e.g. set valid bit, re-order fields etc...
static void postProcessDecode(
    SendDesc desc, SendDesc exDesc,
    DecodeResult &result, DecodedDescFields *fields)
{
    if (!result.errors.empty())
        result.info.attributeSet |= MessageInfo::Attr::VALID;
    if (fields) {
        std::sort(result.fields.begin(), result.fields.end(),
            [&] (const auto &f1, const auto &f2) {
                return std::get<0>(f1).offset > std::get<0>(f2).offset;
            });
        *fields = result.fields;
        //
        // make sure there aren't unmapped bits
        // run through all 64 bits of [ExDesc:Desc] and find maximal spans
        // that have no owner field (breaking between descriptors)
        auto fieldOwnsBit =
            [&](int i) {
                for (const auto &fv : result.fields) {
                    const auto &f = std::get<0>(fv);
                    if (i >= f.offset && i < f.offset + f.length) {
                        return true;
                    }
                }
                return false;
            };

        int len = exDesc.isImm() ? 64 : 32; // 32 if a0
        auto testDescBit =
            [&] (int ix) {
            uint32_t val = desc.imm;
            if (ix >= 32) {
                ix -= 32;
                val = exDesc.imm;
            }
            return (val & (1 << ix)) != 0;
        };

        for (int i = 0; i < len;) {
            if (testDescBit(i) && !fieldOwnsBit(i)) {
                // beginning of an undefined field
                int undefLen = 1;
                while ((i + undefLen) % 32 != 0 &&
                    testDescBit(i + undefLen))
                {
                    // don't span undefined fields across ExDesc:Desc
                    undefLen++;
                }
                //
                result.warnings.emplace_back(
                    DescField(i, undefLen), "bits set in undefined field");
                i += undefLen;
                //
                // uncomment for linting bad descriptors
                // std::stringstream ss;
                // ss << "[" << i << "] = 1 not mapped by field";
                // IGA_ASSERT_FALSE(ss.str().c_str());
            } else {
                i++;
            }
        }
    }

    // set the syntax kind
    if (result && !result.syntax.mnemonic.empty()) {
        // !result.syntax.mnemonic.empty(): to filter out HDC stuff until
        // we get that working
        if (result.info.isLoad()) {
            result.syntax.layout = MessageSyntax::Layout::LOAD;
        } else if (result.info.isStore()) {
            result.syntax.layout = MessageSyntax::Layout::STORE;
        } else if (result.info.isAtomic()) {
            result.syntax.layout = MessageSyntax::Layout::ATOMIC;
        } else {
            result.syntax.layout = MessageSyntax::Layout::CONTROL;
        }
    }
} // postProcessDecode


DecodeResult iga::tryDecode(
    Platform platform, SFID sfid, ExecSize execSize,
    SendDesc exDesc, SendDesc desc,
    DecodedDescFields *fields)
{
    DecodeResult result;

    if (isHDC(sfid)) {
        decodeDescriptorsHDC(
            platform, sfid, execSize,
            exDesc, desc,
            result);
    }
    else {
        decodeDescriptorsOther(
            platform, sfid, execSize,
            exDesc, desc,
            result);
    }

    postProcessDecode(desc, exDesc, result, fields);

    return result;
}

const SendOpDefinition &iga::lookupSendOp(SendOp op)
{
    for (int i = 0; i < sizeof(SEND_OPS)/sizeof(SEND_OPS[0]); i++) {
        if (op == SEND_OPS[i].op) {
            return SEND_OPS[i];
        }
    }

    static constexpr SendOpDefinition INVALID(SendOp::INVALID, "?", "?");
    return INVALID;
/*
    SendOpDefinition soi {false, -1, false};

    switch (op) {
    case SendOp::LOAD_STRIDED:
    case SendOp::STORE_STRIDED:
    case SendOp::READ_STATE:
        soi.isSingleRegAddr = true;
    default: break;
    }

    if (SendOpIsLoad(op)) {
        soi.hasDst = true;
        soi.src1Args = 0;
    } else if (SendOpIsStore(op)) {
        soi.hasDst = false;
        soi.src1Args = 1;
    } else {
        auto atomic = [&](int args) {
            soi.hasDst = true;
            soi.src1Args = args;
        };

        switch (op) {
        case SendOp::ATOMIC_LOAD:
        case SendOp::ATOMIC_IINC:
        case SendOp::ATOMIC_IDEC:
        case SendOp::ATOMIC_IPDEC:
            atomic(0);
            break;
        case SendOp::ATOMIC_STORE:
        case SendOp::ATOMIC_AND:
        case SendOp::ATOMIC_XOR:
        case SendOp::ATOMIC_OR:
        case SendOp::ATOMIC_IADD:
        case SendOp::ATOMIC_ISUB:
        case SendOp::ATOMIC_IRSUB:
        case SendOp::ATOMIC_SMIN:
        case SendOp::ATOMIC_SMAX:
        case SendOp::ATOMIC_UMIN:
        case SendOp::ATOMIC_UMAX:
        case SendOp::ATOMIC_FADD:
        case SendOp::ATOMIC_FSUB:
        case SendOp::ATOMIC_FMIN:
        case SendOp::ATOMIC_FMAX:
            atomic(1);
            break;
        case SendOp::ATOMIC_ICAS:
        case SendOp::ATOMIC_FCAS:
            atomic(2);
            break;
        case SendOp::FENCE:
            // some fences have dstinations...
            soi.hasDst = false;
            break;
        default: break;
        }
    }

    return soi;
*/
}

const SendOpDefinition &iga::lookupSendOp(const char *mnemonic)
{
    std::string mne = mnemonic;
    for (int i = 0; i < sizeof(SEND_OPS)/sizeof(SEND_OPS[0]); i++) {
        if (mne == SEND_OPS[i].mnemonic) {
            return SEND_OPS[i];
        }
    }

    static constexpr SendOpDefinition INVALID(SendOp::INVALID, "?", "?");
    return INVALID;
}


bool iga::sendOpSupportsSyntax(Platform p, SendOp op, SFID sfid)
{
    bool supported = true;
    supported &=
        op == SendOp::LOAD ||
        op == SendOp::LOAD_STRIDED ||
        op == SendOp::LOAD_QUAD ||
        op == SendOp::STORE ||
        op == SendOp::STORE_STRIDED ||
        op == SendOp::STORE_QUAD ||
        lookupSendOp(op).isAtomic();
    return supported;
}




bool iga::encodeDescriptors(
    Platform p,
    const VectorMessageArgs &vma,
    SendDesc &exDesc,
    SendDesc &desc,
    std::string &err)
{
    if (desc.isReg()) {
        err = "cannot encode with register desc";
        return false;
    }
    // TODO: support HDC here
    // encodeVectorMessageHDC(p, vma, exDesc, desc);
    err = "unsupported message for SFID";
    return false;
}

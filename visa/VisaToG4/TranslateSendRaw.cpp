/*========================== begin_copyright_notice ============================

Copyright (C) 2020-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include "BuildIR.h"
#include "../Timer.h"

using namespace vISA;


// on legacy platforms EOT bit is encode in bit[5] of the exdesc.
// used by the rawSend/rawSends instructions
static bool isExDescEOT(uint32_t val)
{
    return val & 0x20;
}

int IR_Builder::translateVISARawSendInst(
    G4_Predicate *predOpnd, VISA_Exec_Size executionSize,
    VISA_EMask_Ctrl emask, uint8_t modifiers, unsigned int exDesc, uint8_t numSrc,
    uint8_t numDst, G4_Operand* msgDescOpnd, G4_SrcRegRegion* msgOpnd, G4_DstRegRegion* dstOpnd)
{
    TIME_SCOPE(VISA_BUILDER_IR_CONSTRUCTION);

    G4_ExecSize exsize = G4_ExecSize(Get_VISA_Exec_Size(executionSize));
    G4_InstOpts inst_opt = Get_Gen4_Emask(emask, exsize);

    if (msgDescOpnd->isSrcRegRegion())
    {
        // mov (1) a0.0<1>:ud src<0;1,0>:ud {NoMask}
        G4_DstRegRegion *dstOpnd = Create_Dst_Opnd_From_Dcl(builtinA0, 1);
        createMov(g4::SIMD1, dstOpnd, msgDescOpnd, InstOpt_WriteEnable, true);
        msgDescOpnd = Create_Src_Opnd_From_Dcl(builtinA0, getRegionScalar());
    }

    uint32_t desc = 0;
    bool isRead = true, isWrite = true, isValidFuncCtrl = true;
    if (msgDescOpnd->isImm())
    {
        desc = (uint32_t) msgDescOpnd->asImm()->getImm();
    }
    else
    {
        desc = G4_SendDescRaw::createDesc(0, false, numSrc, numDst);
        isValidFuncCtrl = false;
    }

    // bit[0-3] of the exDesc (always imm) holds the SFID
    G4_SendDescRaw *sendMsgDesc = createSendMsgDesc(
        intToSFID(exDesc & 0xF), desc, exDesc, 0,
        getSendAccessType(isRead, isWrite), nullptr, isValidFuncCtrl);

    if (isExDescEOT(exDesc))
    {
        sendMsgDesc->setEOT();
    }

    // sanity check on srcLen/dstLen moved to ISA verifier

    createSendInst(
        predOpnd,
        (modifiers & 1) ? G4_sendc : G4_send,
        exsize,
        dstOpnd,
        msgOpnd,
        msgDescOpnd,
        inst_opt,
        sendMsgDesc,
        true);

    return VISA_SUCCESS;
}

int IR_Builder::translateVISARawSendsInst(
    G4_Predicate *predOpnd, VISA_Exec_Size executionSize,
    VISA_EMask_Ctrl emask, uint8_t modifiers, G4_Operand* ex,
    uint8_t numSrc0, uint8_t numSrc1, uint8_t numDst,
    G4_Operand* msgDescOpnd, G4_Operand* src0, G4_Operand* src1, G4_DstRegRegion* dstOpnd,
    unsigned ffid, bool hasEOT)
{
    TIME_SCOPE(VISA_BUILDER_IR_CONSTRUCTION);

    G4_ExecSize exsize = G4_ExecSize(Get_VISA_Exec_Size(executionSize));
    const G4_InstOpts inst_opt = Get_Gen4_Emask(emask, exsize);

    if (msgDescOpnd->isSrcRegRegion())
    {
        // mov (1) a0.0<1>:ud src<0;1,0>:ud {NoMask}
        G4_DstRegRegion* dstOpnd = Create_Dst_Opnd_From_Dcl(builtinA0, 1);
        createMov(g4::SIMD1, dstOpnd, msgDescOpnd, InstOpt_WriteEnable, true);
        msgDescOpnd = Create_Src_Opnd_From_Dcl(builtinA0, getRegionScalar());
    }

    uint32_t exDescVal = 0;
    G4_SrcRegRegion *temp_exdesc_src = nullptr;
    if (ex->isImm())
    {
        exDescVal = (unsigned)ex->asImm()->getInt();
    }

    // bit [6:10] store the extended message length, and when it's >= 16 we have to use indirect
    uint32_t extLength = (exDescVal >> 6) & 0x1F;
    if (ex->isSrcRegRegion() || extLength >= 16)
    {
        // mov (1) a0.2<1>:ud src<0;1,0>:ud {NoMask}
        // to hold the dynamic ext msg descriptor
        G4_DstRegRegion* exDescDst = Create_Dst_Opnd_From_Dcl(getBuiltinA0Dot2(), 1);
        createMov(g4::SIMD1, exDescDst, ex, InstOpt_WriteEnable, true);
        temp_exdesc_src = Create_Src_Opnd_From_Dcl(getBuiltinA0Dot2(), getRegionScalar());

        if (exDescVal == 0)
        {
            exDescVal = G4_SendDescRaw::createExtDesc(intToSFID(ffid), false, numSrc1);
        }
    }

    uint32_t descVal = 0;
    bool isValidFuncCtrl = true;
    if (msgDescOpnd->isImm())
    {
        descVal = (uint32_t) msgDescOpnd->asImm()->getImm();
    }
    else
    {
        descVal = G4_SendDescRaw::createDesc(0, false, numSrc0, numDst);
        isValidFuncCtrl = false;
    }

    G4_SendDescRaw * sendMsgDesc = createSendMsgDesc(
        intToSFID(ffid), descVal, exDescVal, numSrc1,
        SendAccess::READ_WRITE, nullptr, isValidFuncCtrl);

    if (hasEOT)
    {
        sendMsgDesc->setEOT();
    }

    MUST_BE_TRUE(sendMsgDesc->MessageLength() == numSrc0, "message length mismatch for raw sends");
    if (!dstOpnd->isNullReg()) {
        MUST_BE_TRUE(sendMsgDesc->ResponseLength() <= numDst, "response length mismatch for raw sends");
    }
    MUST_BE_TRUE(sendMsgDesc->extMessageLength() <= numSrc1, "extended message length mismatch for raw sends");


    createSplitSendInst(
        predOpnd,
        (modifiers & 1) ? G4_sendsc : G4_sends,
        exsize,
        dstOpnd,
        src0->asSrcRegRegion(),
        src1->asSrcRegRegion(),
        msgDescOpnd,
        inst_opt,
        sendMsgDesc,
        temp_exdesc_src,
        true);

    return VISA_SUCCESS;
}

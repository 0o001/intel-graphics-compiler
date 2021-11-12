/*========================== begin_copyright_notice ============================

Copyright (C) 2020-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include "BuildIR.h"
#include "../Timer.h"

using namespace vISA;


G4_INST* IR_Builder::translateLscFence(
    SFID                    sfid,
    LSC_FENCE_OP            fenceOp,
    LSC_SCOPE               scope,
    int                    &status)
{
    TIME_SCOPE(VISA_BUILDER_IR_CONSTRUCTION);

    status = VISA_SUCCESS;
    auto check =
        [&] (bool z, const char *what) {
        if (!z) {
            MUST_BE_TRUE(false, what);
            status = VISA_FAILURE;
        }
    };

    // NOTE: fence requires 1 register sent and 1 returned for some foolish
    // reason (synchronization requires it), so we must create dummy registers.
    // I'd prefer to use the same register, but vISA blows up
    // if we dare use the same dst as src (old? hardware restriction?),
    // so we'll splurge and use two.
    const RegionDesc *rd = getRegionStride1();

    // G4_Declare *src0DummyRegDecl = createSendPayloadDcl(getGRFSize()/4, Type_UD);
    G4_Declare *src0DummyRegDecl = getBuiltinR0();
    G4_SrcRegRegion *src0Dummy = createSrc(
        src0DummyRegDecl->getRegVar(),
        0, 0, rd, Type_UD);
    //
    // I don't think vISA permits same dst as src0
    // G4_Declare *dstDummyRegDecl = getBuiltinR0();
    G4_DstRegRegion* dstDummy = nullptr;
    if (!hasFenceControl())
    {
        G4_Declare* dstDummyRegDecl = createSendPayloadDcl(getGRFSize() / 4, Type_UD);
        dstDummy = createDstRegRegion(dstDummyRegDecl, 1);
    }
    else
    {
        dstDummy = createNullDst(Type_UD);
    }

    G4_SrcRegRegion *src1NullReg = createNullSrc(Type_UD);
    //
    const int src1Len = 0; // no data needed in src1

    const G4_ExecSize execSize = g4::SIMD1;
    const G4_InstOpts instOpt = Get_Gen4_Emask(vISA_EMASK_M1_NM, execSize);

    ///////////////////////////////////////////////////////////////////////////
    uint32_t desc = 0, exDesc = 0;
    // fence requires 1 non-null register sent and 1 non-null received,
    // but the contents are undefined
    const uint32_t LSC_FENCE_OPCODE = 0x1F;
    desc |= LSC_FENCE_OPCODE; // LSC_FENCE
    desc |= 1 << 25;
    desc |= (hasFenceControl() ? 0 : 1) << 20;
    //
    switch (fenceOp) {
    case LSC_FENCE_OP_NONE:        desc |= 0 << 12; break;
    case LSC_FENCE_OP_EVICT:       desc |= 1 << 12; break;
    case LSC_FENCE_OP_INVALIDATE:  desc |= 2 << 12; break;
    case LSC_FENCE_OP_DISCARD:     desc |= 3 << 12; break;
    case LSC_FENCE_OP_CLEAN:       desc |= 4 << 12; break;
    case LSC_FENCE_OP_FLUSHL3:     desc |= 5 << 12; break;
    case LSC_FENCE_OP_TYPE6:       desc |= 6 << 12; break;
    default: check(false, "invalid fence op");
    }
    switch (scope) {
    case LSC_SCOPE_GROUP:   desc |= 0 << 9; break;
    case LSC_SCOPE_LOCAL:   desc |= 1 << 9; break;
    case LSC_SCOPE_TILE:    desc |= 2 << 9; break;
    case LSC_SCOPE_GPU:     desc |= 3 << 9; break;
    case LSC_SCOPE_GPUS:    desc |= 4 << 9; break;
    case LSC_SCOPE_SYSREL:  desc |= 5 << 9; break;
    case LSC_SCOPE_SYSACQ:  desc |= 6 << 9; break;
    default: check(false, "invalid fence scope");
    }

    if (sfid == SFID::UGM)
    {
        // special token telling EU to route the UGM fence to LSC even in
        // backup mode.  Without bit 18 set, the default behavior is for
        // the UGM fence to be rerouted to HDC when the backup mode chicken
        // bit is set.
        desc |= getOption(vISA_LSCBackupMode) << 18;
    }

    (void) lscEncodeAddrSize(LSC_ADDR_SIZE_32b, desc, status);
    G4_SendDescRaw *msgDesc = createSendMsgDesc(
        sfid,
        desc,
        exDesc,
        src1Len,
        SendAccess::READ_WRITE,
        nullptr);
    G4_InstSend *fenceInst = createLscSendInst(
        nullptr,
        dstDummy,
        src0Dummy,
        src1NullReg,
        execSize,
        msgDesc,
        instOpt,
        LSC_ADDR_TYPE_FLAT,
        true);
    (void)fenceInst;

    return fenceInst;
}

void IR_Builder::generateNamedBarrier(
    int numProducer, int numConsumer,
    NamedBarrierType type, G4_Operand* barrierId)
{
    struct NamedBarrierPayload
    {
        uint32_t id : 8;
        uint32_t fence : 4;
        uint32_t padding : 2;
        uint32_t type : 2;
        uint32_t consumer : 8;
        uint32_t producer: 8;
    };

    union
    {
        NamedBarrierPayload payload;
        uint32_t data;
    } payload;

    payload.data = 0;
    payload.payload.consumer = numConsumer;
    payload.payload.producer = numProducer;

    auto getVal = [](NamedBarrierType type)
    {
        switch (type)
        {
        case NamedBarrierType::BOTH:
            return 0;
        case NamedBarrierType::PRODUCER:
            return 1;
        case NamedBarrierType::CONSUMER:
            return 2;
        default:
            assert(false && "unrecognized NM barreir type");
            return -1;
        }
    };
    payload.payload.type = getVal(type);

    G4_Declare* header = createTempVar(8, Type_UD, GRFALIGN);
    if (barrierId->isImm())
    {
        payload.payload.id = (uint8_t)barrierId->asImm()->getInt();
        auto dst = createDst(header->getRegVar(), 0, 2, 1, Type_UD);
        auto src = createImm(payload.data, Type_UD);
        createMov(g4::SIMD1, dst, src, InstOpt_WriteEnable, true);
    }
    else
    {
        // barrier id should be a srcRegion with int type
        // and (1) Hdr.2:ud barrierId 0xFF
        // or (1) Hdr.2:ud Hdr.2 payload.data
        assert(barrierId->isSrcRegRegion() && IS_INT(barrierId->getType()) && "expect barrier id to be int");
        auto dst = createDst(header->getRegVar(), 0, 2, 1, Type_UD);
        auto src1 = createImm(0xFF, Type_UD);
        createBinOp(G4_and, g4::SIMD1, dst, barrierId, src1, InstOpt_WriteEnable, true);
        dst = createDst(header->getRegVar(), 0, 2, 1, Type_UD);
        auto orSrc0 = createSrc(header->getRegVar(), 0, 2,
            getRegionScalar(), Type_UD);
        auto orSrc1 = createImm(payload.data, Type_UD);
        createBinOp(G4_or, g4::SIMD1, dst, orSrc0, orSrc1, InstOpt_WriteEnable, true);
    }

    // 1 message length, 0 response length, no header, no ack
    int desc = (0x1 << 25) + 0x4;

    auto msgDesc = createSyncMsgDesc(SFID::GATEWAY, desc);
    createSendInst(
        nullptr,
        G4_send,
        g4::SIMD1,
        createNullDst(Type_UD),
        createSrcRegRegion(header, getRegionStride1()),
        createImm(desc, Type_UD),
        InstOpt_WriteEnable,
        msgDesc,
        true);
}

void IR_Builder::generateNamedBarrier(G4_Operand* barrierId, G4_SrcRegRegion* threadCount)
{
    G4_Declare* header = createTempVar(8, Type_UD, GRFALIGN);

    // mov (1) Hdr.2<1>:ud 0x0
    // mov (2) Hdr.10<1>:ub threadcount:ub
    // mov (1) Hdr.8<1>:ub barrierId:ub
    auto dst = createDst(header->getRegVar(), 0, 2, 1, Type_UD);
    auto src = createImm(0, Type_UD);
    createMov(g4::SIMD1, dst, src, InstOpt_WriteEnable, true);
    dst = createDst(header->getRegVar(), 0, 10, 1, Type_UB);
    createMov(g4::SIMD2, dst, threadCount, InstOpt_WriteEnable, true);
    dst = createDst(header->getRegVar(), 0, 8, 1, Type_UB);
    createMov(g4::SIMD1, dst, barrierId, InstOpt_WriteEnable, true);

    // 1 message length, 0 response length, no header, no ack
    int desc = (0x1 << 25) + 0x4;

    auto msgDesc = createSyncMsgDesc(SFID::GATEWAY, desc);
    createSendInst(
        nullptr,
        G4_send,
        g4::SIMD1,
        createNullDst(Type_UD),
        createSrcRegRegion(header, getRegionStride1()),
        createImm(desc, Type_UD),
        InstOpt_WriteEnable,
        msgDesc,
        true);
}

void IR_Builder::generateSingleBarrier()
{
    // single barrier: # producer = # consumer = # threads, barrier id = 0
    // For now produce no fence
    // Number of threads per threadgroup is r0.2[31:24]
    //   mov (1) Hdr.2<1>:ud 0x0
    //   mov (2) Hdr.10<1>:ub R0.11<0;1,0>:ub
    // This SIMD2 byte move is broadcasting the thread group size
    // from the r0 header into both the producer and consumer slots.
    //   Hdr.2:d[31:24,23:16]
    G4_Declare* header = createTempVar(8, Type_UD, GRFALIGN);
    auto dst = createDst(header->getRegVar(), 0, 2, 1, Type_UD);
    auto src = createImm(0, Type_UD);
    createMov(g4::SIMD1, dst, src, InstOpt_WriteEnable, true);
    dst = createDst(header->getRegVar(), 0 , 10, 1, Type_UB);
    auto src0 = createSrc(getBuiltinR0()->getRegVar(), 0, 11,
        getRegionScalar(), Type_UB);
    createMov(g4::SIMD2, dst, src0, InstOpt_WriteEnable, true);
    // 1 message length, 0 response length, no header, no ack
    int desc = (0x1 << 25) + 0x4;

    auto msgDesc = createSyncMsgDesc(SFID::GATEWAY, desc);
    createSendInst(
        nullptr,
        G4_send,
        g4::SIMD1,
        createNullDst(Type_UD),
        createSrcRegRegion(header, getRegionStride1()),
        createImm(desc, Type_UD),
        InstOpt_WriteEnable,
        msgDesc,
        true);
}

static void checkNamedBarrierSrc(G4_Operand* src, bool isBarrierId)
{
    if (src->isImm())
    {
        if (isBarrierId)
        {
            uint32_t val = (uint32_t)src->asImm()->getInt();
            assert(val < 32 && "illegal named barrier id");
        }
    }
    else if (src->isSrcRegRegion())
    {
        assert(src->asSrcRegRegion()->isScalar() && "barrier id should have scalar region");
        assert(IS_BTYPE(src->getType()) && "illegal barrier opperand type");
    }
    else
    {
        assert(false && "illegal barrier id operand");
    }
}

int IR_Builder::translateVISANamedBarrierWait(G4_Operand* barrierId)
{
    TIME_SCOPE(VISA_BUILDER_IR_CONSTRUCTION);

    checkNamedBarrierSrc(barrierId, true);

    G4_Operand* barSrc = barrierId;
    if (barrierId->isSrcRegRegion()) {
        // sync can take only flag src
        G4_Declare* flagDecl = createTempFlag(1);
        createMov(g4::SIMD1, createDstRegRegion(flagDecl, 1), barrierId,
            InstOpt_WriteEnable, true);
        barSrc = createSrcRegRegion(flagDecl, getRegionScalar());
    }
    // wait barrierId
    createInst(nullptr, G4_wait, nullptr, g4::NOSAT, g4::SIMD1, nullptr, barSrc, nullptr,
        InstOpt_WriteEnable, true);

    return VISA_SUCCESS;
}

int IR_Builder::translateVISANamedBarrierSignal(G4_Operand* barrierId, G4_Operand* threadCount)
{
    TIME_SCOPE(VISA_BUILDER_IR_CONSTRUCTION);

    checkNamedBarrierSrc(barrierId, true);
    checkNamedBarrierSrc(threadCount, false);

    if (threadCount->isImm())
    {
        int numThreads = (int)threadCount->asImm()->getInt();
        generateNamedBarrier(numThreads, numThreads, NamedBarrierType::BOTH, barrierId);
    }
    else
    {
        generateNamedBarrier(barrierId, threadCount->asSrcRegRegion());
    }

    return VISA_SUCCESS;
}


// create a fence instruction to the data cache
// flushParam --
//              bit 0 -- commit enable
//              bit 1-4 -- L3 flush parameters
//              bit 5 -- global/SLM
//              bit 6 -- L1 flush
//              bit 7 -- SW fence only; a scheduling barrier but does not generate any code
// bit 7, if set, takes precedence over other bits
G4_INST* IR_Builder::createFenceInstruction(
    uint8_t flushParam, bool commitEnable, bool globalMemFence,
    bool isSendc = false)
{
#define L1_FLUSH_MASK 0x40

    int flushBits = (flushParam >> 1) & 0xF;
    assert(!supportsLSC() && "LSC fence should be handled elsewhere");
    if (noL3Flush())
    {
        // L3 flush is no longer required for image memory
        flushBits = 0;
    }

    bool L1Flush = (flushParam & L1_FLUSH_MASK) != 0 &&
        !(hasSLMFence() && !globalMemFence);

    int desc = 0x7 << 14 | ((commitEnable ? 1 : 0) << 13);

    desc |= flushBits << 9;

    if (L1Flush)
    {
#define L1_FLUSH_BIT_LOC 8
        desc |= 1 << L1_FLUSH_BIT_LOC;
    }

    G4_Declare *srcDcl = getBuiltinR0();
    G4_Declare *dstDcl = createTempVar(8, Type_UD, Any);
    G4_DstRegRegion *sendDstOpnd = commitEnable ? createDstRegRegion(dstDcl, 1) : createNullDst(Type_UD);
    G4_SrcRegRegion *sendSrcOpnd = createSrcRegRegion(srcDcl, getRegionStride1());
    uint8_t BTI = 0x0;

    if (hasSLMFence())
    {
        // we must choose either GLOBAL_MEM_FENCE or SLM_FENCE
        BTI = globalMemFence ? 0 : 0xfe;
    }

    // commitEnable = true: msg length = 1, response length = 1, dst == src
    // commitEnable = false: msg length = 1, response length = 0, dst == null
    return createSendInst(nullptr, sendDstOpnd, sendSrcOpnd, 1, (commitEnable ? 1 : 0), g4::SIMD8,
        desc, SFID::DP_DC0, true, SendAccess::READ_WRITE, createImm(BTI, Type_UD), nullptr, InstOpt_WriteEnable, isSendc);
}

// create a default SLM fence (no flush)
G4_INST* IR_Builder::createSLMFence()
{
    bool commitEnable = needsFenceCommitEnable();
    if (supportsLSC())
    {
        return translateLscFence(SFID::SLM, LSC_FENCE_OP_NONE, LSC_SCOPE_GROUP);
    }
    return createFenceInstruction(0, commitEnable, false, false);
}


int IR_Builder::translateVISAWaitInst(G4_Operand* mask)
{
    TIME_SCOPE(VISA_BUILDER_IR_CONSTRUCTION);

    // clear TDR if mask is not null and not zero
    if (mask && !(mask->isImm() && mask->asImm()->getInt() == 0))
    {
        // mov (1) f0.0<1>:uw <TDR_bits>:ub {NoMask}
        G4_Declare* tmpFlagDcl = createTempFlag(1);
        G4_DstRegRegion* newPredDef = createDstRegRegion(tmpFlagDcl, 1);
        createMov(g4::SIMD1, newPredDef, mask, InstOpt_WriteEnable, true);

        // (f0.0) and (8) tdr0.0<1>:uw tdr0.0<8;8,1>:uw 0x7FFF:uw {NoMask}
        G4_Predicate* predOpnd = createPredicate(PredState_Plus, tmpFlagDcl->getRegVar(), 0, PRED_DEFAULT);
        G4_DstRegRegion* TDROpnd = createDst(phyregpool.getTDRReg(), 0, 0, 1, Type_UW);
        G4_SrcRegRegion* TDRSrc = createSrc(phyregpool.getTDRReg(), 0, 0, getRegionStride1(), Type_UW);
        createInst(predOpnd, G4_and, NULL, g4::NOSAT, g4::SIMD8,
            TDROpnd, TDRSrc, createImm(0x7FFF, Type_UW), InstOpt_WriteEnable, true);
    }

    createIntrinsicInst(nullptr, Intrinsic::Wait, g4::SIMD1,
        nullptr, nullptr, nullptr, nullptr, InstOpt_WriteEnable, true);

    return VISA_SUCCESS;
}


void IR_Builder::generateBarrierSend()
{
    if (hasUnifiedBarrier())
    {
        generateSingleBarrier();
        return;
    }

    // 1 message length, 0 response length, no header, no ack
    int desc = (0x1 << 25) + 0x4;

    //get barrier id
    G4_Declare *dcl = createSendPayloadDcl(GENX_DATAPORT_IO_SZ, Type_UD);

    G4_SrcRegRegion* r0_src_opnd = createSrc(
        builtinR0->getRegVar(),
        0,
        2,
        getRegionScalar(),
        Type_UD);

    G4_DstRegRegion *dst1_opnd = createDstRegRegion(dcl, 1);

    bool enableBarrierInstCounterBits = kernel.getOption(VISA_EnableBarrierInstCounterBits);
    int mask = getBarrierMask(enableBarrierInstCounterBits);

    G4_Imm *g4Imm = createImm(mask, Type_UD);

    createBinOp(
        G4_and,
        g4::SIMD8,
        dst1_opnd,
        r0_src_opnd,
        g4Imm,
        InstOpt_WriteEnable,
        true);

    // Generate the barrier send message
    auto msgDesc = createSyncMsgDesc(SFID::GATEWAY, desc);
    createSendInst(
        NULL,
        G4_send,
        g4::SIMD1,
        createNullDst(Type_UD),
        createSrcRegRegion(dcl, getRegionStride1()),
        createImm(desc, Type_UD),
        InstOpt_WriteEnable,
        msgDesc,
        true);
}

void IR_Builder::generateBarrierWait()
{
    G4_Operand* waitSrc = nullptr;
    if (!hasUnifiedBarrier()) {

        if (getPlatform() < GENX_TGLLP) {
            // before Xe: wait n0.0<0;1,0>:ud
            waitSrc = createSrc(phyregpool.getN0Reg(),
                0, 0, getRegionScalar(), Type_UD);
        } else {
            // Xe: sync.bar null
            waitSrc = createNullSrc(Type_UD);
        }
    }
    else {
        if (getPlatform() >= GENX_PVC) {
            // PVC: sync.bar 0
            waitSrc = createImm(0, Type_UD);
        } else {
            // DG2: sync.bar null
            waitSrc = createNullSrc(Type_UD);
        }
    }
    createInst(nullptr, G4_wait, nullptr, g4::NOSAT, g4::SIMD1,
        nullptr, waitSrc, nullptr, InstOpt_WriteEnable, true);
}

int IR_Builder::translateVISASyncInst(ISA_Opcode opcode, unsigned int mask)
{
    TIME_SCOPE(VISA_BUILDER_IR_CONSTRUCTION);

    switch (opcode)
    {
    case ISA_BARRIER:
    {
        generateBarrierSend();
        generateBarrierWait();
    }
    break;
    case ISA_SAMPLR_CACHE_FLUSH:
    {
        // msg length = 1, response length = 1, header_present = 1,
        // Bit 16-12 = 11111 for Sampler Message Type
        // Bit 18-17 = 11 for SIMD32 mode
        int desc = (1 << 25) + (1 << 20) + (1 << 19) + (0x3 << 17) + (0x1F << 12);

        G4_Declare *dcl = getBuiltinR0();
        G4_Declare *dstDcl = createTempVar(8, Type_UD, Any);
        G4_DstRegRegion* sendDstOpnd = createDstRegRegion(dstDcl, 1);
        G4_SrcRegRegion* sendMsgOpnd = createSrcRegRegion(dcl, getRegionStride1());

        auto msgDesc = createSyncMsgDesc(SFID::SAMPLER, desc);
        createSendInst(nullptr, G4_send, g4::SIMD8, sendDstOpnd, sendMsgOpnd,
            createImm(desc, Type_UD), 0, msgDesc, true);

        G4_SrcRegRegion* moveSrcOpnd = createSrc(dstDcl->getRegVar(), 0, 0, getRegionStride1(), Type_UD);
        createMovInst(dstDcl, 0, 0, g4::SIMD8, NULL, NULL, moveSrcOpnd);
    }
    break;
    case ISA_WAIT:
    {
        //This should be handled by translateVISAWait() now
        MUST_BE_TRUE(false, "Should not reach here");
    }
    break;
    case ISA_YIELD:
    {
        G4_INST* lastInst = instList.empty() ? nullptr : instList.back();
        if (lastInst && lastInst->opcode() != G4_label)
        {
            lastInst->setOptionOn(InstOpt_Switch);
        }
        else
        {
            // dummy move to apply the {switch}
            G4_SrcRegRegion* srcOpnd = createSrc(getBuiltinR0()->getRegVar(), 0, 0, getRegionScalar(), Type_UD);
            G4_DstRegRegion* dstOpnd = createDst(getBuiltinR0()->getRegVar(), 0, 0, 1, Type_UD);

            G4_INST* nop = createMov(g4::SIMD1, dstOpnd, srcOpnd, InstOpt_NoOpt, true);
            nop->setOptionOn(InstOpt_Switch);
        }
    }
    break;
    case ISA_FENCE:
    {
#define GLOBAL_MASK 0x20
        union fenceParam
        {
            VISAFenceMask mask;
            uint8_t data;
        };

        fenceParam fenceMask;
        fenceMask.data = mask & 0xFF;
        bool globalFence = (mask & GLOBAL_MASK) == 0;

        if (fenceMask.mask.SWFence)
        {
            createIntrinsicInst(
                nullptr, Intrinsic::MemFence, g4::SIMD1,
                nullptr, nullptr, nullptr, nullptr, InstOpt_NoOpt, true);
        }
        else if (VISA_WA_CHECK(m_pWaTable, WADisableWriteCommitForPageFault))
        {
            // write commit does not work under page fault
            // so we generate a fence without commit, followed by a read surface info to BTI 0
            createFenceInstruction((uint8_t) mask & 0xFF, false, globalFence);
            G4_Imm* surface = createImm(0, Type_UD);
            G4_Declare* zeroLOD = createTempVar(8, Type_UD, Any);
            createMovInst(zeroLOD, 0, 0, g4::SIMD8, NULL, NULL, createImm(0, Type_UD));
            G4_SrcRegRegion* sendSrc = createSrcRegRegion(zeroLOD, getRegionStride1());
            G4_DstRegRegion* sendDst = createDstRegRegion(zeroLOD, 1);
            ChannelMask maskR = ChannelMask::createFromAPI(CHANNEL_MASK_R);
            translateVISAResInfoInst(EXEC_SIZE_8, vISA_EMASK_M1, maskR, surface, sendSrc, sendDst);
        }
        else if (supportsLSC())
        {
            // translate legacy fence into the LSC fence
            // for local fence we translate into a SLM fence with TG scope
            // for global fence we translate into a untyped and typed fence with GPU scope
            // ToDo: may need a global flag to let user control the fence scope
            if (globalFence)
            {
                auto fenceControl = supportsSampler() ? LSC_FENCE_OP_EVICT : LSC_FENCE_OP_NONE;
                if (fenceMask.mask.flushRWCache)
                {
                    fenceControl = LSC_FENCE_OP_FLUSHL3;
                }
                translateLscFence(SFID::UGM, fenceControl, LSC_SCOPE_GPU);
                translateLscFence(SFID::TGM, fenceControl, LSC_SCOPE_GPU);
            }
            else
            {
                translateLscFence(SFID::SLM, LSC_FENCE_OP_NONE, LSC_SCOPE_GROUP);
            }
        }
        else
        {
            createFenceInstruction((uint8_t) mask & 0xFF, (mask & 0x1) == 0x1, globalFence);
            // The move to ensure the fence is actually complete will be added at the end of compilation,
            // in Optimizer::HWWorkaround()
        }
        break;
    }
    default:
        return VISA_FAILURE;
    }

    return VISA_SUCCESS;
}

int IR_Builder::translateVISASplitBarrierInst(bool isSignal)
{
    TIME_SCOPE(VISA_BUILDER_IR_CONSTRUCTION);

    if (isSignal)
    {
        generateBarrierSend();
    }
    else
    {
        generateBarrierWait();
    }

    return VISA_SUCCESS;
}

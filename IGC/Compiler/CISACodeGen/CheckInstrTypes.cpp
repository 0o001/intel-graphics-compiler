
/*========================== begin_copyright_notice ============================

Copyright (c) 2000-2021 Intel Corporation

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

#include "Compiler/CISACodeGen/CheckInstrTypes.hpp"
#include "Compiler/CISACodeGen/helper.h"
#include "Compiler/CodeGenPublic.h"

#include "common/debug/Debug.hpp"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include "common/LLVMWarningsPop.hpp"
#include "GenISAIntrinsics/GenIntrinsicInst.h"

using namespace llvm;
using namespace IGC;
using namespace GenISAIntrinsic;

#define PASS_FLAG "CheckInstrTypes"
#define PASS_DESCRIPTION "Check individual type of instructions"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS true
IGC_INITIALIZE_PASS_BEGIN(CheckInstrTypes, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_END(CheckInstrTypes, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char CheckInstrTypes::ID = 0;

CheckInstrTypes::CheckInstrTypes(IGC::SInstrTypes* instrList) : FunctionPass(ID), g_InstrTypes(instrList)
{
    initializeLoopInfoWrapperPassPass(*PassRegistry::getPassRegistry());
    initializeCheckInstrTypesPass(*PassRegistry::getPassRegistry());

    instrList->CorrelatedValuePropagationEnable = false;
    instrList->hasLoop = false;
    instrList->hasMultipleBB = false;
    instrList->hasCmp = false;
    instrList->hasSwitch = false;
    instrList->hasPhi = false;
    instrList->hasLoadStore = false;
    instrList->hasCall = false;
    instrList->hasIndirectCall = false;
    instrList->hasInlineAsm = false;
    instrList->hasInlineAsmPointerAccess = false;
    instrList->hasIndirectBranch = false;
    instrList->hasFunctionAddressTaken = false;
    instrList->hasSel = false;
    instrList->hasPointer = false;
    instrList->hasGenericAddressSpacePointers = false;
    instrList->hasLocalLoadStore = false;
    instrList->hasGlobalLoad = false;
    instrList->hasGlobalStore = false;
    instrList->hasStorageBufferLoad = false;
    instrList->hasStorageBufferStore = false;
    instrList->hasSubroutines = false;
    instrList->hasPrimitiveAlloca = false;
    instrList->hasNonPrimitiveAlloca = false;
    instrList->hasReadOnlyArray = false;
    instrList->hasBuiltin = false;
    instrList->hasFRem = false;
    instrList->psHasSideEffect = false;
    instrList->hasDebugInfo = false;
    instrList->hasAtomics = false;
    instrList->hasBarrier = false;
    instrList->hasDiscard = false;
    instrList->hasTypedRead = false;
    instrList->hasTypedwrite = false;
    instrList->mayHaveIndirectOperands = false;
    instrList->mayHaveIndirectResources = false;
    instrList->hasUniformAssumptions = false;
    instrList->hasWaveIntrinsics = false;
    instrList->numPsInputs = 0;
    instrList->numSample = 0;
    instrList->numBB = 0;
    instrList->numLoopInsts = 0;
    instrList->numOfLoop = 0;
    instrList->numInsts = 0;
    instrList->numAllocaInsts = 0;
    instrList->sampleCmpToDiscardOptimizationPossible = false;
    instrList->sampleCmpToDiscardOptimizationSlot = 0;
    instrList->hasPullBary = false;
    instrList->hasDynamicGenericLoadStore = false;
}

void CheckInstrTypes::SetLoopFlags(Function& F)
{
    LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();

    if (!(LI->empty()))
    {
        // find how many instructions are used in loop
        for (auto it = LI->begin(); it != LI->end(); it++)
        {
            g_InstrTypes->numOfLoop++;
            Loop* L = (*it);
            for (uint i = 0; i < L->getNumBlocks(); i++)
            {
                g_InstrTypes->numLoopInsts += L->getBlocks()[i]->getInstList().size();
            }
        }
    }
}

bool CheckInstrTypes::runOnFunction(Function& F)
{
    LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    g_InstrTypes->hasLoop |= !(LI->empty());

    // check if module has debug info
    g_InstrTypes->hasDebugInfo = F.getParent()->getNamedMetadata("llvm.dbg.cu") != nullptr;

    visit(F);
    SetLoopFlags(F);
    return false;
}

void CheckInstrTypes::visitInstruction(llvm::Instruction& I)
{
    if (!llvm::isa<llvm::DbgInfoIntrinsic>(&I))
    {
        g_InstrTypes->numInsts++;
    }

    if (I.getOpcode() == Instruction::FRem)
    {
        g_InstrTypes->hasFRem = true;
    }

    auto PT = dyn_cast<PointerType>(I.getType());
    if (PT && PT->getPointerAddressSpace() == ADDRESS_SPACE_GENERIC)
    {
        g_InstrTypes->hasGenericAddressSpacePointers = true;
    }
}

void CheckInstrTypes::visitCallInst(CallInst& C)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasCall = true;

    Function* calledFunc = C.getCalledFunction();

    if (calledFunc == NULL)
    {
        if (C.isInlineAsm())
        {
            g_InstrTypes->hasInlineAsm = true;
            for (unsigned i = 0; i < C.getNumArgOperands(); i++)
            {
                Type* opndTy = C.getArgOperand(i)->getType();
                if (opndTy->isPointerTy() &&
                    (cast<PointerType>(opndTy)->getAddressSpace() == ADDRESS_SPACE_GLOBAL ||
                        cast<PointerType>(opndTy)->getAddressSpace() == ADDRESS_SPACE_CONSTANT))
                {
                    // If an inline asm call directly accesses a pointer, we need to enable
                    // bindless/stateless support since user does not know the BTI the
                    // resource is bound to.
                    g_InstrTypes->hasInlineAsmPointerAccess = true;
                }
            }
            return;
        }
        // calls to 'blocks' have a null Function object
        g_InstrTypes->hasSubroutines = true;
        g_InstrTypes->hasIndirectCall = true;
    }
    else if (!calledFunc->isDeclaration())
    {
        g_InstrTypes->hasSubroutines = true;
    }
    if (C.mayWriteToMemory())
    {
        if (GenIntrinsicInst * CI = dyn_cast<GenIntrinsicInst>(&C))
        {
            GenISAIntrinsic::ID IID = CI->getIntrinsicID();
            if (IID != GenISA_OUTPUT && IID != GenISA_discard)
            {
                g_InstrTypes->psHasSideEffect = true;
            }
        }
    }

    if (isSampleLoadGather4InfoInstruction(&C))
    {
        g_InstrTypes->numSample++;
    }

    if (GenIntrinsicInst * CI = llvm::dyn_cast<GenIntrinsicInst>(&C))
    {
        switch (CI->getIntrinsicID())
        {
        case GenISAIntrinsic::GenISA_atomiccounterinc:
        case GenISAIntrinsic::GenISA_atomiccounterpredec:
        case GenISAIntrinsic::GenISA_icmpxchgatomicraw:
        case GenISAIntrinsic::GenISA_icmpxchgatomicrawA64:
        case GenISAIntrinsic::GenISA_cmpxchgatomicstructured:
        case GenISAIntrinsic::GenISA_icmpxchgatomictyped:
        case GenISAIntrinsic::GenISA_intatomicraw:
        case GenISAIntrinsic::GenISA_intatomicrawA64:
        case GenISAIntrinsic::GenISA_dwordatomicstructured:
        case GenISAIntrinsic::GenISA_intatomictyped:
        case GenISAIntrinsic::GenISA_fcmpxchgatomicraw:
        case GenISAIntrinsic::GenISA_fcmpxchgatomicrawA64:
        case GenISAIntrinsic::GenISA_fcmpxchgatomicstructured:
        case GenISAIntrinsic::GenISA_floatatomicraw:
        case GenISAIntrinsic::GenISA_floatatomicrawA64:
        case GenISAIntrinsic::GenISA_floatatomicstructured:
            g_InstrTypes->hasAtomics = true;
            break;
        case GenISAIntrinsic::GenISA_discard:
            g_InstrTypes->hasDiscard = true;
            break;
        case GenISAIntrinsic::GenISA_WaveShuffleIndex:
            g_InstrTypes->mayHaveIndirectOperands = true;
            g_InstrTypes->hasWaveIntrinsics = true;
            break;
        case GenISAIntrinsic::GenISA_threadgroupbarrier:
            g_InstrTypes->hasBarrier = true;
            break;
        case GenISAIntrinsic::GenISA_is_uniform:
            g_InstrTypes->hasUniformAssumptions = true;
            break;
        case GenISAIntrinsic::GenISA_typedread:
            g_InstrTypes->hasTypedRead = true;
            break;
        case GenISAIntrinsic::GenISA_typedwrite:
            g_InstrTypes->hasTypedwrite = true;
            break;
        case GenISAIntrinsic::GenISA_WaveAll:
        case GenISAIntrinsic::GenISA_WaveBallot:
        case GenISAIntrinsic::GenISA_wavebarrier:
        case GenISAIntrinsic::GenISA_WaveInverseBallot:
        case GenISAIntrinsic::GenISA_WavePrefix:
        case GenISAIntrinsic::GenISA_WaveClustered:
        case GenISAIntrinsic::GenISA_QuadPrefix:
        case GenISAIntrinsic::GenISA_simdShuffleDown:
            g_InstrTypes->hasWaveIntrinsics = true;
            break;
        case GenISAIntrinsic::GenISA_DCL_inputVec:
        case GenISAIntrinsic::GenISA_DCL_ShaderInputVec:
            g_InstrTypes->numPsInputs++;
            break;
        case GenISAIntrinsic::GenISA_PullSampleIndexBarys:
        case GenISAIntrinsic::GenISA_PullSnappedBarys:
        case GenISAIntrinsic::GenISA_PullCentroidBarys:
            g_InstrTypes->hasPullBary = true;
            break;
        case GenISAIntrinsic::GenISA_ldraw_indexed:
        case GenISAIntrinsic::GenISA_ldrawvector_indexed:
        {
            BufferType bufferType = DecodeBufferType(
                CI->getArgOperand(0)->getType()->getPointerAddressSpace());
            if (bufferType == UAV || bufferType == BINDLESS)
            {
                g_InstrTypes->hasStorageBufferLoad = true;
            }
            break;
        }
        case GenISAIntrinsic::GenISA_storeraw_indexed:
        case GenISAIntrinsic::GenISA_storerawvector_indexed:
        {
            BufferType bufferType = DecodeBufferType(
                CI->getArgOperand(0)->getType()->getPointerAddressSpace());
            if (bufferType == UAV || bufferType == BINDLESS)
            {
                g_InstrTypes->hasStorageBufferStore = true;
            }
            break;
        }
        default:
            break;
        }

        Value* resourcePtr = GetBufferOperand(CI);
        if (resourcePtr == nullptr)
        {
            Value* samplerPtr = nullptr;
            getTextureAndSamplerOperands(CI, resourcePtr, samplerPtr);
        }
        if (resourcePtr &&
            resourcePtr->getType()->isPointerTy() &&
            isStatefulAddrSpace(resourcePtr->getType()->getPointerAddressSpace()) &&
            !IsDirectIdx(resourcePtr->getType()->getPointerAddressSpace()))
        {
            g_InstrTypes->mayHaveIndirectResources = true;
        }
    }
}

void CheckInstrTypes::visitBranchInst(BranchInst& I)
{
    g_InstrTypes->numInsts++;
}

void CheckInstrTypes::visitSwitchInst(SwitchInst& I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasSwitch = true;
}

void CheckInstrTypes::visitIndirectBrInst(IndirectBrInst& I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasIndirectBranch = true;
}

void CheckInstrTypes::visitICmpInst(ICmpInst& I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasCmp = true;
}

void CheckInstrTypes::visitFCmpInst(FCmpInst& I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasCmp = true;
}

void CheckInstrTypes::visitAllocaInst(AllocaInst& I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->numAllocaInsts++;
    if (I.isArrayAllocation() ||
        I.getAllocatedType()->isArrayTy() ||
        I.getAllocatedType()->isStructTy() ||
        I.getAllocatedType()->isVectorTy())
    {
        g_InstrTypes->hasNonPrimitiveAlloca = true;
    }
    else
    {
        g_InstrTypes->hasPrimitiveAlloca = true;
    }

    if (I.getMetadata("igc.read_only_array"))
    {
        g_InstrTypes->hasReadOnlyArray = true;
    }

    auto PT = dyn_cast<PointerType>(I.getAllocatedType());
    if (PT && PT->getPointerAddressSpace() == ADDRESS_SPACE_GENERIC)
    {
        g_InstrTypes->hasGenericAddressSpacePointers = true;
    }
}

void CheckInstrTypes::visitLoadInst(LoadInst& I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasLoadStore = true;
    uint as = I.getPointerAddressSpace();
    switch (as)
    {
    case ADDRESS_SPACE_LOCAL:
        g_InstrTypes->hasLocalLoadStore = true;
        break;
    case ADDRESS_SPACE_GENERIC:
        g_InstrTypes->hasGenericAddressSpacePointers = true;
        break;
    case ADDRESS_SPACE_GLOBAL:
        g_InstrTypes->hasGlobalLoad = true;
        break;
    default:
    {
        BufferType bufferType = DecodeBufferType(as);
        if (bufferType == UAV || bufferType == BINDLESS)
        {
            g_InstrTypes->hasStorageBufferLoad = true;
        }
        if (isStatefulAddrSpace(as) && !IsDirectIdx(as))
        {
            g_InstrTypes->mayHaveIndirectResources = true;
        }
        break;
    }
    }
}

void CheckInstrTypes::visitStoreInst(StoreInst& I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasLoadStore = true;
    uint as = I.getPointerAddressSpace();
    if (as != ADDRESS_SPACE_PRIVATE)
    {
        g_InstrTypes->psHasSideEffect = true;
    }
    switch (as)
    {
    case ADDRESS_SPACE_LOCAL:
        g_InstrTypes->hasLocalLoadStore = true;
        break;
    case ADDRESS_SPACE_GENERIC:
        g_InstrTypes->hasGenericAddressSpacePointers = true;
        break;
    case ADDRESS_SPACE_GLOBAL:
        g_InstrTypes->hasGlobalStore = true;
        break;
    default:
    {
        BufferType bufferType = DecodeBufferType(as);
        if (bufferType == UAV || bufferType == BINDLESS)
        {
            g_InstrTypes->hasStorageBufferStore = true;
        }
        if (isStatefulAddrSpace(as) && !IsDirectIdx(as))
        {
            g_InstrTypes->mayHaveIndirectResources = true;
        }
        break;
    }
    }
}

void CheckInstrTypes::visitPHINode(PHINode& PN)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasPhi = true;
}

void CheckInstrTypes::visitSelectInst(SelectInst& I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasSel = true;
}

void CheckInstrTypes::visitGetElementPtrInst(llvm::GetElementPtrInst& I)
{
    g_InstrTypes->numInsts++;
    if (I.getPointerAddressSpace() == ADDRESS_SPACE_GENERIC)
    {
        g_InstrTypes->hasGenericAddressSpacePointers = true;
    }
}

#undef PASS_FLAG
#undef PASS_DESCRIPTION
#undef PASS_CFG_ONLY
#undef PASS_ANALYSIS

#define PASS_FLAG "InstrStatistic"
#define PASS_DESCRIPTION "Check individual type of instructions"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(InstrStatistic, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_END(InstrStatistic, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char InstrStatistic::ID = 0;

InstrStatistic::InstrStatistic(CodeGenContext* ctx, InstrStatTypes type, InstrStatStage stage, int threshold) :
    FunctionPass(ID), m_ctx(ctx), m_type(type), m_stage(stage), m_threshold(threshold)
{
    initializeInstrStatisticPass(*PassRegistry::getPassRegistry());
    initializeLoopInfoWrapperPassPass(*PassRegistry::getPassRegistry());

    if (stage == InstrStatStage::BEGIN)
    {
        m_ctx->instrStat[type][InstrStatStage::BEGIN] = 0;
        m_ctx->instrStat[type][InstrStatStage::END] = 0;
        m_ctx->instrStat[type][InstrStatStage::EXCEED_THRESHOLD] = 0;
    }
}

bool InstrStatistic::runOnFunction(Function& F)
{
    bool changed = false;

    if (m_type == LICM_STAT) {
        m_LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
        changed = parseLoops();
    }
    else {
        // run the pass
        visit(F);
    }

    // if this is a call for ending statistic, find out if the difference exceeds the threshold.
    if (m_stage == InstrStatStage::END)
    {
        if (m_ctx->instrStat[m_type][InstrStatStage::BEGIN] - m_ctx->instrStat[m_type][InstrStatStage::END] > m_threshold)
        {
            m_ctx->instrStat[m_type][InstrStatStage::EXCEED_THRESHOLD] = 1;
        }

        if (m_type == SROA_PROMOTED)
        {
            m_ctx->m_retryManager.Disable();
        }
    }

    return changed;
}

void InstrStatistic::visitInstruction(llvm::Instruction& I)
{
}

void InstrStatistic::visitLoadInst(LoadInst& I)
{
    if (m_type == SROA_PROMOTED)
        m_ctx->instrStat[m_type][m_stage]++;
}

void InstrStatistic::visitStoreInst(StoreInst& I)
{
    if (m_type == SROA_PROMOTED)
        m_ctx->instrStat[m_type][m_stage]++;
}

bool InstrStatistic::parseLoops()
{
    bool changed = false;

    for (auto& LI : *m_LI)
    {
        Loop* L1 = &(*LI);
        changed |= parseLoop(L1);

        for (auto& L2 : L1->getSubLoops())
        {
            changed |= parseLoop(L2);
        }
    }

    return changed;
}

bool InstrStatistic::parseLoop(Loop* loop)
{
    auto* header = loop->getHeader();
    m_ctx->instrStat[m_type][m_stage] += header->size();

    return false;
}

/*========================== begin_copyright_notice ============================

Copyright (C) 2019-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include "PromoteStatelessToBindless.h"
#include "AdaptorCommon/ImplicitArgs.hpp"
#include "Compiler/IGCPassSupport.h"
#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include "common/LLVMWarningsPop.hpp"
#include "common/IGCIRBuilder.h"
#include "Compiler/CISACodeGen/helper.h"
#include "Compiler/CISACodeGen/OpenCLKernelCodeGen.hpp"
#include "Probe/Assertion.h"
using namespace IGC::IGCMD;

using namespace llvm;
using namespace IGC;
using namespace GenISAIntrinsic;

// Register pass to igc-opt
#define PASS_FLAG "igc-promote-stateless-to-bindless"
#define PASS_DESCRIPTION "Pass promotes stateless accesses to bindless accesses"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(PromoteStatelessToBindless, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(CodeGenContextWrapper)
IGC_INITIALIZE_PASS_END(PromoteStatelessToBindless, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char PromoteStatelessToBindless::ID = 0;

PromoteStatelessToBindless::PromoteStatelessToBindless()
    : FunctionPass(ID),
    m_PrintfBuffer(nullptr)
{
    initializePromoteStatelessToBindlessPass(*PassRegistry::getPassRegistry());
}

bool PromoteStatelessToBindless::runOnFunction(Function& F)
{
    CodeGenContext* ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    auto ClContext = static_cast<OpenCLProgramContext*>(ctx);

    bool HasStackCall = F.hasFnAttribute("visaStackCall");
    // Skip functions marked with stackcall.
    if (HasStackCall)
        return false;

    m_AccessToSrcPtrMap.clear();
    m_AddressUsedSrcPtrMap.clear();
    if (!ClContext->m_InternalOptions.UseBindlessPrintf)
    {
        CheckPrintfBuffer(F);
    }
    visit(F);
    PromoteStatelessToBindlessBuffers(F);

    return true;
}

void PromoteStatelessToBindless::visitInstruction(Instruction& I)
{
    Value* bufptr = IGC::GetBufferOperand(&I);

    if (bufptr && bufptr->getType()->isPointerTy())
    {
        GetAccessInstToSrcPointerMap(&I, bufptr);
    }
}

void PromoteStatelessToBindless::CheckPrintfBuffer(Function& F)
{
    MetaDataUtils* MdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    ImplicitArgs implicitArgs(F, MdUtils);

    m_PrintfBuffer = implicitArgs.getImplicitArgValue(F, ImplicitArg::PRINTF_BUFFER, MdUtils);
}

void PromoteStatelessToBindless::GetAccessInstToSrcPointerMap(Instruction* inst, Value* resourcePtr)
{
    unsigned addrSpace = resourcePtr->getType()->getPointerAddressSpace();

    if (addrSpace != ADDRESS_SPACE_GLOBAL && addrSpace != ADDRESS_SPACE_CONSTANT)
    {
        // Only try to promote stateless buffer pointers ( as(1) or as(2) )
        return;
    }

    //We only support LoadInst, StoreInst, GenISA_simdBlockRead, and GenISA_simdBlockWrite intrinsic
    if (!isa<LoadInst>(inst) && !isa<StoreInst>(inst))
    {
        if (GenIntrinsicInst * GInst = dyn_cast<GenIntrinsicInst>(inst))
        {
            switch (GInst->getIntrinsicID())
            {
            case GenISAIntrinsic::GenISA_simdBlockRead:
            case GenISAIntrinsic::GenISA_simdBlockWrite:
                break;
            case GenISAIntrinsic::GenISA_intatomicrawA64:
                // Ignore a buffer in this intrinsic, keep it stateless.
                return;
            default:
                IGC_ASSERT_MESSAGE(0, "Unsupported Instruction");
                return;
            }
        }
        else
            return;
    }

    std::vector<Value*> tempList;
    Value* srcPtr = IGC::TracePointerSource(resourcePtr, false, true, true, tempList);

    if (!srcPtr ||
        !srcPtr->getType()->isPointerTy() ||
        !isa<Argument>(srcPtr))
    {
        // Cannot trace the resource pointer back to it's source, cannot promote
        return;
    }

    if (m_PrintfBuffer && srcPtr == m_PrintfBuffer)
    {
        // Process PrintfBuffer separately. Printf implementation required operations with
        // printf buffer address (through atomic add), see printf implementation in
        // OpenCLPrintfResolution.cpp. Currently keep printf implementation as stateless and
        // thus skip printf buffer for now.
        return;
    }

    m_promotedArgs.insert(cast<Argument>(srcPtr)->getArgNo());

    // Save the instruction, which makes access (load/store/intrinsic) to the buffer
    m_AccessToSrcPtrMap[inst] = srcPtr;
    // Save the instruction, which generate an address of the buffer. This is the
    // instruction right before the last one. The last one has to be the buffer itself.
    if (tempList.size() > 1)
    {
        m_AddressUsedSrcPtrMap[tempList[tempList.size()-2]] = srcPtr;
    }
    else
    {
        m_AddressUsedSrcPtrMap[inst] = srcPtr;
    }
}

void PromoteStatelessToBindless::PromoteStatelessToBindlessBuffers(Function& F) const
{
    CodeGenContext* ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    ModuleMetaData* modMD = getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();
    MetaDataUtils * pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    ImplicitArgs implicitArgs(F, pMdUtils);

    if (modMD->FuncMD.find(&F) == modMD->FuncMD.end())
        return;

    FunctionMetaData* funcMD = &modMD->FuncMD[&F];
    ResourceAllocMD* resourceAlloc = &funcMD->resAllocMD;

    bool supportDynamicBTIsAllocation = ctx->platform.supportDynamicBTIsAllocation();

    // Modify the reference to the buffer not through all users but only in instructions
    // which are used in accesing (load/store) the buffer.
    for (auto inst : m_AddressUsedSrcPtrMap)
    {
        Instruction* accessInst = cast<Instruction>(inst.first);
        Argument* srcPtr = cast<Argument>(inst.second);

        Value* nullSrcPtr = ConstantPointerNull::get(cast<PointerType>(srcPtr->getType()));
        accessInst->replaceUsesOfWith(srcPtr, nullSrcPtr);

        ArgAllocMD* argInfo = &resourceAlloc->argAllocMDList[srcPtr->getArgNo()];
        IGC_ASSERT_MESSAGE((size_t)srcPtr->getArgNo() < resourceAlloc->argAllocMDList.size(), "ArgAllocMD List Out of Bounds");
        // Update metadata to show bindless resource type
        argInfo->type = ResourceTypeEnum::BindlessUAVResourceType;
        if (supportDynamicBTIsAllocation)
        {
            argInfo->indexType =
                resourceAlloc->uavsNumType +
                (unsigned)std::distance(m_promotedArgs.begin(), m_promotedArgs.find(srcPtr->getArgNo()));
        }
    }

    if(supportDynamicBTIsAllocation)
        resourceAlloc->uavsNumType += m_promotedArgs.size();

    for (auto inst : m_AccessToSrcPtrMap)
    {
        Instruction* accessInst = cast<Instruction>(inst.first);
        Argument* srcPtr = cast<Argument>(inst.second);

        // Get the base bindless pointer
        IGCIRBuilder<> builder(accessInst);
        Value* resourcePtr = IGC::GetBufferOperand(accessInst);
        IGC_ASSERT(resourcePtr);
        unsigned bindlessAS = IGC::EncodeAS4GFXResource(*UndefValue::get(builder.getInt32Ty()), IGC::BINDLESS);
        PointerType* basePointerType = PointerType::get(IGCLLVM::getNonOpaquePtrEltTy(resourcePtr->getType()), bindlessAS);
        Value* bufferOffset = builder.CreatePtrToInt(resourcePtr, builder.getInt32Ty());

        Value* basePointer = nullptr;
        if (!modMD->compOpt.UseLegacyBindlessMode) {
            Argument * srcOffset = implicitArgs.getNumberedImplicitArg(F, ImplicitArg::BINDLESS_OFFSET, srcPtr->getArgNo());
            basePointer = builder.CreateIntToPtr(srcOffset, basePointerType);
        } else {
            basePointer = builder.CreatePointerCast(srcPtr, basePointerType);
        }

        if (LoadInst * load = dyn_cast<LoadInst>(accessInst))
        {
            Value* ldraw = IGC::CreateLoadRawIntrinsic(load, cast<Instruction>(basePointer), bufferOffset);
            load->replaceAllUsesWith(ldraw);
            load->eraseFromParent();
        }
        else if (StoreInst * store = dyn_cast<StoreInst>(accessInst))
        {
            IGC::CreateStoreRawIntrinsic(store, cast<Instruction>(basePointer), bufferOffset);
            store->eraseFromParent();
        }
        else if (GenIntrinsicInst * pIntr = dyn_cast<GenIntrinsicInst>(accessInst))
        {
            if (pIntr->getIntrinsicID() == GenISAIntrinsic::GenISA_simdBlockRead)
            {
                Function* newBlockReadFunc = GenISAIntrinsic::getDeclaration(F.getParent(),
                    GenISAIntrinsic::GenISA_simdBlockReadBindless,
                    { accessInst->getType(), basePointer->getType(),Type::getInt32Ty(accessInst->getContext()) });
                Instruction* newBlockRead = CallInst::Create(newBlockReadFunc, { basePointer, bufferOffset }, "", accessInst);
                newBlockRead->setDebugLoc(pIntr->getDebugLoc());
                accessInst->replaceAllUsesWith(newBlockRead);
                accessInst->eraseFromParent();
            }
            else if (pIntr->getIntrinsicID() == GenISAIntrinsic::GenISA_simdBlockWrite)
            {
                Function* newBlockWriteFunc = GenISAIntrinsic::getDeclaration(F.getParent(),
                    GenISAIntrinsic::GenISA_simdBlockWriteBindless,
                    { basePointer->getType(), pIntr->getOperand(1)->getType(), Type::getInt32Ty(accessInst->getContext()) });
                Instruction* newBlockWrite = CallInst::Create(newBlockWriteFunc, { basePointer, pIntr->getOperand(1), bufferOffset }, "", accessInst);
                newBlockWrite->setDebugLoc(pIntr->getDebugLoc());
                accessInst->replaceAllUsesWith(newBlockWrite);
                accessInst->eraseFromParent();
            }
        }
    }
}

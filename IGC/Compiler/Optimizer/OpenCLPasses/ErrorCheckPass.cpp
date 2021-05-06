/*========================== begin_copyright_notice ============================

Copyright (C) 2018-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include "Compiler/Optimizer/OpenCLPasses/ErrorCheckPass.h"
#include "Compiler/Optimizer/OpenCLPasses/KernelArgs.hpp"
#include "Compiler/IGCPassSupport.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/Pass.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include "common/LLVMWarningsPop.hpp"


using namespace llvm;
using namespace IGC;

char ErrorCheck::ID = 0;

// Register pass to igc-opt
#define PASS_FLAG "igc-error-check"
#define PASS_DESCRIPTION "Check for input errors"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(ErrorCheck, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(CodeGenContextWrapper)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_END(ErrorCheck, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

ErrorCheck::ErrorCheck(void) : FunctionPass(ID)
{
    initializeErrorCheckPass(*PassRegistry::getPassRegistry());
}

bool ErrorCheck::runOnFunction(Function& F)
{
    // add more checks as needed later
    visit(F);

    if (isEntryFunc(getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils(), &F))
    {
        checkArgsSize(F);
    }

    return m_hasError;
}

void ErrorCheck::checkArgsSize(Function& F)
{
    auto Ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    auto MdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    auto ModMD = getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();

    auto DL = F.getParent()->getDataLayout();
    KernelArgs KernelArgs(F, &DL, MdUtils, ModMD, Ctx->platform.getGRFSize());
    auto MaxParamSize = Ctx->platform.getMaxOCLParameteSize();
    uint64_t TotalSize = 0;

    if (KernelArgs.empty())
    {
        return;
    }

    for (auto& KernelArg : KernelArgs)
    {
        auto Arg = KernelArg.getArg();
        Type* ArgType = Arg->getType();
        ArgType = Arg->hasByValAttr() ? ArgType->getPointerElementType() : ArgType;
        if (!KernelArg.isImplicitArg())
        {
            TotalSize += DL.getTypeAllocSize(ArgType);
        }
    }

    if (TotalSize > MaxParamSize)
    {
        std::string ErrorMsg = "Total size of kernel arguments exceeds limit! Total arguments size: "
            + std::to_string(TotalSize) + ", limit: " + std::to_string(MaxParamSize);

        Ctx->EmitError(ErrorMsg.c_str(), &F);
        m_hasError = true;
    }
}

void ErrorCheck::visitInstruction(llvm::Instruction& I)
{
    auto ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();

    if (!ctx->m_DriverInfo.NeedFP64(ctx->platform.getPlatformInfo().eProductFamily) && ctx->platform.hasNoFP64Inst()
        && IGC_IS_FLAG_DISABLED(ForceDPEmulation))
    {
        // check that input does not use double
        // For testing purpose, this check is skipped if ForceDPEmulation is on.
        if (I.getType()->isDoubleTy())
        {
            ctx->EmitError("double type is not supported on this platform", &I);
            m_hasError = true;
            return;
        }
        for (int i = 0, numOpnd = (int)I.getNumOperands(); i < numOpnd; ++i)
        {
            if (I.getOperand(i)->getType()->isDoubleTy())
            {
                ctx->EmitError("double type is not supported on this platform", &I);
                m_hasError = true;
                return;
            }
        }
    }
}

void ErrorCheck::visitCallInst(CallInst& CI)
{
    if (auto* GII = dyn_cast<GenIntrinsicInst>(&CI))
    {
        switch (GII->getIntrinsicID()) {
        case GenISAIntrinsic::GenISA_dp4a_ss:
        case GenISAIntrinsic::GenISA_dp4a_su:
        case GenISAIntrinsic::GenISA_dp4a_us:
        case GenISAIntrinsic::GenISA_dp4a_uu:
        {
            CodeGenContext* Ctx =
                getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
            if (!Ctx->platform.hasHWDp4AddSupport())
            {
                std::string Msg = "Unsupported call to ";
                Msg += CI.getCalledFunction() ?
                    CI.getCalledFunction()->getName() : "indirect function";
                Ctx->EmitError(Msg.c_str(), &CI);
                m_hasError = true;
            }
            break;
        }
        default:
            // Intrinsic supported.
            break;
        }
    }
}

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

#include "Compiler/Optimizer/OpenCLPasses/ErrorCheckPass.h"
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

    return m_hasError;
}

void ErrorCheck::visitInstruction(llvm::Instruction& I)
{
    auto ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();

    if (!ctx->m_DriverInfo.NeedFP64(ctx->platform.getPlatformInfo().eProductFamily) && !ctx->platform.supportFP64()
        && IGC_IS_FLAG_DISABLED(ForceDPEmulation))
    {
        // check that input does not use double
        // For testing purpose, this check is skipped if ForceDPEmulation is on.
        if (I.getType()->isDoubleTy())
        {
            ctx->EmitError("double type is not supported on this platform");
            m_hasError = true;
            return;
        }
        for (int i = 0, numOpnd = (int)I.getNumOperands(); i < numOpnd; ++i)
        {
            if (I.getOperand(i)->getType()->isDoubleTy())
            {
                ctx->EmitError("double type is not supported on this platform");
                m_hasError = true;
                return;
            }
        }
    }
}
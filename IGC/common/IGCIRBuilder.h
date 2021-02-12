/*========================== begin_copyright_notice ============================

Copyright (c) 2016-2021 Intel Corporation

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

#pragma once

#include "common/LLVMWarningsPush.hpp"
#include "llvmWrapper/IR/IRBuilder.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/ConstantFolder.h"
#include "llvm/IR/Function.h"
#include <llvm/Analysis/TargetFolder.h>
#include "llvm/IR/InstrTypes.h"
#include "common/LLVMWarningsPop.hpp"

//This Builder class provides definitions for functions calls that were once available till LLVM version 3.6.0
//===--------------------------------------------------------------------===
// CreateCall Variations which are removed in 3.8 for API Simplification, which IGC still finds convenient to use

//===--------------------------------------------------------------------===
namespace llvm {

    template<typename T = ConstantFolder, typename InserterTyDef() = IRBuilderDefaultInserter >
    class IGCIRBuilder : public IGCLLVM::IRBuilder<T, InserterTyDef()>
    {
    public:
        IGCIRBuilder(LLVMContext &C, const T &F, InserterTyDef() I = InserterTyDef()(),
            MDNode *FPMathTag = nullptr,
            ArrayRef<OperandBundleDef> OpBundles = None)
            : IGCLLVM::IRBuilder<T, InserterTyDef()>(C, F, I, FPMathTag, OpBundles){}

        explicit IGCIRBuilder(LLVMContext &C, MDNode *FPMathTag = nullptr,
            ArrayRef<OperandBundleDef> OpBundles = None)
            : IGCLLVM::IRBuilder<T, InserterTyDef()>(C, FPMathTag, OpBundles){}

        explicit IGCIRBuilder(BasicBlock *TheBB, MDNode *FPMathTag = nullptr)
            : IGCLLVM::IRBuilder<T, InserterTyDef()>(TheBB, FPMathTag){}

        explicit IGCIRBuilder(Instruction *IP, MDNode *FPMathTag = nullptr,
            ArrayRef<OperandBundleDef> OpBundles = None)
            : IGCLLVM::IRBuilder<T, InserterTyDef()>(IP, FPMathTag, OpBundles) {}

        CallInst *CreateCall2(Value *Callee, Value *Arg1, Value *Arg2,
            const Twine &Name = "") {
            Value *Args[] = { Arg1, Arg2 };
            return this->CreateCall(Callee, Args);
        }

        CallInst *CreateCall3(Value *Callee, Value *Arg1, Value *Arg2, Value *Arg3,
            const Twine &Name = "") {
            Value *Args[] = { Arg1, Arg2, Arg3 };
            return this->CreateCall(Callee, Args);
        }

        CallInst *CreateCall4(Value *Callee, Value *Arg1, Value *Arg2, Value *Arg3,
            Value *Arg4, const Twine &Name = "") {
            Value *Args[] = { Arg1, Arg2, Arg3, Arg4 };
            return this->CreateCall(Callee, Args);
        }

        CallInst *CreateCall5(Value *Callee, Value *Arg1, Value *Arg2, Value *Arg3,
            Value *Arg4, Value *Arg5, const Twine &Name = "") {
            Value *Args[] = { Arg1, Arg2, Arg3, Arg4, Arg5 };
            return this->CreateCall(Callee, Args);
        }

        inline Value* CreateAnyValuesNotZero(Value** values, unsigned nvalues)
        {
            Value* f0 = ConstantFP::get(values[0]->getType(), 0.0);
            Value* ne0 = this->CreateFCmpUNE(values[0], f0);
            for (unsigned i = 1; i < nvalues; i++)
            {
                ne0 = this->CreateOr(ne0,
                    this->CreateFCmpUNE(values[i], f0));
            }
            return ne0;
        }

        inline Value* CreateAllValuesAreZeroF(Value** values, unsigned nvalues)
        {
            if (nvalues)
            {
                return CreateAllValuesAreConstantFP(values, nvalues,
                    ConstantFP::get(values[0]->getType(), 0.0));
            }
            return nullptr;
        }

        inline Value* CreateAllValuesAreOneF(Value** values, unsigned nvalues)
        {
            if (nvalues)
            {
                return CreateAllValuesAreConstantFP(values, nvalues,
                    ConstantFP::get(values[0]->getType(), 1.0));
            }
            return nullptr;
        }

        inline Value* CreateExtractElementOrPropagate(Value* vec, Value* idx, const Twine& name = "")
        {
            if(vec == nullptr || idx == nullptr) return nullptr;
            Value* srcVec = vec;

            // Traverse ir that created source vector, looking for
            // insertelement with the index we are interested in
            while(auto* IE = dyn_cast<InsertElementInst>(srcVec))
            {
                Value* srcVal = IE->getOperand(1);
                Value* srcIdx = IE->getOperand(2);

                auto* cSrcIdx = dyn_cast<ConstantInt>(srcIdx);
                auto* cIdx    = dyn_cast<ConstantInt>(idx);
                if(srcIdx == idx ||
                   (cSrcIdx && cIdx &&
                    cSrcIdx->getZExtValue() == cIdx->getZExtValue()))
                {
                    return srcVal;
                }
                else
                {
                    // If index isn't constant we cannot iterate further, since
                    // we don't know which element was replaced in just visited
                    // insertelement.
                    if(!isa<ConstantInt>(idx))
                    {
                        break;
                    }
                    // This is not the instruction we are looking for.
                    // Get it's source and iterate further.
                    srcVec = IE->getOperand(0);
                }
            }

            // We cannot find value to propagate propagate, add extractelement
            return this->CreateExtractElement(vec, idx, name);
        }

        inline Value* CreateExtractElementOrPropagate(Value* vec, uint64_t idx, const Twine& name = "")
        {
            return CreateExtractElementOrPropagate(vec, this->getInt64(idx), name);
        }

    private:

        inline Value* CreateAllValuesAreConstantFP(Value** values,
            unsigned nvalues, Value* constVal)
        {
            if (nvalues)
            {
                Value* aeq = this->CreateFCmpOEQ(values[0], constVal);
                for (unsigned i = 1; i < nvalues; i++)
                {
                    aeq = this->CreateAnd(aeq,
                        this->CreateFCmpOEQ(values[i], constVal));
                }
                return aeq;
            }
            return nullptr;
        }
    };

} // end namespace llvm


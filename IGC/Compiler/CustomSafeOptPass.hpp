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
#pragma once
#include "Compiler/CodeGenContextWrapper.hpp"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/Pass.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/LoopPass.h>
#include "common/LLVMWarningsPop.hpp"

namespace llvm
{
    // Forward declare:
    class SampleIntrinsic;
}

namespace IGC
{
    class CustomSafeOptPass : public llvm::FunctionPass, public llvm::InstVisitor<CustomSafeOptPass>
    {
    public:
        static char ID;

        CustomSafeOptPass();

        ~CustomSafeOptPass() {}

        virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const override
        {
            AU.addRequired<CodeGenContextWrapper>();
            AU.setPreservesCFG();
        }

        virtual bool runOnFunction(llvm::Function &F) override;

        virtual llvm::StringRef getPassName() const override
        {
            return "Custom Pass Optimization";
        }

        void visitInstruction(llvm::Instruction &I);
        void visitAllocaInst(llvm::AllocaInst &I);
        void visitCallInst(llvm::CallInst &C);
        void visitBinaryOperator(llvm::BinaryOperator &I);
        bool isEmulatedAdd(llvm::BinaryOperator &I);
        void visitBfi(llvm::CallInst* inst);
        void visitf32tof16(llvm::CallInst* inst);
        void visitSampleBptr(llvm::SampleIntrinsic* inst);
        void visitMulH(llvm::CallInst* inst, bool isSigned);
        void visitFPToUIInst(llvm::FPToUIInst& FPUII);
        void visitFPTruncInst(llvm::FPTruncInst &I);
        void visitExtractElementInst(llvm::ExtractElementInst& I);
        void visitLdptr(llvm::CallInst* inst);
        //
        // IEEE Floating point arithmetic is not associative.  Any pattern
        // match that changes the order or paramters is unsafe.
        //

        //
        // Removing sources is also unsafe.
        //  X * 1 => X     : Unsafe
        //  X + 0 => X     : Unsafe
        //  X - X => X     : Unsafe
        //

        // When in doubt assume a floating point optimization is unsafe!

        void visitBinaryOperatorTwoConstants(llvm::BinaryOperator &I);
        void visitBinaryOperatorPropNegate(llvm::BinaryOperator &I);
        void visitBitCast(llvm::BitCastInst &BC);

    private:
        bool psHasSideEffect;
    };

    class GenSpecificPattern : public llvm::FunctionPass, public llvm::InstVisitor<GenSpecificPattern>
    {
    public:
        static char ID;

        GenSpecificPattern();

        ~GenSpecificPattern() {}

        virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const override
        {
            AU.setPreservesCFG();
            AU.addRequired<CodeGenContextWrapper>();
        }

        virtual bool runOnFunction(llvm::Function &F) override;

        virtual llvm::StringRef getPassName() const override
        {
            return "GenSpecificPattern";
        }

        void visitBinaryOperator(llvm::BinaryOperator &I);
        void visitSelectInst(llvm::SelectInst &I);
        void visitCmpInst(llvm::CmpInst &I);
        void visitZExtInst(llvm::ZExtInst &I);
        void visitCastInst(llvm::CastInst &I);
        void visitIntToPtr(llvm::IntToPtrInst& I);
        void visitSDiv(llvm::BinaryOperator& I);
        void visitTruncInst(llvm::TruncInst &I);
        void visitBitCastInst(llvm::BitCastInst &I);

        template <typename MaskType> void matchReverse(llvm::BinaryOperator &I);
    };

    class IGCConstProp : public llvm::FunctionPass
    {
    public:
        static char ID;

        IGCConstProp(bool enableMathConstProp=false,
                     bool enableSimplifyGEP = false);

        ~IGCConstProp() {}

        virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const override
        {
            AU.addRequired<llvm::TargetLibraryInfoWrapperPass>();
            AU.addRequired<CodeGenContextWrapper>();
            AU.setPreservesCFG();
        }

        virtual bool runOnFunction(llvm::Function &F) override;

        virtual llvm::StringRef getPassName() const override
        {
            return "specialized const-prop with shader-const replacement";
        }
    private:
        llvm::Module *module;
        llvm::Constant* ReplaceFromDynConstants(unsigned bufId, unsigned eltId, unsigned int size_in_bytes, llvm::Type* type);
        llvm::Constant* replaceShaderConstant(llvm::LoadInst *inst);
        llvm::Constant* ConstantFoldCallInstruction(llvm::CallInst *inst);
        llvm::Constant* ConstantFoldCmpInst(llvm::CmpInst *inst);
        llvm::Constant* ConstantFoldExtractElement(llvm::ExtractElementInst *inst);
        bool simplifyAdd(llvm::BinaryOperator *BO);
        bool simplifyGEP(llvm::GetElementPtrInst *GEP);
        // try to evaluate the address if it is constant.
        bool EvalConstantAddress(llvm::Value* address, unsigned int &offset, llvm::Value* ptrSrc = nullptr);
        bool m_enableMathConstProp;
        bool m_enableSimplifyGEP;
        const llvm::DataLayout *m_TD;
        llvm::TargetLibraryInfo *m_TLI;
    };
    
    llvm::FunctionPass *createGenStrengthReductionPass();
    llvm::FunctionPass *createNanHandlingPass();
    llvm::FunctionPass *createFlattenSmallSwitchPass();
    llvm::FunctionPass *createIGCIndirectICBPropagaionPass();
    llvm::FunctionPass *createBlendToDiscardPass();
    llvm::FunctionPass *createMarkReadOnlyLoadPass();
    llvm::FunctionPass *createLogicalAndToBranchPass();

} // namespace IGC

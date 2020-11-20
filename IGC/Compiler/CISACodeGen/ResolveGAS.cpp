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

#define DEBUG_TYPE "gas-resolver"
#include "Compiler/CISACodeGen/ResolveGAS.h"
#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"
#include "Compiler/CodeGenContextWrapper.hpp"
#include "Compiler/MetaDataUtilsWrapper.h"
#include "Compiler/IGCPassSupport.h"
#include "common/LLVMWarningsPush.hpp"
#include "WrapperLLVM/Utils.h"
#include "llvm/Support/Debug.h"
#include "llvmWrapper/IR/Constant.h"
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/Pass.h>
#include "common/LLVMWarningsPop.hpp"
#include "GenISAIntrinsics/GenIntrinsics.h"
#include "Probe/Assertion.h"

using namespace llvm;
using namespace IGC;
using namespace IGC::IGCMD;

namespace {

    typedef IRBuilder<llvm::NoFolder> BuilderType;

    class GASPropagator;

    // Generaic address space (GAS) pointer resolving is done in two steps:
    // 1) Find cast from non-GAS pointer to GAS pointer
    // 2) Propagate that non-GAS pointer to all users of that GAS pointer at best
    //    effort.
    class GASResolving : public FunctionPass {
        const unsigned GAS = ADDRESS_SPACE_GENERIC;

        BuilderType* IRB;
        GASPropagator* Propagator;

        // Phi node being able to be resolved from its initial value.
        DenseSet<PHINode*> ResolvableLoopPHIs;

    public:
        static char ID;

        GASResolving() : FunctionPass(ID), IRB(nullptr), Propagator(nullptr) {
            initializeGASResolvingPass(*PassRegistry::getPassRegistry());
        }

        bool runOnFunction(Function&) override;

        void getAnalysisUsage(AnalysisUsage& AU) const override {
            AU.setPreservesCFG();
            AU.addRequired<LoopInfoWrapperPass>();
            AU.addRequired<AAResultsWrapperPass>();
            AU.addRequired<MetaDataUtilsWrapper>();
        }

        bool isResolvableLoopPHI(PHINode* PN) const {
            return ResolvableLoopPHIs.count(PN) != 0;
        }

    private:
        bool resolveOnFunction(Function*) const;
        bool resolveOnBasicBlock(BasicBlock*) const;

        bool resolveMemoryFromHost(Function&) const;

        void populateResolvableLoopPHIs();
        void populateResolvableLoopPHIsForLoop(const Loop*);

        bool isAddrSpaceResolvable(PHINode* PN, const Loop* L,
            BasicBlock* BackEdge) const;

        bool checkGenericArguments(Function& F) const;
        void convertLoadToGlobal(LoadInst* LI) const;
        bool isLoadGlobalCandidate(LoadInst* LI) const;
    };

    class GASPropagator : public InstVisitor<GASPropagator, bool> {
        friend class InstVisitor<GASPropagator, bool>;

        GASResolving* Resolver;
        BuilderType* IRB;

        Use* TheUse;
        Value* TheVal;

    public:
        GASPropagator(GASResolving* R, BuilderType* Builder)
            : Resolver(R), IRB(Builder) {}

        bool propagate(Use* U, Value* V) {
            TheUse = U;
            TheVal = V;
            Instruction* I = cast<Instruction>(U->getUser());
            return visit(*I);
        }

    private:
        bool visitInstruction(Instruction& I);

        bool visitLoadInst(LoadInst&);
        bool visitStoreInst(StoreInst&);
        bool visitAddrSpaceCastInst(AddrSpaceCastInst&);
        bool visitBitCastInst(BitCastInst&);
        bool visitPtrToIntInst(PtrToIntInst&);
        bool visitGetElementPtrInst(GetElementPtrInst&);
        bool visitPHINode(PHINode&);
        bool visitICmp(ICmpInst&);
        bool visitSelect(SelectInst&);
        bool visitMemCpyInst(MemCpyInst&);
        bool visitMemMoveInst(MemMoveInst&);
        bool visitMemSetInst(MemSetInst&);
        bool visitCallInst(CallInst&);
    };

} // End anonymous namespace

FunctionPass* IGC::createResolveGASPass() { return new GASResolving(); }

char GASResolving::ID = 0;

#define PASS_FLAG "igc-gas-resolve"
#define PASS_DESC "Resolve generic address space"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
namespace IGC {
    IGC_INITIALIZE_PASS_BEGIN(GASResolving, PASS_FLAG, PASS_DESC, PASS_CFG_ONLY,
        PASS_ANALYSIS)
        IGC_INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
        IGC_INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
        IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
        IGC_INITIALIZE_PASS_END(GASResolving, PASS_FLAG, PASS_DESC, PASS_CFG_ONLY,
            PASS_ANALYSIS)
}

bool GASResolving::runOnFunction(Function& F) {
    BuilderType TheBuilder(F.getContext());
    GASPropagator ThePropagator(this, &TheBuilder);
    IRB = &TheBuilder;
    Propagator = &ThePropagator;

    resolveMemoryFromHost(F);

    populateResolvableLoopPHIs();

    bool Changed = false;
    bool LocalChanged = false;
    do {
        LocalChanged = resolveOnFunction(&F);
        Changed |= LocalChanged;
    } while (LocalChanged);

    return Changed;
}

bool GASResolving::resolveOnFunction(Function* F) const {
    bool Changed = false;

    ReversePostOrderTraversal<Function*> RPOT(F);
    for (auto& BB : RPOT)
        Changed |= resolveOnBasicBlock(BB);

    return Changed;
}

bool GASResolving::resolveOnBasicBlock(BasicBlock* BB) const {
    bool Changed = false;

    for (auto BI = BB->begin(), BE = BB->end(); BI != BE; /* EMPTY */) {
        Instruction* I = &(*BI++);
        AddrSpaceCastInst* CI = dyn_cast<AddrSpaceCastInst>(I);
        // Skip non `addrspacecast` instructions.
        if (!CI)
            continue;
        PointerType* DstPtrTy = cast<PointerType>(CI->getType());
        // Skip non generic address casting.
        if (DstPtrTy->getAddressSpace() != GAS)
            continue;
        Type* DstTy = DstPtrTy->getElementType();
        Value* Src = CI->getOperand(0);
        PointerType* SrcPtrTy = cast<PointerType>(Src->getType());
        // Canonicalize the addrspace cast by separating out the type casting if
        // any.
        if (SrcPtrTy->getElementType() != DstTy) {
            BuilderType::InsertPointGuard Guard(*IRB);
            // Transform the following cast
            //
            //  addrspacecast SrcTy addrspace(S)* to DstTy addrspace(T)*
            //
            // to
            //  bitcast SrcTy addrspace(S)* to DstTy addrspace(S)*
            //  addrspacecast DstTy addrspace(S)* to DstTy addrspace(T)*
            //
            PointerType* TransPtrTy =
                PointerType::get(DstTy, SrcPtrTy->getAddressSpace());

            IRB->SetInsertPoint(CI);
            // Update source.
            Src = IRB->CreateBitCast(Src, TransPtrTy);
            SrcPtrTy = cast<PointerType>(Src->getType());
            CI->setOperand(0, Src);
            Changed = true;
        }

        llvm::Module* ModuleMeta = BB->getModule();

        if (CI->isUsedByMetadata()) {
            auto* L = LocalAsMetadata::getIfExists(CI);
            if (L) {
                auto* MDV = MetadataAsValue::getIfExists(ModuleMeta->getContext(), L);
                if (MDV) {
                    SmallPtrSet<User*, 4> Users(MDV->users().begin(), MDV->users().end());
                    for (auto U : Users) {
                        if (isa<DbgDeclareInst>(cast<Value>(U)) || isa<DbgValueInst>(cast<Value>(U))) {
                            CallInst* CallI = dyn_cast_or_null<CallInst>(cast<Value>(U));
                            if (CallI) {
                                MetadataAsValue* MAV = MetadataAsValue::get(CI->getContext(), ValueAsMetadata::get(Src));
                                CallI->setArgOperand(0, MAV);
                                Changed = true;
                            }
                            else {
                                IGC_ASSERT_MESSAGE(false, "Unexpected instruction");
                            }
                        }
                        else {
                            IGC_ASSERT_MESSAGE(false, "Unexpected user");
                        }
                    }
                }
            }
        }

        // Since %49 is used twice in a phi instruction like the one below:
        // %56 = phi %"class.someclass" addrspace(4)* [ %49, %53 ], [ %49, %742 ]
        // the use iterator was handling such phi instructions twice.
        // This was causing a crash since propagate function might erase instructions.
        DenseSet<Instruction*> instructionSet;
        DenseSet<Use*> useSet;
        for (auto UI = CI->use_begin(), UE = CI->use_end(); UI != UE; ++UI) {
            Use* U = &(*UI);
            Instruction* I = cast<Instruction>(U->getUser());
            if (instructionSet.find(I) == instructionSet.end())
            {
                instructionSet.insert(I);
                useSet.insert(U);
            }
        }
        // Propagate that source through all users of this cast.
        for (auto it = useSet.begin(); it != useSet.end(); ++it) {
            Use* U = *it;
            Changed |= Propagator->propagate(U, Src);
        }
        // Re-update next instruction once there's change.
        if (Changed)
            BI = std::next(BasicBlock::iterator(CI));
        // Remove this `addrspacecast` once it's no longer used.
        if (CI->use_empty()) {
            CI->eraseFromParent();
            Changed = true;
        }
    }

    return Changed;
}

void GASResolving::populateResolvableLoopPHIs() {
    LoopInfo& LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    for (LoopInfo::reverse_iterator I = LI.rbegin(), E = LI.rend(); I != E; ++I) {
        Loop* L = *I;
        populateResolvableLoopPHIsForLoop(L);
    }
}

void GASResolving::populateResolvableLoopPHIsForLoop(const Loop* L) {
    BasicBlock* H = L->getHeader();

    pred_iterator PI = pred_begin(H), E = pred_end(H);
    if (PI == E)
        return;

    BasicBlock* Incoming = *PI++;
    if (PI == E)
        return;

    BasicBlock* BackEdge = *PI++;
    if (PI != E)
        return;

    if (L->contains(Incoming)) {
        if (L->contains(BackEdge))
            return;
        std::swap(Incoming, BackEdge);
    }
    else if (!L->contains(BackEdge))
        return;

    for (auto I = H->begin(); isa<PHINode>(I); ++I) {
        PHINode* PN = cast<PHINode>(I);
        if (!isAddrSpaceResolvable(PN, L, BackEdge))
            continue;
        ResolvableLoopPHIs.insert(PN);
    }
}

bool GASResolving::isAddrSpaceResolvable(PHINode* PN, const Loop* L,
    BasicBlock* BackEdge) const {
    PointerType* PtrTy = dyn_cast<PointerType>(PN->getType());
    if (!PtrTy || PtrTy->getAddressSpace() != GAS)
        return false;

    Instruction* Next =
        dyn_cast<Instruction>(PN->getIncomingValueForBlock(BackEdge));
    if (!Next)
        return false;

    // Walk through use-def chain to figure out whether `Next` is resolvable from
    // `PN`.
    while (Next != PN) {
        // GEP
        if (GetElementPtrInst * GEP = dyn_cast<GetElementPtrInst>(Next)) {
            Next = dyn_cast<Instruction>(GEP->getPointerOperand());
            if (!Next)
                return false;
            continue;
        }
        // TODO: Add other operators.
        return false;
    }

    return true;
}

bool GASPropagator::visitInstruction(Instruction& I) {
    // DO NOTHING.
    LLVM_DEBUG(dbgs() << "PROPAGATE:" << *TheVal << '\n');
    LLVM_DEBUG(dbgs() << "  THROUGH:" << I << '\n');
    return false;
}

bool GASPropagator::visitLoadInst(LoadInst&) {
    TheUse->set(TheVal);
    return true;
}

bool GASPropagator::visitStoreInst(StoreInst& ST) {
    // Only propagate on the `pointer` operand. If that generic pointer is used
    // as value operand and stored in memory, we have to use its value in generic
    // address space.
    if (TheUse->getOperandNo() != ST.getPointerOperandIndex())
        return false;
    TheUse->set(TheVal);
    return true;
}

bool GASPropagator::visitAddrSpaceCastInst(AddrSpaceCastInst& I) {
    PointerType* SrcPtrTy = cast<PointerType>(TheVal->getType());
    PointerType* DstPtrTy = cast<PointerType>(I.getType());
    // Skip if a cast between two different address spaces will be generated.
    if (SrcPtrTy->getAddressSpace() != DstPtrTy->getAddressSpace())
        return false;

    Value* Src = TheVal;
    if (SrcPtrTy->getElementType() != DstPtrTy->getElementType()) {
        BuilderType::InsertPointGuard Guard(*IRB);
        IRB->SetInsertPoint(&I);
        Src = IRB->CreateBitCast(Src, DstPtrTy);
    }
    I.replaceAllUsesWith(Src);
    I.eraseFromParent();

    return true;
}

bool GASPropagator::visitBitCastInst(BitCastInst& I) {
    PointerType* SrcPtrTy = cast<PointerType>(TheVal->getType());
    PointerType* DstPtrTy = cast<PointerType>(I.getType());

    BuilderType::InsertPointGuard Guard(*IRB);
    IRB->SetInsertPoint(&(*std::next(BasicBlock::iterator(&I))));
    // Push `addrspacecast` forward by replacing this `bitcast` on GAS with the
    // one on non-GAS followed by a new `addrspacecast` to GAS.
    Type* DstTy = DstPtrTy->getElementType();
    PointerType* TransPtrTy =
        PointerType::get(DstTy, SrcPtrTy->getAddressSpace());
    Value* Src = TheVal;
    if (SrcPtrTy->getElementType() != DstTy)
        Src = IRB->CreateBitCast(Src, TransPtrTy);
    Value* NewPtr = IRB->CreateAddrSpaceCast(Src, DstPtrTy);
    I.replaceAllUsesWith(NewPtr);
    I.eraseFromParent();

    return true;
}

bool GASPropagator::visitPtrToIntInst(PtrToIntInst& I) {
    // Don't propagate through `ptrtoint` as that conversion is different from
    // various address spaces.
    return false;
}

bool GASPropagator::visitGetElementPtrInst(GetElementPtrInst& I) {
    PointerType* SrcPtrTy = cast<PointerType>(TheVal->getType());
    PointerType* DstPtrTy = cast<PointerType>(I.getType());

    BuilderType::InsertPointGuard Guard(*IRB);
    IRB->SetInsertPoint(&(*std::next(BasicBlock::iterator(&I))));
    // Push `getelementptr` forward by replacing this `bitcast` on GAS with the
    // one on non-GAS followed by a new `addrspacecast` to GAS.
    Type* DstTy = DstPtrTy->getElementType();
    PointerType* TransPtrTy =
        PointerType::get(DstTy, SrcPtrTy->getAddressSpace());
    TheUse->set(TheVal);
    I.mutateType(TransPtrTy);
    Value* NewPtr = IRB->CreateAddrSpaceCast(&I, DstPtrTy);
    for (auto UI = I.use_begin(), UE = I.use_end(); UI != UE; /*EMPTY*/) {
        Use& U = *UI++;
        if (U.getUser() == NewPtr)
            continue;
        U.set(NewPtr);
    }
    return true;
}

bool GASPropagator::visitPHINode(PHINode& PN) {
    Type* NonGASTy = TheVal->getType();
    Type* GASTy = PN.getType();

    unsigned e = PN.getNumIncomingValues();
    SmallVector<Value*, 4> NewIncomingValues(e);

    if (Resolver->isResolvableLoopPHI(&PN)) {
        // For resolvable loop phi, resolve it based on the
        for (unsigned i = 0; i != e; ++i) {
            Value* V = PN.getIncomingValue(i);
            // For incoming value, use the value being propagated.
            if (V == TheUse->get()) {
                NewIncomingValues[i] = TheVal;
                continue;
            }
            Instruction* I = cast<Instruction>(V);
            // For value generated inside loop, cast them to non-GAS pointers.
            BuilderType::InsertPointGuard Guard(*IRB);
            IRB->SetInsertPoint(&(*std::next(BasicBlock::iterator(I))));

            NewIncomingValues[i] = IRB->CreateAddrSpaceCast(I, NonGASTy);
        }
    }
    else {
        // Otherwise check whether all incoming values are casted from the same
        // address space.
        for (unsigned i = 0; i != e; ++i) {
            Value* V = PN.getIncomingValue(i);
            if (V == TheUse->get()) {
                NewIncomingValues[i] = TheVal;
                continue;
            }

            AddrSpaceCastInst* ASCI = dyn_cast<AddrSpaceCastInst>(V);
            if (!ASCI || ASCI->getSrcTy() != NonGASTy)
                return false;

            NewIncomingValues[i] = ASCI->getOperand(0);
        }
    }

    // Propagate this phi node.
    PHINode* NewPN = PHINode::Create(NonGASTy, e, "", &PN);
    for (unsigned i = 0; i != e; ++i)
        NewPN->addIncoming(NewIncomingValues[i], PN.getIncomingBlock(i));
    NewPN->takeName(&PN);

    BuilderType::InsertPointGuard Guard(*IRB);
    IRB->SetInsertPoint(PN.getParent()->getFirstNonPHI());
    Value* NewPtr = IRB->CreateAddrSpaceCast(NewPN, GASTy);
    PN.replaceAllUsesWith(NewPtr);
    PN.eraseFromParent();
    return true;
}

bool GASPropagator::visitICmp(ICmpInst& I) {
    Type* NonGASTy = TheVal->getType();

    unsigned OpNo = TheUse->getOperandNo();
    Use* TheOtherUse = &I.getOperandUse(1 - OpNo);

    AddrSpaceCastInst* ASCI = dyn_cast<AddrSpaceCastInst>(TheOtherUse->get());
    if (!ASCI || ASCI->getSrcTy() != NonGASTy)
        return false;

    TheUse->set(TheVal);
    TheOtherUse->set(ASCI->getOperand(0));

    return true;
}

bool GASPropagator::visitSelect(SelectInst& I) {
    Type* NonGASTy = TheVal->getType();

    unsigned OpNo = TheUse->getOperandNo();
    Use* TheOtherUse = &I.getOperandUse(3 - OpNo);

    AddrSpaceCastInst* ASCI = dyn_cast<AddrSpaceCastInst>(TheOtherUse->get());
    if (!ASCI || ASCI->getSrcTy() != NonGASTy)
        return false;

    // Change select operands to non-GAS
    TheUse->set(TheVal);
    TheOtherUse->set(ASCI->getOperand(0));

    // Handle select return type
    BuilderType::InsertPointGuard Guard(*IRB);
    IRB->SetInsertPoint(&(*std::next(BasicBlock::iterator(&I))));

    PointerType* DstPtrTy = cast<PointerType>(I.getType());
    PointerType* NonGASPtrTy = dyn_cast<PointerType>(NonGASTy);

    // Push 'addrspacecast' forward by changing the select return type to non-GAS pointer
    // followed by a new 'addrspacecast' to GAS
    PointerType* TransPtrTy = PointerType::get(DstPtrTy->getElementType(), NonGASPtrTy->getAddressSpace());
    I.mutateType(TransPtrTy);
    Value* NewPtr = IRB->CreateAddrSpaceCast(&I, DstPtrTy);

    for (auto UI = I.use_begin(), UE = I.use_end(); UI != UE;) {
        Use& U = *UI++;
        if (U.getUser() == NewPtr)
            continue;
        U.set(NewPtr);
    }
    return true;
}

static bool handelMemTransferInst(MemTransferInst& I) {
    Value* NewDst = nullptr;
    Type* NewDstTy = nullptr;
    Use* DstUse = &I.getArgOperandUse(0);
    if (auto ASCI = dyn_cast<AddrSpaceCastInst>(DstUse->get())) {
        NewDst = ASCI->getOperand(0);
        NewDstTy = NewDst->getType();
    }

    Value* NewSrc = nullptr;
    Type* NewSrcTy = nullptr;
    Use* SrcUse = &I.getArgOperandUse(1);
    if (auto ASCI = dyn_cast<AddrSpaceCastInst>(SrcUse->get())) {
        NewSrc = ASCI->getOperand(0);
        NewSrcTy = NewSrc->getType();
    }

    // No address space cast on src or dst.
    if (NewDst == nullptr && NewSrc == nullptr)
        return false;

    Type* Tys[] = { NewDstTy ? NewDstTy : I.getArgOperand(0)->getType(),
                   NewSrcTy ? NewSrcTy : I.getArgOperand(1)->getType(),
                   I.getArgOperand(2)->getType() };
    Function* Fn = nullptr;
    IGC_ASSERT(nullptr != I.getParent());
    IGC_ASSERT(nullptr != I.getParent()->getParent());
    Module* M = I.getParent()->getParent()->getParent();
    if (isa<MemCpyInst>(I))
        Fn = Intrinsic::getDeclaration(M, Intrinsic::memcpy, Tys);
    else if (isa<MemMoveInst>(I))
        Fn = Intrinsic::getDeclaration(M, Intrinsic::memmove, Tys);
    else
        IGC_ASSERT_EXIT_MESSAGE(0, "unsupported memory intrinsic");

    I.setCalledFunction(Fn);
    if(nullptr != NewDst)
    {
        IGC_ASSERT(nullptr != DstUse);
        DstUse->set(NewDst);
    }
    if(nullptr != NewSrc)
    {
        IGC_ASSERT(nullptr != SrcUse);
        SrcUse->set(NewSrc);
    }
    return true;
}

bool GASPropagator::visitMemCpyInst(MemCpyInst& I) {
    return handelMemTransferInst(I);
}

bool GASPropagator::visitMemMoveInst(MemMoveInst& I) {
    return handelMemTransferInst(I);
}

bool GASPropagator::visitMemSetInst(MemSetInst& I) {
    Use* DstUse = &I.getArgOperandUse(0);
    auto ASCI = dyn_cast<AddrSpaceCastInst>(DstUse->get());
    if (!ASCI)
        return false;

    Value* OrigDst = ASCI->getOperand(0);
    Type* OrigDstTy = OrigDst->getType();

    Type* Tys[] = { OrigDstTy, I.getArgOperand(2)->getType() };
    Function* Fn = Intrinsic::getDeclaration(
        I.getParent()->getParent()->getParent(), Intrinsic::memset, Tys);

    I.setCalledFunction(Fn);
    DstUse->set(OrigDst);
    return true;
}

bool GASPropagator::visitCallInst(CallInst& I) {
    Function* Callee = I.getCalledFunction();

    if (!Callee)
        return false;

    PointerType* SrcPtrTy = cast<PointerType>(TheVal->getType());
    bool IsGAS2P =
        Callee->getName().equals("__builtin_IB_memcpy_generic_to_private");
    bool IsP2GAS =
        Callee->getName().equals("__builtin_IB_memcpy_private_to_generic");
    if (IsGAS2P || IsP2GAS) {
        PointerType* SrcPtrTy = cast<PointerType>(TheVal->getType());

        Type* Tys[4];
        Tys[0] = IsGAS2P ? I.getArgOperand(0)->getType() : SrcPtrTy;
        Tys[1] = IsGAS2P ? SrcPtrTy : I.getArgOperand(1)->getType();
        Tys[2] = I.getArgOperand(2)->getType();
        Tys[3] = I.getArgOperand(3)->getType();
        FunctionType* FTy = FunctionType::get(I.getType(), Tys, false);
        Module* M = I.getParent()->getParent()->getParent();
        IGCLLVM::Constant NewF = nullptr;
        switch (SrcPtrTy->getAddressSpace()) {
        case ADDRESS_SPACE_PRIVATE:
            NewF =
                M->getOrInsertFunction("__builtin_IB_memcpy_private_to_private", FTy);
            break;
        case ADDRESS_SPACE_GLOBAL:
            NewF = M->getOrInsertFunction(
                IsGAS2P ? "__builtin_IB_memcpy_global_to_private"
                : "__builtin_IB_memcpy_private_to_global",
                FTy);
            break;
        case ADDRESS_SPACE_CONSTANT:
            NewF = M->getOrInsertFunction(
                IsGAS2P ? "__builtin_IB_memcpy_constant_to_private"
                : "__builtin_IB_memcpy_private_to_constant",
                FTy);
            break;
        case ADDRESS_SPACE_LOCAL:
            NewF = M->getOrInsertFunction(
                IsGAS2P ? "__builtin_IB_memcpy_local_to_private"
                : "__builtin_IB_memcpy_private_to_local",
                FTy);
            break;
        }

        if (NewF) {
            I.setCalledFunction(NewF);
            TheUse->set(TheVal);
            return true;
        }
    }

    if (Callee->getName().equals("__builtin_IB_to_local")) {
        Type* DstTy = I.getType();
        Value* NewPtr = Constant::getNullValue(DstTy);
        if (SrcPtrTy->getAddressSpace() == ADDRESS_SPACE_LOCAL) {
            BuilderType::InsertPointGuard Guard(*IRB);
            IRB->SetInsertPoint(&I);
            NewPtr = IRB->CreateBitCast(TheVal, DstTy);
        }
        I.replaceAllUsesWith(NewPtr);
        I.eraseFromParent();

        return true;
    }

    if (Callee->getName().equals("__builtin_IB_to_private")) {
        Type* DstTy = I.getType();
        Value* NewPtr = Constant::getNullValue(DstTy);
        if (SrcPtrTy->getAddressSpace() == ADDRESS_SPACE_PRIVATE) {
            BuilderType::InsertPointGuard Guard(*IRB);
            IRB->SetInsertPoint(&I);
            NewPtr = IRB->CreateBitCast(TheVal, DstTy);
        }
        I.replaceAllUsesWith(NewPtr);
        I.eraseFromParent();

        return true;
    }

    return false;
}

bool GASResolving::resolveMemoryFromHost(Function& F) const {
    MetaDataUtils* pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();

    // skip all non-entry functions
    if (!isEntryFunc(pMdUtils, &F))
        return false;

    // early check in order not to iterate whole function
    if (!checkGenericArguments(F))
        return false;

    SmallVector<StoreInst*, 32> Stores;
    SmallVector<LoadInst*, 32> Loads;
    AliasAnalysis* AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();

    // collect load candidates and in parallel check for unsafe instructions
    // visitor may be a more beautiful way to do this
    bool HasASCast = false; // if there exists addrspace cast from non global/generic space
    bool HasPtoi = false; // if there exists ptrtoint with global/generic space
    for (BasicBlock& B : F) {
        for (Instruction& I : B) {
            if (auto LI = dyn_cast<LoadInst>(&I)) {
                if (isLoadGlobalCandidate(LI)) {
                    Loads.push_back(LI);
                }
            }
            else if (auto CI = dyn_cast<CallInst>(&I)) {
                if (CI->onlyReadsMemory())
                    continue;

                // currently recognize only these ones
                // in fact intrinsics should be marked as read-only
                if (auto II = dyn_cast<IntrinsicInst>(CI)) {
                    if (II->getIntrinsicID() == Intrinsic::lifetime_start ||
                        II->getIntrinsicID() == Intrinsic::lifetime_end)
                        continue;
                }

                // if we have an unsafe call in the kernel, abort
                // to improve we can collect arguments of writing calls as memlocations for alias analysis
                return false;
            }
            else if (auto PI = dyn_cast<PtrToIntInst>(&I)) {
                // if we have a ptrtoint we need to check data flow which we don't want to
                if (PI->getPointerAddressSpace() != ADDRESS_SPACE_GLOBAL &&
                    PI->getPointerAddressSpace() != ADDRESS_SPACE_GENERIC)
                    return false;
                else {
                    HasPtoi = true;
                }

                return false;
            }
            else if (auto AI = dyn_cast<AddrSpaceCastInst>(&I)) {
                if (AI->getSrcAddressSpace() != ADDRESS_SPACE_GLOBAL &&
                    AI->getSrcAddressSpace() != ADDRESS_SPACE_GENERIC) {
                    HasASCast = true;
                }
            }
            else if (auto SI = dyn_cast<StoreInst>(&I)) {
                Value* V = SI->getValueOperand();
                if (isa<PointerType>(V->getType())) {
                    // this store can potentially write non-global pointer to memory
                    Stores.push_back(SI);
                }
            }
            else if (I.mayWriteToMemory()) {
                // unsupported instruction poisoning memory
                return false;
            }
        }
    }
    if (HasASCast && HasPtoi)
        return false;

    if (Loads.empty())
        return false;

    bool Changed = false;
    while (!Loads.empty())
    {
        LoadInst* LI = Loads.pop_back_val();

        // check that we don't have aliasing stores for this load
        // we expect to have basic and addrspace AA available at the moment
        // on optimization phase
        bool aliases = false;
        for (auto SI : Stores) {
            if (AA->alias(MemoryLocation::get(SI), MemoryLocation::get(LI))) {
                aliases = true;
                break;
            }
        }
        if (aliases)
            continue;

        convertLoadToGlobal(LI);
        Changed = true;
    }
    return Changed;
}

bool GASResolving::isLoadGlobalCandidate(LoadInst* LI) const {
    // first check that loaded address has generic address space
    // otherwise it is not our candidate
    PointerType* PtrTy = dyn_cast<PointerType>(LI->getType());
    if (!PtrTy || PtrTy->getAddressSpace() != ADDRESS_SPACE_GENERIC)
        return false;

    // next check that it is a load from function argument + offset
    // which is necessary to prove that this address has global addrspace
    Value* LoadBase = LI->getPointerOperand()->stripInBoundsOffsets();
    if (!isa<Argument>(LoadBase))
        return false;

    // don't want to process cases when argument is from local address space
    auto LoadTy = cast<PointerType>(LoadBase->getType());
    if (LoadTy->getAddressSpace() != ADDRESS_SPACE_GLOBAL)
        return false;

    // TODO: skip cases that have been fixed on previous traversals

    return true;
}

void GASResolving::convertLoadToGlobal(LoadInst* LI) const {
    // create two addressspace casts: generic -> global -> generic
    // the next scalar phase of this pass will propagate global to all uses of the load

    PointerType* PtrTy = cast<PointerType>(LI->getType());
    IRB->SetInsertPoint(LI->getNextNode());
    PointerType* GlobalPtrTy = PointerType::get(PtrTy->getElementType(), ADDRESS_SPACE_GLOBAL);
    Value* GlobalAddr = IRB->CreateAddrSpaceCast(LI, GlobalPtrTy);
    Value* GenericCopyAddr = IRB->CreateAddrSpaceCast(GlobalAddr, PtrTy);

    for (auto UI = LI->use_begin(), UE = LI->use_end(); UI != UE; /*EMPTY*/) {
        Use& U = *UI++;
        if (U.getUser() == GlobalAddr)
            continue;
        U.set(GenericCopyAddr);
    }
}

bool GASResolving::checkGenericArguments(Function& F) const {
    // check that we have a pointer to pointer or pointer to struct that has pointer elements
    // and main pointer type is global while underlying pointer type is generic

    auto* FT = F.getFunctionType();
    for (unsigned p = 0; p < FT->getNumParams(); ++p) {
        if (auto Ty = dyn_cast<PointerType>(FT->getParamType(p))) {
            if (Ty->getAddressSpace() != ADDRESS_SPACE_GLOBAL)
                continue;
            auto PteeTy = Ty->getElementType();
            if (auto PTy = dyn_cast<PointerType>(PteeTy)) {
                if (PTy->getAddressSpace() == ADDRESS_SPACE_GENERIC)
                    return true;
            }
            if (auto STy = dyn_cast<StructType>(PteeTy)) {
                for (unsigned e = 0; e < STy->getNumElements(); ++e) {
                    if (auto ETy = dyn_cast<PointerType>(STy->getElementType(e))) {
                        if (ETy->getAddressSpace() == ADDRESS_SPACE_GENERIC)
                            return true;
                    }
                }
            }
        }
    }
    return false;
}
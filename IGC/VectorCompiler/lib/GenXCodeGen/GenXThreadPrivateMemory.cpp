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

//
/// This pass lowers alloca instructions to genx.alloca intrinsics and changes
/// pointer from alloca to offset in predefined stack surface
//
//===----------------------------------------------------------------------===//

#include "GenX.h"
#include "GenXModule.h"
#include "GenXRegion.h"
#include "GenXSubtarget.h"
#include "GenXTargetMachine.h"
#include "GenXUtil.h"
#include "GenXVisa.h"
#include "vc/GenXCodeGen/GenXInternalMetadata.h"

#include "Probe/Assertion.h"
#include "llvmWrapper/IR/DerivedTypes.h"
#include "llvmWrapper/IR/InstrTypes.h"
#include "llvmWrapper/IR/Instructions.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/GenXIntrinsics/GenXMetadata.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/Local.h"

#include <forward_list>
#include <queue>
#include <utility>

using namespace llvm;
using namespace genx;

#define DEBUG_TYPE "genx-tpm"

static cl::opt<bool> ForceSVMTPM("force-svm-tpm", cl::init(true), cl::Hidden,
  cl::desc("Force putting thread-private memory to SVM"));

namespace {

// This actually should've been a FunctionGroupPass,
// but due to the FGPassManager hack we can't run GenXModule twice
// so for now we can't insert module pass that invalidate FGA betw FGPasses
class GenXThreadPrivateMemory : public ModulePass,
                                public InstVisitor<GenXThreadPrivateMemory> {
public:
  GenXThreadPrivateMemory();

  virtual StringRef getPassName() const override {
    return "GenXThreadPrivateMemory";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    ModulePass::getAnalysisUsage(AU);
    AU.addRequired<TargetPassConfig>();
    AU.setPreservesCFG();
  }

  bool runOnModule(Module &M) override;
  bool runOnFunction(Function &F);

  void visitAllocaInst(AllocaInst &I);

private:
  bool replacePhi(PHINode *Phi);
  bool preparePhiForReplacement(PHINode *Phi);
  bool replaceScatterPrivate(CallInst *CI);
  bool replaceGatherPrivate(CallInst *CI);
  bool replacePTI(PtrToIntInst *PTI);
  bool replaceStore(StoreInst *StI);
  bool replaceLoad(LoadInst *LdI);
  bool replaceSelect(SelectInst *Sel);
  bool replaceAddrSpaceCast(AddrSpaceCastInst * AddrCast);
  bool replaceInsertElement(InsertElementInst *Insert);
  bool replaceShuffleVector(ShuffleVectorInst *ShuffleVec);
  Value *lookForPtrReplacement(Value *Ptr) const;
  void addUsers(Value *V);
  void collectEachPossibleTPMUsers();
  void addUsersIfNeeded(Value *V);
  Value *NormalizeFuncPtrVec(Value *V, Instruction *InsPoint);
  std::pair<Value *, unsigned> NormalizeVector(Value *From, Type *To,
                                               Instruction *InsertBefore);
  Instruction *RestoreVectorAfterNormalization(Instruction *From, Type *To);

public:
  static char ID;

private:
  LLVMContext *m_ctx;
  const GenXSubtarget *m_ST;
  const DataLayout *m_DL;
  std::vector<AllocaInst *> m_alloca;
  std::vector<Argument *> m_args;
  std::vector<CallInst *> m_gather;
  std::vector<CallInst *> m_scatter;
  std::map<AllocaInst *, CallInst *> m_allocaToIntrinsic;
  std::queue<Instruction *> m_AIUsers;
  std::set<Instruction *> m_AlreadyAdded;
  PreDefined_Surface m_stack;
  bool m_useGlobalMem = ForceSVMTPM;
};
} // namespace

// Register pass to igc-opt
namespace llvm {
void initializeGenXThreadPrivateMemoryPass(PassRegistry &);
}

INITIALIZE_PASS_BEGIN(GenXThreadPrivateMemory, "GenXThreadPrivateMemory",
                      "GenXThreadPrivateMemory", false, false)
INITIALIZE_PASS_END(GenXThreadPrivateMemory, "GenXThreadPrivateMemory",
                    "GenXThreadPrivateMemory", false, false)

char GenXThreadPrivateMemory::ID = 0;

ModulePass *llvm::createGenXThreadPrivateMemoryPass() {
  return new GenXThreadPrivateMemory;
}

GenXThreadPrivateMemory::GenXThreadPrivateMemory() : ModulePass(ID) {
  initializeGenXThreadPrivateMemoryPass(*PassRegistry::getPassRegistry());
}

static Value *ZExtOrTruncIfNeeded(Value *From, Type *To,
                                  Instruction *InsertBefore) {
  Type *FromTy = From->getType();
  if (FromTy == To)
    return From;

  unsigned FromTySz = FromTy->getPrimitiveSizeInBits();
  unsigned ToTySz = To->getPrimitiveSizeInBits();
  Value *Res = From;
  if (FromTy->isVectorTy() && cast<VectorType>(FromTy)->getNumElements() == 1) {
    auto *TmpRes = CastInst::CreateBitOrPointerCast(
        Res, cast<VectorType>(FromTy)->getElementType(), "", InsertBefore);
    Res = TmpRes;
  }
  if (FromTySz < ToTySz)
    Res = CastInst::CreateZExtOrBitCast(Res, To, "", InsertBefore);
  else if (FromTySz > ToTySz)
    Res = CastInst::CreateTruncOrBitCast(Res, To, "", InsertBefore);
  return Res;
}

// Wipe all internal ConstantExprs out of V if it's a ConstantVector of function pointers
Value *GenXThreadPrivateMemory::NormalizeFuncPtrVec(Value *V, Instruction *InsPoint) {
  V = breakConstantVector(cast<ConstantVector>(V), InsPoint, InsPoint);
  auto *Inst = dyn_cast<InsertElementInst>(V);
  if (!Inst)
    return V;
  std::vector<ExtractElementInst *> Worklist;
  for (; Inst; Inst = dyn_cast<InsertElementInst>(Inst->getOperand(0))) {
    if (auto *EEInst = dyn_cast<ExtractElementInst>(Inst->getOperand(1)))
      if (auto *Idx = dyn_cast<Constant>(EEInst->getIndexOperand());
          Idx && Idx->isZeroValue())
        Worklist.push_back(EEInst);
  }

  std::vector<Constant *> NewVector;
  std::transform(
      Worklist.rbegin(), Worklist.rend(), std::back_inserter(NewVector),
      [this](ExtractElementInst *I) {
        IGC_ASSERT(I->getType()->getScalarType()->isIntegerTy(genx::ByteBits));
        auto *F = cast_or_null<Function>(
            getFunctionPointerFunc(I->getVectorOperand()));
        IGC_ASSERT(F);
        return ConstantExpr::getPtrToInt(F, IntegerType::getInt64Ty(*m_ctx));
      });
  auto *NewCV = ConstantVector::get(NewVector);
  IGC_ASSERT(m_DL->getTypeSizeInBits(V->getType()) ==
             m_DL->getTypeSizeInBits(NewCV->getType()));
  return NewCV;
}

// If data is a vector of double/int64, bitcast each element to 2 int32.
// If data is a vector of function pointers, strip all internal bitcasts
// and possible extractelems (64->8xi8 cast case) to get a vector of int64s.
// If data is a vector of type < 32bit, extend each element in order to create
// proper send instruction in the finalizer.
std::pair<Value *, unsigned>
GenXThreadPrivateMemory::NormalizeVector(Value *From, Type *To,
                                         Instruction *Inst) {
  Type *I32Ty = Type::getInt32Ty(Inst->getContext());
  Type *I64Ty = Type::getInt64Ty(Inst->getContext());
  Value *Res = From;
  Type *FromTy = From->getType();
  IGC_ASSERT(isa<VectorType>(FromTy));
  unsigned NumElts = cast<VectorType>(FromTy)->getNumElements();
  static_assert(genx::ByteBits);
  unsigned EltSz =
      m_DL->getTypeSizeInBits(FromTy->getScalarType()) / genx::ByteBits;
  IGC_ASSERT(EltSz > 0);
  if (isFuncPointerVec(From) &&
      m_DL->getTypeSizeInBits(From->getType()->getScalarType()) <
          genx::QWordBits) {
    From = NormalizeFuncPtrVec(From, Inst);
    IGC_ASSERT(From);
    To = From->getType();
    IGC_ASSERT(To);
    NumElts = cast<VectorType>(To)->getNumElements();
  }
  if (To->getScalarType()->isPointerTy() &&
      To->getScalarType()->getPointerElementType()->isFunctionTy()) {
    Type *I64Ty = Type::getInt64Ty(Inst->getContext());
    To = IGCLLVM::FixedVectorType::get(I64Ty, NumElts);
    Res = CastInst::Create(Instruction::PtrToInt, From, To, "", Inst);
    NumElts *= 2;
    To = IGCLLVM::FixedVectorType::get(I32Ty, NumElts);
    EltSz = I32Ty->getPrimitiveSizeInBits() / genx::ByteBits;
    Res = CastInst::Create(Instruction::BitCast, Res, To, "", Inst);
  } else if (m_DL->getTypeSizeInBits(cast<VectorType>(To)->getElementType()) <
             genx::DWordBits) {
    To = IGCLLVM::FixedVectorType::get(I32Ty, NumElts);
    Res = CastInst::CreateZExtOrBitCast(From, To, "", Inst);
  } else if (m_DL->getTypeSizeInBits(cast<VectorType>(To)->getElementType()) ==
             genx::QWordBits) {
    if (From->getType()->getScalarType()->isPointerTy()) {
      auto *NewType = IGCLLVM::FixedVectorType::get(I64Ty, NumElts);
      From = CastInst::Create(CastInst::PtrToInt, From, NewType, "", Inst);
      EltSz = I64Ty->getPrimitiveSizeInBits() / genx::ByteBits;
    }
    if (!m_useGlobalMem) {
      NumElts *= 2;
      EltSz = I32Ty->getPrimitiveSizeInBits() / genx::ByteBits;
      To = IGCLLVM::FixedVectorType::get(I32Ty, NumElts);
    }
    Res = CastInst::CreateBitOrPointerCast(From, To, "", Inst);
  }

  return std::make_pair(Res, EltSz);
}

Instruction *
GenXThreadPrivateMemory::RestoreVectorAfterNormalization(Instruction *From,
                                                         Type *To) {
  if (From->getType() == To)
    return From;
  Instruction *Restored = From;
  unsigned EltSz = m_DL->getTypeSizeInBits(To->getScalarType());
  IGC_ASSERT(EltSz > 0);
  if (To->getScalarType()->isPointerTy() &&
      To->getScalarType()->getPointerElementType()->isFunctionTy()) {
    auto *NewFrom = From;
    if (From->getType()->isVectorTy() &&
        From->getType()->getScalarType()->isIntegerTy(genx::DWordBits)) {
      auto *NewTy =
          IGCLLVM::FixedVectorType::get(Type::getInt64Ty(*m_ctx),
                          cast<VectorType>(From->getType())->getNumElements() / 2);
      NewFrom = CastInst::CreateBitOrPointerCast(From, NewTy);
      NewFrom->insertAfter(From);
      From = NewFrom;
    }
    Restored = CastInst::Create(Instruction::IntToPtr, NewFrom, To);
  } else if (EltSz < genx::DWordBits) {
    Restored = CastInst::Create(Instruction::Trunc, From, To, "");
  } else if (EltSz == genx::QWordBits &&
             !(m_useGlobalMem && To->getScalarType()->isIntegerTy(64))) {
    if (!From->getType()->getScalarType()->isPointerTy() &&
        To->getScalarType()->isPointerTy()) {
      if (!m_useGlobalMem) {
        IGC_ASSERT(
            From->getType()->getScalarType()->isIntegerTy(genx::DWordBits));
        Type *NewTy = IGCLLVM::FixedVectorType::get(
            Type::getInt64Ty(*m_ctx),
            cast<VectorType>(From->getType())->getNumElements() / 2);
        auto *NewFrom = CastInst::CreateBitOrPointerCast(From, NewTy);
        NewFrom->insertAfter(From);
        From = NewFrom;
      }
      Restored = CastInst::Create(CastInst::IntToPtr, From, To);
    } else
      Restored = CastInst::CreateBitOrPointerCast(From, To);
  }
  if (Restored != From)
    Restored->insertAfter(From);
  return Restored;
}

static Value *DoubleVector(Value *OrigVector, unsigned ShiftVal,
                           Instruction *InsertPoint) {
  IRBuilder<> Builder(InsertPoint);
  Type *I32Ty = Type::getInt32Ty(InsertPoint->getContext());
  unsigned NumElts =
      cast<VectorType>(OrigVector->getType())->getNumElements() * 2;
  Type *OrigVectorEltTy =
      cast<VectorType>(OrigVector->getType())->getElementType();
  Value *NewElts =
      UndefValue::get(IGCLLVM::FixedVectorType::get(OrigVectorEltTy, NumElts));
  for (unsigned CurEltNum = 0; CurEltNum * 2 < NumElts; ++CurEltNum) {
    Value *OldIdx = ConstantInt::get(I32Ty, CurEltNum);
    Value *NewIdx = ConstantInt::get(I32Ty, CurEltNum * 2);
    Value *EltOld = Builder.CreateExtractElement(OrigVector, OldIdx);
    NewElts = Builder.CreateInsertElement(NewElts, EltOld, NewIdx);
    NewIdx = ConstantInt::get(I32Ty, CurEltNum * 2 + 1);
    if (ShiftVal) {
      Value *TyShift = ConstantInt::get(I32Ty, ShiftVal);
      EltOld = Builder.CreateAdd(EltOld, TyShift);
    }
    NewElts = Builder.CreateInsertElement(NewElts, EltOld, NewIdx);
  }

  return NewElts;
}

static Value *FormEltsOffsetVector(unsigned NumElts, unsigned TySz,
                                   Instruction *InsertBefore) {
  IRBuilder<> Builder(InsertBefore);
  Type *I32Ty = Type::getInt32Ty(InsertBefore->getContext());
  Value *EltsOffset =
      UndefValue::get(IGCLLVM::FixedVectorType::get(I32Ty, NumElts));
  for (unsigned CurElt = 0; CurElt < NumElts; ++CurElt) {
    Value *Idx = ConstantInt::get(I32Ty, CurElt);
    Value *EltOffset = ConstantInt::get(I32Ty, CurElt * TySz);
    EltsOffset = Builder.CreateInsertElement(EltsOffset, EltOffset, Idx);
  }

  return EltsOffset;
}

static Value *FormEltsOffsetVectorForSVM(Value *BaseOffset,
                                         Value *Offsets,
                                         Instruction *InsertBefore) {
  IGC_ASSERT(BaseOffset->getType()->isIntegerTy(64));
  IGC_ASSERT(Offsets->getType()->isVectorTy());

  IRBuilder<> Builder(InsertBefore);
  Type *I64Ty = Type::getInt64Ty(InsertBefore->getContext());
  unsigned NumElts = cast<VectorType>(Offsets->getType())->getNumElements();
  Value *BaseOffsets = Builder.CreateVectorSplat(NumElts, BaseOffset);
  if (!Offsets->getType()->getScalarType()->isIntegerTy(64))
    Offsets = Builder.CreateZExtOrBitCast(Offsets,
        IGCLLVM::FixedVectorType::get(I64Ty, NumElts));
  return Builder.CreateAdd(BaseOffsets, Offsets);
}

Value *GenXThreadPrivateMemory::lookForPtrReplacement(Value *Ptr) const {
  Type *PtrTy = Ptr->getType();
  IGC_ASSERT(PtrTy->isPtrOrPtrVectorTy());

  Type *MemTy = IntegerType::get(*m_ctx, (m_useGlobalMem ? 64 : 32));
  if (isa<UndefValue>(Ptr)) {
    if (auto PtrVecTy = dyn_cast<VectorType>(PtrTy))
      return UndefValue::get(
          IGCLLVM::FixedVectorType::get(MemTy, PtrVecTy->getNumElements()));
    return UndefValue::get(MemTy);
  } else if (auto BC = dyn_cast<BitCastInst>(Ptr))
    return lookForPtrReplacement(BC->getOperand(0));
  else if (auto ITP = dyn_cast<IntToPtrInst>(Ptr))
    return ITP->getOperand(0);
  else if (auto AI = dyn_cast<AllocaInst>(Ptr)) {
    auto AllocaIntr = m_allocaToIntrinsic.find(AI);
    IGC_ASSERT_MESSAGE(AllocaIntr != m_allocaToIntrinsic.end(),
      "Each alloca must be here");
    return AllocaIntr->second;
  } else if (isa<Argument>(Ptr)) {
    if (PtrTy->isPointerTy()) {
      auto *PTI = CastInst::Create(CastInst::PtrToInt, Ptr, MemTy);
      PTI->insertBefore(&cast<Argument>(Ptr)->getParent()->front().front());
      return PTI;
    } else
      return Ptr;
  } else if (isa<ExtractElementInst>(Ptr) &&
             lookForPtrReplacement(
                 cast<ExtractElementInst>(Ptr)->getVectorOperand())) {
    if (PtrTy->isPointerTy()) {
      auto *PTI = CastInst::Create(Instruction::PtrToInt, Ptr, MemTy);
      PTI->insertAfter(cast<Instruction>(Ptr));
      return PTI;
    } else
      return Ptr;
  } else if (auto *CI = dyn_cast<CallInst>(Ptr)) {
    if (!IGCLLVM::isIndirectCall(*CI) &&
        (GenXIntrinsic::getAnyIntrinsicID(CI->getCalledFunction()) ==
             GenXIntrinsic::genx_svm_block_ld ||
         GenXIntrinsic::getAnyIntrinsicID(CI->getCalledFunction()) ==
             GenXIntrinsic::genx_svm_gather)) {
      return Ptr;
    }
  } else if (auto *LI = dyn_cast<LoadInst>(Ptr)) {
    // meeting load means we're processing load's user earlier
    // than the load itself, which is possible because we could
    // reach load's user earlier in the du chains thru some other value
    // generate cast for now
    auto *Cast = CastInst::Create(Instruction::PtrToInt, LI, MemTy, "");
    Cast->insertAfter(LI);
    return Cast;
  } else if (auto *ASCast = dyn_cast<AddrSpaceCastInst>(Ptr)) {
    return lookForPtrReplacement(ASCast->getPointerOperand());
  } else if (isa<ConstantPointerNull>(Ptr))
    return ConstantInt::get(MemTy, 0);

  report_fatal_error("Cannot find pointer replacement");
}

static std::pair<Value *, Value *>
castValuesToCommonType(Value *V1, Value *V2, Instruction *InsertBefore) {
  auto *V1T = V1->getType();
  auto *V2T = V2->getType();
  if (V1T == V2T)
    return {V1, V2};

  auto *V1I = dyn_cast<IntegerType>(V1T);
  auto *V2I = dyn_cast<IntegerType>(V2T);
  if (V1I && V2I) {
    IGC_ASSERT(V1I->getBitWidth() != V2I->getBitWidth());
    // Integer here is some pointer representation, thus using zero extension
    if (V1I->getBitWidth() < V2I->getBitWidth())
      V1 = new ZExtInst(V1, V2I, V1->getName() + ".common.ty", InsertBefore);
    else
      V2 = new ZExtInst(V2, V1I, V2->getName() + ".common.ty", InsertBefore);
    return {V1, V2};
  }

  IGC_ASSERT_MESSAGE(0, "Cannot find common type for values");
  return {V1, V2};
}

bool GenXThreadPrivateMemory::replaceAddrSpaceCast(
  AddrSpaceCastInst* AddrCast) {
  auto NewAlloca = lookForPtrReplacement(AddrCast->getPointerOperand());

  auto IntToPtr = IntToPtrInst::Create(
    llvm::Instruction::CastOps::IntToPtr, NewAlloca,
    AddrCast->getPointerOperand()->getType(), "", AddrCast);
  auto NewAddrCast =
    AddrSpaceCastInst::Create(llvm::Instruction::CastOps::AddrSpaceCast,
    IntToPtr, AddrCast->getType(), "", AddrCast);

  AddrCast->replaceAllUsesWith(NewAddrCast);
  AddrCast->eraseFromParent();

  return true;
}

bool GenXThreadPrivateMemory::replaceInsertElement(InsertElementInst *Insert) {
  LLVM_DEBUG(dbgs() << "Replacing insert element inst " << *Insert
                    << " ===>\n");
  auto InsertTy = cast<VectorType>(Insert->getType());
  if (!InsertTy->isPtrOrPtrVectorTy())
    return false;

  Value *Vec = Insert->getOperand(0);
  Value *Elt = Insert->getOperand(1);
  Value *Idx = Insert->getOperand(2);

  Value *NewVec = lookForPtrReplacement(Vec);
  auto NewElt = lookForPtrReplacement(Elt);
  auto NewInsert = InsertElementInst::Create(NewVec, NewElt, Idx,
                                             Insert->getName() + ".tpm");
  NewInsert->insertAfter(Insert);

  auto CastToOldTy =
      CastInst::Create(Instruction::IntToPtr, NewInsert, InsertTy,
                       NewInsert->getName() + ".temp.itp");
  CastToOldTy->insertAfter(NewInsert);
  Insert->replaceAllUsesWith(CastToOldTy);
  Insert->eraseFromParent();

  LLVM_DEBUG(dbgs() << *CastToOldTy << "\n");
  return true;
}

bool GenXThreadPrivateMemory::replaceShuffleVector(
    ShuffleVectorInst *ShuffleVec) {
  LLVM_DEBUG(dbgs() << "Replacing insert element inst " << *ShuffleVec
                    << " ===>\n");
  auto ShuffleTy = cast<VectorType>(ShuffleVec->getType());
  if (!ShuffleTy->isPtrOrPtrVectorTy())
    return false;

  Value *Vec1 = ShuffleVec->getOperand(0);
  Value *Vec2 = ShuffleVec->getOperand(1);

  Value *NewVec1 = lookForPtrReplacement(Vec1);
  Value *NewVec2 = lookForPtrReplacement(Vec2);
  auto NewShuffleVec = new ShuffleVectorInst(
      NewVec1, NewVec2, IGCLLVM::getShuffleMaskForBitcode(ShuffleVec), ShuffleVec->getName() + ".tpm");
  NewShuffleVec->insertAfter(ShuffleVec);

  auto CastToOldTy =
      CastInst::Create(Instruction::IntToPtr, NewShuffleVec, ShuffleTy,
                       NewShuffleVec->getName() + ".temp.itp");
  CastToOldTy->insertAfter(NewShuffleVec);
  ShuffleVec->replaceAllUsesWith(CastToOldTy);
  ShuffleVec->eraseFromParent();

  LLVM_DEBUG(dbgs() << *CastToOldTy << "\n");
  return true;
}

bool GenXThreadPrivateMemory::replaceLoad(LoadInst *LdI) {
  LLVM_DEBUG(dbgs() << "Replacing load " << *LdI << " ===>\n");
  IRBuilder<> Builder(LdI);
  Type *LdTy = LdI->getType();
  Type *LdEltTy = LdTy;
  if (isa<VectorType>(LdEltTy))
    LdEltTy = cast<VectorType>(LdEltTy)->getElementType();
  else
    LdTy = IGCLLVM::FixedVectorType::get(LdTy, 1);

  unsigned NumEltsToLoad = cast<VectorType>(LdTy)->getNumElements();
  unsigned ValueEltSz = m_DL->getTypeSizeInBits(LdEltTy) / genx::ByteBits;

  Value *PredVal = ConstantInt::get(Type::getInt1Ty(*m_ctx), 1);
  Value *Pred = Builder.CreateVectorSplat(NumEltsToLoad, PredVal);

  Type *I32Ty = Type::getInt32Ty(*m_ctx);
  Type *I64Ty = Type::getInt64Ty(*m_ctx);
  Value *OldValOfTheDataRead =
      Builder.CreateVectorSplat(NumEltsToLoad, UndefValue::get(LdEltTy));
  std::tie(OldValOfTheDataRead, ValueEltSz) =
      NormalizeVector(OldValOfTheDataRead, LdTy, LdI);
  NumEltsToLoad =
      cast<VectorType>(OldValOfTheDataRead->getType())->getNumElements();

  Value *PointerOp = LdI->getPointerOperand();
  Value *Offset = lookForPtrReplacement(PointerOp);
  Offset =
      ZExtOrTruncIfNeeded(Offset, m_useGlobalMem ? I64Ty : I32Ty, LdI);
  auto IID = m_useGlobalMem
                 ? llvm::GenXIntrinsic::genx_svm_gather
                 : llvm::GenXIntrinsic::genx_gather_scaled;

  Value *EltsOffset = FormEltsOffsetVector(NumEltsToLoad, ValueEltSz, LdI);

  unsigned NumBlocks = m_DL->getTypeSizeInBits(LdEltTy) / genx::ByteBits;
  // This logic is aligned with the on in CisaBuilder and GenXLowering
  // The reason behind check for == 2 is that svm intrinsics don't support
  // BlockSize of 2, so for ops with i16s we have to use BlockSize == 1 and NumBlocks == 2
  Value *logNumBlocks = ConstantInt::get(I32Ty, genx::log2(NumBlocks == 2 ? NumBlocks : 1));
  Value *Scale = ConstantInt::get(Type::getInt16Ty(*m_ctx), 0);
  Value *Surface = ConstantInt::get(I32Ty,
                                    visa::getReservedSurfaceIndex(m_stack));
  if (m_useGlobalMem)
    Offset = FormEltsOffsetVectorForSVM(Offset, EltsOffset, LdI);
  Function *F = GenXIntrinsic::getGenXDeclaration(
      LdI->getModule(), IID,
      {OldValOfTheDataRead->getType(),
      Pred->getType(),
       (m_useGlobalMem ? Offset : EltsOffset)->getType()});
  CallInst *Gather =
      m_useGlobalMem
          ? IntrinsicInst::Create(
                F, {Pred, logNumBlocks, Offset, OldValOfTheDataRead},
                LdI->getName())
          : IntrinsicInst::Create(F,
                                  {Pred, logNumBlocks, Scale, Surface, Offset,
                                   EltsOffset, OldValOfTheDataRead},
                                  LdI->getName());
  Gather->insertAfter(LdI);
  m_gather.push_back(Gather);
  Instruction *ProperGather = RestoreVectorAfterNormalization(Gather, LdTy);

  if (!isa<VectorType>(LdI->getType()) &&
      isa<VectorType>(ProperGather->getType())) {
    VectorType *GatheredTy = cast<VectorType>(ProperGather->getType());
    Builder.ClearInsertionPoint();
    Instruction *LdVal = nullptr;
    if (GatheredTy->getNumElements() == 1)
      LdVal = cast<Instruction>(Builder.CreateExtractElement(
          ProperGather, static_cast<uint64_t>(0ul),
          ProperGather->getName() + ".tpm.loadres"));
    else
      LdVal = cast<Instruction>(Builder.CreateBitOrPointerCast(
          ProperGather, LdI->getType(),
          ProperGather->getName() + ".tpm.loadres"));
    LdVal->insertAfter(ProperGather);
    ProperGather = LdVal;
  }

  Gather->setMetadata(InstMD::SVMBlockType,
                      MDNode::get(*m_ctx, llvm::ValueAsMetadata::get(
                                              UndefValue::get(LdEltTy))));

  LLVM_DEBUG(dbgs() << *Gather << "\n");
  LdI->replaceAllUsesWith(ProperGather);
  LdI->eraseFromParent();

  return true;
}

bool GenXThreadPrivateMemory::replaceStore(StoreInst *StI) {
  LLVM_DEBUG(dbgs() << "Replacing store " << *StI << " ===>\n");
  IRBuilder<> Builder(StI);
  Value *ValueOp = StI->getValueOperand();
  Type *ValueOpTy = ValueOp->getType();
  if (ValueOpTy->isIntOrPtrTy() || ValueOpTy->isFloatingPointTy()) {
    ValueOp = Builder.CreateVectorSplat(1, ValueOp);
    ValueOpTy = ValueOp->getType();
  }
  IGC_ASSERT(ValueOpTy->isVectorTy());

  unsigned ValueEltSz = 0;
  std::tie(ValueOp, ValueEltSz) = NormalizeVector(ValueOp, ValueOpTy, StI);
  unsigned ValueNumElts =
      cast<VectorType>(ValueOp->getType())->getNumElements();

  Value *PointerOp = StI->getPointerOperand();
  Value *Offset = lookForPtrReplacement(PointerOp);
  Type *I32Ty = Type::getInt32Ty(*m_ctx);
  Type *I64Ty = Type::getInt64Ty(*m_ctx);
  Offset =
      ZExtOrTruncIfNeeded(Offset, m_useGlobalMem ? I64Ty : I32Ty, StI);

  auto IID = m_useGlobalMem
                 ? llvm::GenXIntrinsic::genx_svm_scatter
                 : llvm::GenXIntrinsic::genx_scatter_scaled;

  Value *PredVal = ConstantInt::get(Type::getInt1Ty(*m_ctx), 1);
  Value *Pred = Builder.CreateVectorSplat(ValueNumElts, PredVal);
  Value *EltsOffset = FormEltsOffsetVector(ValueNumElts, ValueEltSz, StI);

  if (m_useGlobalMem)
    Offset = FormEltsOffsetVectorForSVM(Offset, EltsOffset, StI);

  Function *F = GenXIntrinsic::getGenXDeclaration(
      StI->getModule(), IID,
      {Pred->getType(),
       (m_useGlobalMem ? Offset : EltsOffset)->getType(),
       ValueOp->getType()});
  unsigned NumBlocks = m_DL->getTypeSizeInBits(ValueOpTy->getScalarType()) / genx::ByteBits;
  // see the comment in replaceLoad above
  Value *logNumBlocks = ConstantInt::get(I32Ty, genx::log2(NumBlocks == 2 ? NumBlocks : 1));
  Value *Scale = ConstantInt::get(Type::getInt16Ty(*m_ctx), 0);
  Value *Surface = ConstantInt::get(I32Ty,
                                    visa::getReservedSurfaceIndex(m_stack));
  auto *Scatter =
      m_useGlobalMem
          ? IntrinsicInst::Create(F, {Pred, logNumBlocks, Offset, ValueOp},
                                  StI->getName())
          : IntrinsicInst::Create(F,
                                  {Pred, logNumBlocks, Scale, Surface, Offset,
                                   EltsOffset, ValueOp},
                                  StI->getName());
  Scatter->insertAfter(StI);
  StI->eraseFromParent();

  Scatter->setMetadata(
      InstMD::SVMBlockType,
      MDNode::get(*m_ctx, llvm::ValueAsMetadata::get(
                              UndefValue::get(ValueOpTy->getScalarType()))));

  LLVM_DEBUG(dbgs() << *Scatter << "\n");
  m_scatter.push_back(Scatter);

  return true;
}

bool GenXThreadPrivateMemory::replacePTI(PtrToIntInst *PTI) {
  LLVM_DEBUG(dbgs() << "Replacing PTI " << *PTI << " ===> ");
  Value *PointerOp = PTI->getPointerOperand();
  Value *Offset = lookForPtrReplacement(PointerOp);

  if (isa<Argument>(Offset))
    return false;

  Offset = ZExtOrTruncIfNeeded(Offset, PTI->getDestTy(), PTI);
  LLVM_DEBUG(dbgs() << *Offset << "\n");
  PTI->replaceAllUsesWith(Offset);
  PTI->eraseFromParent();

  return true;
}

static Value *lookForTruncOffset(Value *V) {
  if (auto *I = dyn_cast<TruncInst>(V))
    return I->getOperand(0);
  else {
    // TODO: extend the list of supported instruction types
    if (auto *I = dyn_cast<BinaryOperator>(V)) {
      for (unsigned i = 0; i < I->getNumOperands(); ++i) {
        auto *Op = I->getOperand(i);
        if (Value *Off = lookForTruncOffset(Op); Off != Op) {
          if (I->getType() != Off->getType()) {
            auto *OtherOp = I->getOperand((i + 1) % 2);
            OtherOp = ZExtOrTruncIfNeeded(OtherOp, Off->getType(), I);
            if (i == 0)
              I = BinaryOperator::Create(I->getOpcode(), Off, OtherOp,
                                         I->getName(), I);
            else
              I = BinaryOperator::Create(I->getOpcode(), OtherOp, Off,
                                         I->getName(), I);
          }
          return I;
        }
      }
    }
    return V;
  }
}

bool GenXThreadPrivateMemory::replaceGatherPrivate(CallInst *CI) {
  LLVM_DEBUG(dbgs() << "Replacing gather.priv " << *CI << " ===>\n");
  auto IID = m_useGlobalMem ? llvm::GenXIntrinsic::genx_svm_gather
                            : llvm::GenXIntrinsic::genx_gather_scaled;

  Type *OrigDstTy = CI->getType();
  IGC_ASSERT(isa<VectorType>(OrigDstTy));
  Type *NewDstTy = OrigDstTy;
  Value *OldValue = CI->getArgOperand(3);
  unsigned ValueEltSz =
      m_DL->getTypeSizeInBits(NewDstTy->getScalarType()) / genx::ByteBits;

  // Check gather.private invariant.
  IGC_ASSERT(NewDstTy == OldValue->getType());

  // Cast data type to legal.
  // Consider i64 legal for SVM cases
  if (!(m_useGlobalMem && CI->getType()->getScalarType()->isIntegerTy(64)))
    std::tie(OldValue, ValueEltSz) = NormalizeVector(OldValue, NewDstTy, CI);
  NewDstTy = OldValue->getType();
  unsigned ValueNumElts = cast<VectorType>(NewDstTy)->getNumElements();

  Value *Pred = CI->getArgOperand(0);
  Value *EltsOffset = CI->getArgOperand(2);
  if (!m_useGlobalMem &&
      cast<VectorType>(OrigDstTy)->getElementType()->getPrimitiveSizeInBits() ==
          genx::QWordBits) {
    IGC_ASSERT(ValueNumElts ==
               cast<VectorType>(EltsOffset->getType())->getNumElements() * 2);
    EltsOffset = DoubleVector(EltsOffset, ValueEltSz, CI);
    Pred = DoubleVector(Pred, 0, CI);
  }

  Type *I32Ty = Type::getInt32Ty(*m_ctx);
  Type *I64Ty = Type::getInt64Ty(*m_ctx);
  Value *PointerOp = CI->getOperand(1);
  Value *Offset = lookForPtrReplacement(PointerOp);
  Offset = ZExtOrTruncIfNeeded(Offset, m_useGlobalMem ? I64Ty : I32Ty, CI);

  if (m_useGlobalMem)
    Offset = FormEltsOffsetVectorForSVM(lookForTruncOffset(Offset), EltsOffset, CI);

  Function *F = GenXIntrinsic::getGenXDeclaration(
      CI->getModule(), IID,
      {NewDstTy, Pred->getType(),
       (m_useGlobalMem ? Offset : EltsOffset)->getType()});

  // 32u is max exec_size allowed (see GenXCisaBuilder.cpp:buildIntrinsic
  // GetExecSize lambda) For svm.gather/scatter:
  //    BlockSize is inferred from vec elem type
  //    BlockNum should be TotalMemSize / (ExecSize * BlockSize)
  //      where TotalMemSize is a total amount of mem read/written for
  //      gather/scatter
  // TODO: revise NumBlocks for non-svm case
  unsigned NumBlocks =
      (m_useGlobalMem)
          ? genx::log2(m_DL->getTypeSizeInBits(NewDstTy) /
                       (genx::ByteBits *
                        std::min<unsigned>(
                            32u, cast<VectorType>(NewDstTy)->getNumElements()) *
                        (m_DL->getTypeSizeInBits(NewDstTy->getScalarType()) /
                         genx::ByteBits)))
          : genx::log2(ValueEltSz);
  Value *logNumBlocks = ConstantInt::get(I32Ty, NumBlocks);
  Value *Scale = ConstantInt::get(Type::getInt16Ty(*m_ctx), 0);
  Value *Surface =
      ConstantInt::get(I32Ty, visa::getReservedSurfaceIndex(m_stack));

  CallInst *Gather =
      m_useGlobalMem
          ? IntrinsicInst::Create(F, {Pred, logNumBlocks, Offset, OldValue},
                                  CI->getName())
          : IntrinsicInst::Create(F,
                                  {Pred, logNumBlocks, Scale, Surface, Offset,
                                   EltsOffset, OldValue},
                                  CI->getName());
  Gather->insertAfter(CI);
  m_gather.push_back(Gather);
  LLVM_DEBUG(dbgs() << *Gather << "\n");

  Instruction *ProperGather =
      RestoreVectorAfterNormalization(Gather, OrigDstTy);
  CI->replaceAllUsesWith(ProperGather);
  CI->eraseFromParent();

  return true;
}

bool GenXThreadPrivateMemory::replaceScatterPrivate(CallInst *CI) {
  LLVM_DEBUG(dbgs() << "Replacing scatter.priv " << *CI << " ===>\n");
  auto IID = m_useGlobalMem
                 ? llvm::GenXIntrinsic::genx_svm_scatter
                 : llvm::GenXIntrinsic::genx_scatter_scaled;
  Value *ValueOp = CI->getArgOperand(3);
  Type *OrigValueTy = ValueOp->getType();
  IGC_ASSERT(isa<VectorType>(OrigValueTy));
  unsigned EltSz = 0;
  std::tie(ValueOp, EltSz) = NormalizeVector(ValueOp, ValueOp->getType(), CI);

  Value *Pred = CI->getArgOperand(0);
  Value *EltsOffset = CI->getArgOperand(2);
  if (cast<VectorType>(OrigValueTy)
          ->getElementType()
          ->getPrimitiveSizeInBits() == genx::QWordBits) {
    // TODO: revisit this for splat and/or non-const value cases,
    // e.g. replace EltSz with  (isSplatValue(EltsOffset) ||
    //                          !isa<Constant>(EltsOffset)) ? 0 : EltSZ
    EltsOffset = DoubleVector(EltsOffset, EltSz, CI);
    Pred = DoubleVector(Pred, 0, CI);
  }

  Value *ScatterPtr = CI->getArgOperand(1);
  Type *I32Ty = Type::getInt32Ty(*m_ctx),
       *I64Ty = Type::getInt64Ty(*m_ctx);
  Value *Offset = lookForPtrReplacement(ScatterPtr);
  Offset = ZExtOrTruncIfNeeded(Offset, m_useGlobalMem ? I64Ty : I32Ty, CI);

  if (m_useGlobalMem)
    EltsOffset = FormEltsOffsetVectorForSVM(Offset, EltsOffset, CI);

  Function *F = GenXIntrinsic::getGenXDeclaration(
      CI->getModule(), IID,
      {Pred->getType(), EltsOffset->getType(),
       ValueOp->getType()});

  Value *logNumBlocks = ConstantInt::get(I32Ty, m_useGlobalMem ? 0 : genx::log2(EltSz));
  Value *Scale = ConstantInt::get(Type::getInt16Ty(*m_ctx), 0); // scale is always 0
  Value *Surface = ConstantInt::get(I32Ty,
                                    visa::getReservedSurfaceIndex(m_stack));
  CallInst *ScatterStScaled =
      m_useGlobalMem
          ? IntrinsicInst::Create(
                F,
                {Pred, logNumBlocks, EltsOffset, ValueOp})
          : IntrinsicInst::Create(
                F, {Pred, logNumBlocks,
                    Scale, Surface,
                    Offset, EltsOffset, ValueOp});
  ScatterStScaled->insertAfter(CI);
  m_scatter.push_back(ScatterStScaled);
  LLVM_DEBUG(dbgs() << *ScatterStScaled << "\n");
  CI->replaceAllUsesWith(ScatterStScaled);
  CI->eraseFromParent();

  return true;
}

bool GenXThreadPrivateMemory::replacePhi(PHINode *Phi) {
  SmallVector<Value *, 8> PhiOps;
  for (auto &IncVal : Phi->incoming_values())
    PhiOps.push_back(lookForPtrReplacement(static_cast<Value *>(IncVal.get())));

  IGC_ASSERT(!PhiOps.empty());

  // first we need to synchronize operands of types T and <1 x T> =>
  // make all of them scalar T
  auto NonVecOpIt = std::find_if(PhiOps.begin(), PhiOps.end(), [](Value *V) {
    return !V->getType()->isVectorTy();
  });
  if (NonVecOpIt != PhiOps.end()) {
    auto *NonVecTy = (*NonVecOpIt)->getType();

    auto TypeFixer = [NonVecTy, PhiOps](Value *&V) {
      if (V->getType() == NonVecTy)
        return;
      else if (V->getType()->getScalarType() == NonVecTy->getScalarType() &&
               V->getType()->isVectorTy() != NonVecTy->isVectorTy()) {
        if (V->getType()->isVectorTy()) {
          IGC_ASSERT(cast<VectorType>(V->getType())->getNumElements() == 1);
          auto *VCast = CastInst::Create(CastInst::BitCast, V, NonVecTy->getScalarType());
          VCast->insertAfter(cast<Instruction>(V));
          V = VCast;
        }
      } else {
        IGC_ASSERT_MESSAGE(0, "New phi types mismatch");
      }
    };
    std::for_each(PhiOps.begin(), PhiOps.end(), TypeFixer);
  }

  Type *OffsetTy = PhiOps[0]->getType();
  auto TypeChecker = [OffsetTy](Value *V) { return OffsetTy == V->getType(); };
  IGC_ASSERT(std::all_of(PhiOps.begin(), PhiOps.end(), TypeChecker));

  PHINode *NewPhi = PHINode::Create(OffsetTy, PhiOps.size());
  for (unsigned i = 0; i < PhiOps.size(); ++i)
    NewPhi->addIncoming(PhiOps[i], Phi->getIncomingBlock(i));

  NewPhi->insertAfter(Phi);

  // Create temporary cast instruction to satisfy old phi users. Types must be
  // different due to replacement pointer by integer offset.
  IGC_ASSERT(NewPhi->getType() != Phi->getType());
  CastInst *TempCast = CastInst::CreateBitOrPointerCast(NewPhi, Phi->getType());
  TempCast->insertAfter(NewPhi->getParent()->getFirstNonPHI());

  Phi->replaceAllUsesWith(TempCast);
  Phi->eraseFromParent();

  return true;
}

// |--%1 = PHI(%2, ...)
// |         ^
// |         |
// |         |
// |  %2 = PHI(%1, ...)
// |---------^
//
// In this situation, it's difficult to find the origin of the pointer. PtrToInt
// and IntToPtr break the process of searching (see lookForPtrReplacement) and
// it helps to 'emulate' phi in TPM
bool GenXThreadPrivateMemory::preparePhiForReplacement(PHINode *Phi) {
  if (!isa<PointerType>(Phi->getType()))
    return false;

  Type *I64Ty = Type::getInt64Ty(Phi->getContext());
  StringRef Name = Phi->getName();
  Instruction *TempPtrToInt = CastInst::Create(
      Instruction::PtrToInt, Phi, I64Ty, Name + ".tpm.temp.pti",
      Phi->getParent()->getFirstNonPHI());
  Instruction *TempIntToPtr =
      CastInst::Create(Instruction::IntToPtr, TempPtrToInt, Phi->getType(),
                       Name + ".tpm.temp.itp");
  TempIntToPtr->insertAfter(TempPtrToInt);
  Phi->replaceAllUsesWith(TempIntToPtr);

  // Replacement here was incorrect
  TempPtrToInt->replaceUsesOfWith(TempIntToPtr, Phi);

  return true;
}

bool GenXThreadPrivateMemory::replaceSelect(SelectInst *Sel) {
  Value *Cond = Sel->getCondition();
  Value *TrueValue = lookForPtrReplacement(Sel->getTrueValue());
  Value *FalseValue = lookForPtrReplacement(Sel->getFalseValue());

  std::tie(TrueValue, FalseValue) =
      castValuesToCommonType(TrueValue, FalseValue, Sel);

  SelectInst *NewSel = SelectInst::Create(Cond, TrueValue, FalseValue);
  NewSel->insertAfter(Sel);
  NewSel->setDebugLoc(Sel->getDebugLoc());

  CastInst *TempCast = CastInst::CreateBitOrPointerCast(NewSel, Sel->getType());
  TempCast->insertAfter(NewSel);
  TempCast->setDebugLoc(Sel->getDebugLoc());

  Sel->replaceAllUsesWith(TempCast);
  Sel->eraseFromParent();

  return true;
}

static Value *GetUndefVec(Type *Ty, unsigned NumElts) {
  return UndefValue::get(IGCLLVM::FixedVectorType::get(Ty, NumElts));
}

static std::pair<Value *, Value *> GetUndefPair(Type *Ty, unsigned NumElts) {
  return std::make_pair(GetUndefVec(Ty, NumElts), GetUndefVec(Ty, NumElts));
}

static Value *FillVecWithSeqVals(Value *Vec, unsigned Start,
                                 Instruction *InsertBefore) {
  IRBuilder<> Builder(InsertBefore);
  Builder.SetInsertPoint(InsertBefore);

  Type *I32Ty = Type::getInt32Ty(InsertBefore->getContext());
  unsigned NumElts = cast<VectorType>(Vec->getType())->getNumElements();
  for (unsigned i = 0; i < NumElts; ++i) {
    Value *Idx = ConstantInt::get(I32Ty, i);
    Value *Val = ConstantInt::get(I32Ty, i + Start);
    Vec = Builder.CreateInsertElement(Vec, Val, Idx);
  }
  return Vec;
}

static std::pair<Value *, Value *>
SplitVec(Value *Vec, unsigned NumElts, Instruction *InsertBefore,
         std::pair<Value *, Value *> Splitters) {
  IRBuilder<> Builder(InsertBefore);
  Builder.SetInsertPoint(InsertBefore);

  Type *EltTy = cast<VectorType>(Vec->getType())->getElementType();
  Value *First = Builder.CreateShuffleVector(Vec, GetUndefVec(EltTy, NumElts),
                                             Splitters.first);
  Value *Second = Builder.CreateShuffleVector(Vec, GetUndefVec(EltTy, NumElts),
                                              Splitters.second);
  return std::make_pair(First, Second);
}

static void EraseUsers(Instruction *Inst) {
  std::forward_list<User *> Users(Inst->user_begin(), Inst->user_end());
  for (auto U : Users) {
    IGC_ASSERT_MESSAGE(
        !isa<StoreInst>(U) &&
            !(isa<CallInst>(U) &&
              (GenXIntrinsic::getGenXIntrinsicID(cast<CallInst>(U)) ==
                   GenXIntrinsic::genx_svm_scatter ||
               GenXIntrinsic::getGenXIntrinsicID(cast<CallInst>(U)) ==
                   GenXIntrinsic::genx_scatter_scaled ||
               GenXIntrinsic::getGenXIntrinsicID(cast<CallInst>(U)) ==
                   GenXIntrinsic::genx_svm_block_st)),
        "Should not erase stores");
    Instruction *PotentiallyDeadInst = cast<Instruction>(U);
    EraseUsers(PotentiallyDeadInst);
    IGC_ASSERT_MESSAGE(U->use_empty(),
                       "Cannot recursively remove users of a replaced alloca");
    PotentiallyDeadInst->eraseFromParent();
  }
}

void SplitScatter(CallInst *CI) {
  auto IID = static_cast<llvm::GenXIntrinsic::ID>(
      GenXIntrinsic::getAnyIntrinsicID(CI));
  IGC_ASSERT((IID == llvm::GenXIntrinsic::genx_scatter_scaled) ||
             (IID == llvm::GenXIntrinsic::genx_svm_scatter));
  Type *DataTy = nullptr;
  if (IID == llvm::GenXIntrinsic::genx_scatter_scaled) {
    DataTy = CI->getArgOperand(5)->getType();
  } else if (IID == llvm::GenXIntrinsic::genx_svm_scatter) {
    DataTy = CI->getArgOperand(2)->getType();
  }
  unsigned NumElts = cast<VectorType>(DataTy)->getNumElements();
  IGC_ASSERT(NumElts % 2 == 0);

  Type *I32Ty = Type::getInt32Ty(CI->getContext());
  std::pair<Value *, Value *> Splitters = GetUndefPair(I32Ty, NumElts / 2);
  Splitters.first = FillVecWithSeqVals(Splitters.first, 0, CI);
  Splitters.second = FillVecWithSeqVals(Splitters.second, NumElts / 2, CI);

  Value *Pred = nullptr;
  Value *EltOffsets = nullptr;
  Value *OldVal = nullptr;
  if (IID == llvm::GenXIntrinsic::genx_scatter_scaled) {
    Pred = CI->getArgOperand(0);
    EltOffsets = CI->getArgOperand(5);
    OldVal = CI->getArgOperand(6);
  } else if (IID == llvm::GenXIntrinsic::genx_svm_scatter) {
    Pred = CI->getArgOperand(0);
    EltOffsets = CI->getArgOperand(2);
    OldVal = CI->getArgOperand(3);
  }
  IGC_ASSERT(Pred && EltOffsets && OldVal);

  std::pair<Value *, Value *> NewPreds = SplitVec(Pred, NumElts, CI, Splitters);

  std::pair<Value *, Value *> NewEltOffsets =
      SplitVec(EltOffsets, NumElts, CI, Splitters);

  std::pair<Value *, Value *> OldVals =
      SplitVec(OldVal, NumElts, CI, Splitters);

  Function *F = GenXIntrinsic::getGenXDeclaration(CI->getModule(), IID,
                                          {NewPreds.first->getType(),
                                           NewEltOffsets.first->getType(),
                                           OldVals.first->getType()});

  CallInst *FirstScatter = nullptr;
  CallInst *SecondScatter = nullptr;
  if (IID == llvm::GenXIntrinsic::genx_scatter_scaled) {
    Value *LogNumBlock = CI->getArgOperand(1);
    Value *Scale = CI->getArgOperand(2);
    Value *Surface = CI->getArgOperand(3);
    Value *Offset = CI->getArgOperand(4);

    FirstScatter =
        IntrinsicInst::Create(F, {NewPreds.first, LogNumBlock, Scale, Surface,
                                  Offset, NewEltOffsets.first, OldVals.first});
    SecondScatter = IntrinsicInst::Create(
        F, {NewPreds.second, LogNumBlock, Scale, Surface, Offset,
            NewEltOffsets.second, OldVals.second});
  } else if (IID == llvm::GenXIntrinsic::genx_svm_scatter) {
    Value *LogNumBlock = CI->getArgOperand(1);
    FirstScatter = IntrinsicInst::Create(
        F, {NewPreds.first, LogNumBlock, NewEltOffsets.first, OldVals.first});
    SecondScatter =
        IntrinsicInst::Create(F, {NewPreds.second, LogNumBlock,
                                  NewEltOffsets.second, OldVals.second});
  }
  IGC_ASSERT(FirstScatter && SecondScatter);

  auto *MD = CI->getMetadata(InstMD::SVMBlockType);
  if (MD) {
    FirstScatter->setMetadata(InstMD::SVMBlockType, MD);
    SecondScatter->setMetadata(InstMD::SVMBlockType, MD);
  }

  FirstScatter->insertAfter(CI);
  SecondScatter->insertAfter(FirstScatter);

  CI->eraseFromParent();
}

void SplitGather(CallInst *CI) {
  auto IID = static_cast<llvm::GenXIntrinsic::ID>(
      GenXIntrinsic::getAnyIntrinsicID(CI));
  IGC_ASSERT((IID == llvm::GenXIntrinsic::genx_gather_scaled) ||
             (IID == llvm::GenXIntrinsic::genx_svm_gather));
  Type *DstTy = CI->getType();
  unsigned NumElts = cast<VectorType>(DstTy)->getNumElements();
  IGC_ASSERT(NumElts % 2 == 0);

  Type *I32Ty = Type::getInt32Ty(CI->getContext());
  std::pair<Value *, Value *> Splitters = GetUndefPair(I32Ty, NumElts / 2);
  Splitters.first = FillVecWithSeqVals(Splitters.first, 0, CI);
  Splitters.second = FillVecWithSeqVals(Splitters.second, NumElts / 2, CI);

  Value *Pred = nullptr;
  Value *EltOffsets = nullptr;
  Value *OldVal = nullptr;
  if (IID == llvm::GenXIntrinsic::genx_gather_scaled) {
    Pred = CI->getArgOperand(0);
    EltOffsets = CI->getArgOperand(5);
    OldVal = CI->getArgOperand(6);
  } else if (IID == llvm::GenXIntrinsic::genx_svm_gather) {
    Pred = CI->getArgOperand(0);
    EltOffsets = CI->getArgOperand(2);
    OldVal = CI->getArgOperand(3);
  }
  IGC_ASSERT(Pred && EltOffsets && OldVal);

  std::pair<Value *, Value *> NewPreds = SplitVec(Pred, NumElts, CI, Splitters);

  std::pair<Value *, Value *> NewEltOffsets =
      SplitVec(EltOffsets, NumElts, CI, Splitters);
  std::pair<Value *, Value *> OldVals =
      SplitVec(OldVal, NumElts, CI, Splitters);
  Function *F = GenXIntrinsic::getGenXDeclaration(CI->getModule(), IID,
                                          {OldVals.first->getType(),
                                           NewPreds.first->getType(),
                                           NewEltOffsets.first->getType()});

  CallInst *FirstGather = nullptr;
  CallInst *SecondGather = nullptr;
  if (IID == llvm::GenXIntrinsic::genx_gather_scaled) {
    Value *LogNumBlock = CI->getArgOperand(1);
    Value *Scale = CI->getArgOperand(2);
    Value *Surface = CI->getArgOperand(3);
    Value *Offset = CI->getArgOperand(4);

    FirstGather =
        IntrinsicInst::Create(F, {NewPreds.first, LogNumBlock, Scale, Surface,
                                  Offset, NewEltOffsets.first, OldVals.first});
    SecondGather = IntrinsicInst::Create(
        F, {NewPreds.second, LogNumBlock, Scale, Surface, Offset,
            NewEltOffsets.second, OldVals.second});
  } else if (IID == llvm::GenXIntrinsic::genx_svm_gather) {
    Value *LogNumBlock = CI->getArgOperand(1);
    FirstGather = IntrinsicInst::Create(
        F, {NewPreds.first, LogNumBlock, NewEltOffsets.first, OldVals.first});
    SecondGather =
        IntrinsicInst::Create(F, {NewPreds.second, LogNumBlock,
                                  NewEltOffsets.second, OldVals.second});
  }
  IGC_ASSERT(FirstGather && SecondGather);

  auto *MD = CI->getMetadata(InstMD::SVMBlockType);
  if (MD) {
    FirstGather->setMetadata(InstMD::SVMBlockType, MD);
    SecondGather->setMetadata(InstMD::SVMBlockType, MD);
  }

  FirstGather->insertAfter(CI);
  SecondGather->insertAfter(FirstGather);

  Value *Joiner = FillVecWithSeqVals(GetUndefVec(I32Ty, NumElts), 0, CI);
  IRBuilder<> Builder(CI);
  Builder.SetInsertPoint(SecondGather->getNextNode());
  Value *JointGather =
      Builder.CreateShuffleVector(FirstGather, SecondGather, Joiner);

  CI->replaceAllUsesWith(JointGather);
  CI->eraseFromParent();
}

class SVMChecker {
  static constexpr unsigned LoadsThreshold = 1;

  std::map<Value *, unsigned> Visited;

public:
  // pre-transformation analysis to determine
  // which kind of mem should we place TPM at
  unsigned checkSVMNecessary(Value *V) {
    if (Visited.count(V) > 0)
      return Visited.at(V);
    // do not handle ConstExprs for now
    if (!isa<Instruction>(V) && !isa<Argument>(V))
      return 0;
    unsigned LoadsMet = 0;
    if (isa<LoadInst>(V)) {
      ++LoadsMet;
    } else if (auto *CI = dyn_cast<CallInst>(V)) {
      auto IID = GenXIntrinsic::getAnyIntrinsicID(CI);
      if (IID == GenXIntrinsic::genx_gather_private ||
          IID == GenXIntrinsic::genx_scatter_private ||
          // TODO: make this analysis interprocedural
          IID == GenXIntrinsic::not_any_intrinsic) {
        // do not process users of priv mem intrinsics
        // or calls to other functions
        return 0;
      } else if (IID == GenXIntrinsic::genx_svm_gather ||
                 IID == GenXIntrinsic::genx_svm_scatter) {
        // Switch to SVM immediately once we meet some previously
        // generated genx.svm intrinsics communicating with private memory
        // TODO: handling svm.block_ld/st requires support from replace* and
        // split* methods as well
        return LoadsThreshold + 1;
      }
    } else if (isa<PHINode>(V) || isa<ICmpInst>(V)) {
      // do not go thru phi as loops may appear and
      // it doesn't seem necessary for the analysis now
      return 0;
    }
    unsigned Result = 0;
    for (auto *U : V->users())
      Result = std::max(Result, checkSVMNecessary(U));
    Visited.insert(std::make_pair(V, Result + LoadsMet));
    return Result + LoadsMet;
  }

  bool operator()(Value *V) { return checkSVMNecessary(V) > LoadsThreshold; }
};

void GenXThreadPrivateMemory::addUsers(Value *V) {
  IGC_ASSERT(isa<Instruction>(V) || isa<Argument>(V));
  for (const auto &Usr : V->users()) {
    Instruction *ToAdd = cast<Instruction>(Usr);
    auto Found = m_AlreadyAdded.find(ToAdd);
    if (Found == m_AlreadyAdded.end()) {
      m_AlreadyAdded.insert(ToAdd);
      m_AIUsers.push(ToAdd);
    }
  }
}

void GenXThreadPrivateMemory::collectEachPossibleTPMUsers() {
  IGC_ASSERT(m_AIUsers.empty());
  // At first collect every alloca user
  for (auto B = m_allocaToIntrinsic.begin(), E = m_allocaToIntrinsic.end();
       B != E; ++B) {
    Instruction *I = dyn_cast<Instruction>(B->first);
    IGC_ASSERT(I);
    addUsers(I);
  }
  // Then collect all pointer args - they may be used
  // in loads/stores we need to lower to svm intrinsics
  // m_args already contatins only args that require processing
  for (auto &Arg : m_args)
    addUsers(Arg);
}

void GenXThreadPrivateMemory::addUsersIfNeeded(Value *V) {
  bool isGatherScatterPrivate = false;
  if (IntrinsicInst *CI = dyn_cast<IntrinsicInst>(V)) {
    unsigned ID = GenXIntrinsic::getAnyIntrinsicID(CI);
    switch (ID) {
    case GenXIntrinsic::genx_gather_private:
    case GenXIntrinsic::genx_scatter_private:
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
      isGatherScatterPrivate = true;
      break;
    default:
      break;
    }
  }
  if (!isa<LoadInst>(V) && !isa<StoreInst>(V) &&
      V->getType()->getScalarType()->isIntegerTy(1))
    return;
  if (m_useGlobalMem ||
      (!isa<LoadInst>(V) && !isa<StoreInst>(V) && !isGatherScatterPrivate))
    addUsers(V);
}

bool GenXThreadPrivateMemory::runOnModule(Module &M) {
  m_ST = &getAnalysis<TargetPassConfig>()
              .getTM<GenXTargetMachine>()
              .getGenXSubtarget();
  if (!m_ST->isOCLRuntime())
    m_useGlobalMem = false;
  for (auto &F : M)
    visit(F);
  if (m_useGlobalMem ||
      (m_ST->isOCLRuntime() && std::find_if(m_alloca.begin(), m_alloca.end(),
                                            SVMChecker()) != m_alloca.end())) {
    LLVM_DEBUG(dbgs() << "Switching TPM to SVM\n");
    M.addModuleFlag(Module::ModFlagBehavior::Error, ModuleMD::UseSVMStack, 1);
    m_useGlobalMem = true;
  }
  bool Result = false;
  for (auto &F : M)
    Result |= runOnFunction(F);
  return Result;
}

bool GenXThreadPrivateMemory::runOnFunction(Function &F) {
  // skip function which is not a kernel or stackfunc
  // typically it's an emulation-related func (__cm_intrinsic_impl_*)
  if (GenXIntrinsic::getAnyIntrinsicID(&F) !=
          GenXIntrinsic::not_any_intrinsic ||
      !(F.hasFnAttribute(genx::FunctionMD::CMStackCall) ||
        F.hasFnAttribute(genx::FunctionMD::CMGenXMain)))
    return false;
  LLVM_DEBUG(dbgs() << "Running TPM on " << F.getName() << "\n");
  m_DL = &F.getParent()->getDataLayout();
  m_stack = m_ST->stackSurface();

  m_ctx = &F.getContext();
  m_DL = &F.getParent()->getDataLayout();
  m_alloca.clear();
  m_args.clear();
  m_gather.clear();
  m_scatter.clear();
  m_allocaToIntrinsic.clear();
  m_AIUsers = {};
  m_AlreadyAdded.clear();

  visit(F);

  for (auto Alloca : m_alloca) {
    Type *AllocaTy = Alloca->getAllocatedType();

    auto IID = llvm::GenXIntrinsic::genx_alloca;
    Function *IntrDecl = GenXIntrinsic::getGenXDeclaration(
        Alloca->getModule(), IID,
        {IntegerType::get(*m_ctx,
                          (m_useGlobalMem ? genx::QWordBits : genx::DWordBits)),
         AllocaTy});
    CallInst *AllocaIntr =
        IntrinsicInst::Create(IntrDecl, {Constant::getNullValue(AllocaTy)});
    AllocaIntr->insertAfter(Alloca);
    m_allocaToIntrinsic[Alloca] = AllocaIntr;
  }

  // Firstly, we resolve dependencies in PHI nodes (see comments in
  // preparePhiForReplacement).
  collectEachPossibleTPMUsers();
  bool Changed = false;
  while (!m_AIUsers.empty()) {
    Instruction *I = m_AIUsers.front();
    m_AIUsers.pop();

    addUsersIfNeeded(I);

    if (PHINode *Phi = dyn_cast<PHINode>(I))
      Changed |= preparePhiForReplacement(Phi);
  }

  // Main loop where instructions are replaced one by one.
  m_AlreadyAdded.clear();
  collectEachPossibleTPMUsers();
  while (!m_AIUsers.empty()) {
    Instruction *I = m_AIUsers.front();
    LLVM_DEBUG(dbgs() << "Processing inst: " << *I << "\n");
    m_AIUsers.pop();

    addUsersIfNeeded(I);

    if (auto *LdI = dyn_cast<LoadInst>(I))
      Changed |= replaceLoad(LdI);
    else if (auto *StI = dyn_cast<StoreInst>(I))
      Changed |= replaceStore(StI);
    else if (auto *PTI = dyn_cast<PtrToIntInst>(I))
      Changed |= replacePTI(PTI);
    else if (auto* AddrCast = dyn_cast<AddrSpaceCastInst>(I))
      Changed |= replaceAddrSpaceCast(AddrCast);
    else if (isa<IntToPtrInst>(I) || isa<BitCastInst>(I)) {
      // resolve all IntToPtr users and remove it.
      if (I->use_empty()) {
        I->eraseFromParent();
        Changed = true;
      }
    } else if (auto *CI = dyn_cast<CallInst>(I)) {
      unsigned ID = GenXIntrinsic::getAnyIntrinsicID(CI);
      if (ID == GenXIntrinsic::genx_gather_private)
        Changed |= replaceGatherPrivate(CI);
      else if (ID == GenXIntrinsic::genx_scatter_private)
        Changed |= replaceScatterPrivate(CI);
      else if (ID == Intrinsic::lifetime_start ||
               ID == Intrinsic::lifetime_end) {
        CI->eraseFromParent();
        Changed = true;
      } else if (ID == GenXIntrinsic::not_any_intrinsic) {
        bool ArgChanged = false;
        std::for_each(CI->arg_begin(), CI->arg_end(),
                      [this, &CI, &ArgChanged](Value *Op) {
                        if (auto *AI = dyn_cast<AllocaInst>(Op)) {
                          CI->replaceUsesOfWith(AI, m_allocaToIntrinsic.at(AI));
                          ArgChanged = true;
                        }
                      });
        IGC_ASSERT_MESSAGE(
            ArgChanged, "Cannot analyze modified alloca passed to other func");
        Changed = true;
      }
    } else if (PHINode *Phi = dyn_cast<PHINode>(I)) {
      if (isa<PointerType>(Phi->getType()))
        Changed |= replacePhi(Phi);
    } else if (SelectInst *Sel = dyn_cast<SelectInst>(I)) {
      if (isa<PointerType>(Sel->getType()))
        Changed |= replaceSelect(Sel);
    }

    if (m_AIUsers.empty()) {
      if (!Changed)
        report_fatal_error("Thread private memory: cannot resolve all alloca uses");
      Changed = false;
      collectEachPossibleTPMUsers();
    }
  }

  for (auto AllocaPair : m_allocaToIntrinsic) {
    EraseUsers(AllocaPair.first);
    IGC_ASSERT_MESSAGE(AllocaPair.first->use_empty(),
      "uses of replaced alloca aren't empty");
    AllocaPair.first->eraseFromParent();
  }

  // TODO: Rewrite split conditions due to possible exec sizes are 1, 2, 4, 8,
  // 16 and 32.
  for (auto CI : m_gather) {
    Type *DstTy = CI->getType();
    unsigned NumElts = cast<VectorType>(DstTy)->getNumElements();
    unsigned EltSz =
        cast<VectorType>(DstTy)->getElementType()->getPrimitiveSizeInBits();
    unsigned ExecSz = NumElts * EltSz;

    if (ExecSz > 2 * genx::GRFBits || NumElts > 32)
      SplitGather(CI);
  }

  for (auto CI : m_scatter) {
    Type *DataTy =
        CI->getArgOperand(m_useGlobalMem ? 3 : 5)->getType();
    unsigned NumElts = cast<VectorType>(DataTy)->getNumElements();
    unsigned EltSz =
        cast<VectorType>(DataTy)->getElementType()->getPrimitiveSizeInBits();
    unsigned ExecSz = NumElts * EltSz;

    if (ExecSz > 2 * genx::GRFBits || NumElts > 32)
      SplitScatter(CI);
  }

  return !m_allocaToIntrinsic.empty();
}

void GenXThreadPrivateMemory::visitAllocaInst(AllocaInst &I) {
  m_alloca.push_back(&I);
}

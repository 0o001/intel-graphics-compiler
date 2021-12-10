/*========================== begin_copyright_notice ============================

Copyright (C) 2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include "vc/Utils/General/Types.h"
#include "llvmWrapper/Support/TypeSize.h"

#include "Probe/Assertion.h"

#include <llvm/Support/Casting.h>
#include "llvmWrapper/IR/DerivedTypes.h"

using namespace llvm;

IGCLLVM::FixedVectorType *vc::changeAddrSpace(IGCLLVM::FixedVectorType *OrigTy,
                                              int AddrSpace) {
  IGC_ASSERT_MESSAGE(OrigTy, "wrong argument");
  auto *PointeeTy = OrigTy->getElementType()->getPointerElementType();
  auto EC = OrigTy->getNumElements();
  return IGCLLVM::FixedVectorType::get(
      llvm::PointerType::get(PointeeTy, AddrSpace), EC);
}

Type *vc::changeAddrSpace(Type *OrigTy, int AddrSpace) {
  IGC_ASSERT_MESSAGE(OrigTy, "wrong argument");
  IGC_ASSERT_MESSAGE(
      OrigTy->isPtrOrPtrVectorTy(),
      "wrong argument: pointer or vector of pointers type is expected");
  if (OrigTy->isPointerTy())
    return changeAddrSpace(cast<PointerType>(OrigTy), AddrSpace);
  return changeAddrSpace(cast<IGCLLVM::FixedVectorType>(OrigTy), AddrSpace);
}

int vc::getAddrSpace(Type *PtrOrPtrVec) {
  IGC_ASSERT_MESSAGE(PtrOrPtrVec, "wrong argument");
  IGC_ASSERT_MESSAGE(
      PtrOrPtrVec->isPtrOrPtrVectorTy(),
      "wrong argument: pointer or vector of pointers type is expected");
  if (PtrOrPtrVec->isPointerTy())
    return PtrOrPtrVec->getPointerAddressSpace();
  return cast<VectorType>(PtrOrPtrVec)->getElementType()->getPointerAddressSpace();
}

const Type &vc::fixDegenerateVectorType(const Type &Ty) {
  if (!isa<IGCLLVM::FixedVectorType>(Ty))
    return Ty;
  auto &VecTy = cast<IGCLLVM::FixedVectorType>(Ty);
  if (VecTy.getNumElements() != 1)
    return Ty;
  return *VecTy.getElementType();
}

Type &vc::fixDegenerateVectorType(Type &Ty) {
  return const_cast<Type &>(
      fixDegenerateVectorType(static_cast<const Type &>(Ty)));
}

// calculates new return type for cast instructions
// * trunc
// * bitcast
// Expect that scalar type of instruction not changed and previous
// combination of OldOutType & OldInType is valid
Type *vc::getNewTypeForCast(Type *OldOutType, Type *OldInType,
                            Type *NewInType) {
  IGC_ASSERT_MESSAGE(OldOutType && NewInType && OldInType,
                     "Error: nullptr input");

  auto NewInVecType = dyn_cast<IGCLLVM::FixedVectorType>(NewInType);
  auto OldOutVecType = dyn_cast<IGCLLVM::FixedVectorType>(OldOutType);
  auto OldInVecType = dyn_cast<IGCLLVM::FixedVectorType>(OldInType);

  bool NewInIsPtrOrVecPtr = NewInType->isPtrOrPtrVectorTy();
  bool OldOutIsPtrOrVecPtr = OldOutType->isPtrOrPtrVectorTy();
  bool OldInIsPtrOrVecPtr = OldInType->isPtrOrPtrVectorTy();

  // only  pointer to pointer
  IGC_ASSERT(NewInIsPtrOrVecPtr == OldOutIsPtrOrVecPtr &&
             NewInIsPtrOrVecPtr == OldInIsPtrOrVecPtr);

  // <2 x char> -> int : < 4 x char> -> ? forbidden
  IGC_ASSERT((bool)OldOutVecType == (bool)OldInVecType &&
             (bool)OldOutVecType == (bool)NewInVecType);

  Type *NewOutType = OldOutType;
  if (OldOutVecType) {
    // <4 x char> -> <2 x int> : <8 x char> -> <4 x int>
    // <4 x char> -> <2 x int> : <2 x char> -> <1 x int>
    auto NewInEC  = NewInVecType->getNumElements();
    auto OldOutEC = OldOutVecType->getNumElements();
    auto OldInEC  = OldInVecType->getNumElements();
    auto NewOutEC = OldOutEC * NewInEC / OldInEC;
    // <4 x char> -> <2 x int> : <5 x char> -> ? forbidden
    IGC_ASSERT_MESSAGE((OldOutEC * NewInEC) % OldInEC == 0,
                       "Error: wrong combination of input/output");
    // element count changed, scalar type as previous
    NewOutType = IGCLLVM::FixedVectorType::get(
        OldOutVecType->getElementType(), NewOutEC);
  }

  IGC_ASSERT(NewOutType);

  if (NewInIsPtrOrVecPtr) {
    // <4 x char*> -> <2 x half*> : < 2 x int*> - ? forbidden
    // char* -> half* : int* -> ? forbidden
    IGC_ASSERT_MESSAGE(OldInType->getScalarType()->getPointerElementType() ==
                           NewInType->getScalarType()->getPointerElementType(),
                       "Error: unexpected type change");
    // address space from new
    // element count calculated as for vector
    // element type expect address space similar
    auto AddressSpace = getAddrSpace(NewInType);
    return changeAddrSpace(NewOutType, AddressSpace);
  }
  // <4 x char> -> <2 x half> : < 2 x int> - ? forbiddeb
  IGC_ASSERT_MESSAGE(OldInType->getScalarType() == NewInType->getScalarType(),
                     "Error: unexpected type change");
  return NewOutType;
}

IGCLLVM::FixedVectorType &vc::getVectorType(Type &Ty) {
  if (isa<IGCLLVM::FixedVectorType>(Ty))
    return cast<IGCLLVM::FixedVectorType>(Ty);
  return *IGCLLVM::FixedVectorType::get(&Ty, 1);
}

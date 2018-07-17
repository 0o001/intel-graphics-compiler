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

#include "Compiler/CodeGenContextWrapper.hpp"
#include "Compiler/MetaDataUtilsWrapper.h"
#include "Compiler/CISACodeGen/RegisterPressureEstimate.hpp"
#include "Compiler/CISACodeGen/WIAnalysis.hpp"
#include "common/LLVMUtils.h"

#include "Compiler/CISACodeGen/LowerGEPForPrivMem.hpp"
#include "Compiler/CodeGenPublic.h"
#include "Compiler/IGCPassSupport.h"
#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"
#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/ADT/SmallVector.h>
#include "common/LLVMWarningsPop.hpp"

#define MAX_ALLOCA_PROMOTE_GRF_NUM      48
#define MAX_PRESSURE_GRF_NUM            64

using namespace llvm;
using namespace IGC;
using namespace IGC::IGCMD;

namespace IGC {
/// @brief  LowerGEPForPrivMem pass is used for lowering the allocas identified while visiting the alloca instructions
///         and then inserting insert/extract elements instead of load stores. This allows us
///         to store the data in registers instead of propagating it to scratch space.
class LowerGEPForPrivMem : public llvm::FunctionPass, public llvm::InstVisitor<LowerGEPForPrivMem>
{
public:
    LowerGEPForPrivMem();

    ~LowerGEPForPrivMem() {}

    virtual llvm::StringRef getPassName() const override
    {
        return "LowerGEPForPrivMem";
    }

    virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const override
    {
        AU.addRequired<RegisterPressureEstimate>();
        AU.addRequired<MetaDataUtilsWrapper>();
        AU.addRequired<CodeGenContextWrapper>();
        AU.addRequired<WIAnalysis>();
        AU.setPreservesCFG();
    }

    virtual bool runOnFunction(llvm::Function &F) override;

    void visitAllocaInst(llvm::AllocaInst &I);

    unsigned int extractAllocaSize(llvm::AllocaInst* pAlloca);

private:
    llvm::AllocaInst* createVectorForAlloca(
        llvm::AllocaInst *pAlloca,
        llvm::Type *pBaseType);
    void handleAllocaInst(llvm::AllocaInst *pAlloca);

    bool CheckIfAllocaPromotable(llvm::AllocaInst* pAlloca);

    /// Conservatively check if a store allow an Alloca to be uniform
    bool IsUniformStore(llvm::StoreInst* pStore);
public:
    static char ID;

private:
    const llvm::DataLayout                              *m_pDL;
    CodeGenContext                                      *m_ctx;
    std::vector<llvm::AllocaInst*>                       m_allocasToPrivMem;
    RegisterPressureEstimate*                            m_pRegisterPressureEstimate;
    llvm::Function                                      *m_pFunc;

    /// Keep track of each BB affected by promoting MemtoReg and the current pressure at that block
    llvm::DenseMap<llvm::BasicBlock *, unsigned>         m_pBBPressure;
};

FunctionPass *createPromotePrivateArrayToReg()
{
    return new LowerGEPForPrivMem();
}
}

// Register pass to igc-opt
#define PASS_FLAG "igc-priv-mem-to-reg"
#define PASS_DESCRIPTION "Lower GEP of Private Memory to Register Pass"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(LowerGEPForPrivMem, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(RegisterPressureEstimate)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_DEPENDENCY(CodeGenContextWrapper)
IGC_INITIALIZE_PASS_END(LowerGEPForPrivMem, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char LowerGEPForPrivMem::ID = 0;

LowerGEPForPrivMem::LowerGEPForPrivMem() : FunctionPass(ID), m_pFunc(nullptr)
{
    initializeLowerGEPForPrivMemPass(*PassRegistry::getPassRegistry());
}

llvm::AllocaInst* LowerGEPForPrivMem::createVectorForAlloca(
    llvm::AllocaInst* pAlloca,
    llvm::Type* pBaseType)
{
    IRBuilder<> IRB(pAlloca);

    unsigned int totalSize = extractAllocaSize(pAlloca) / int_cast<unsigned int>(m_pDL->getTypeAllocSize(pBaseType));

    llvm::VectorType* pVecType = llvm::VectorType::get(pBaseType, totalSize);

    AllocaInst *pAllocaValue = IRB.CreateAlloca(pVecType, 0);
    return pAllocaValue;
}

bool LowerGEPForPrivMem::runOnFunction(llvm::Function &F)
{
    m_pFunc = &F;
    CodeGenContextWrapper* pCtxWrapper = &getAnalysis<CodeGenContextWrapper>();
    m_ctx = pCtxWrapper->getCodeGenContext();

    MetaDataUtils *pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    if (pMdUtils->findFunctionsInfoItem(&F) == pMdUtils->end_FunctionsInfo())
    {
        return false;
    }
    m_pDL = &F.getParent()->getDataLayout();
    m_pRegisterPressureEstimate = &getAnalysis<RegisterPressureEstimate>();

    m_allocasToPrivMem.clear();

    visit(F);

    std::vector<llvm::AllocaInst*> &allocaToHande = m_allocasToPrivMem;
    for (auto pAlloca : allocaToHande)
    {
        handleAllocaInst(pAlloca);
    }

    // Last remove alloca instructions
    for (auto pInst : allocaToHande)
    {
        if (pInst->use_empty())
        {
            pInst->eraseFromParent();
        }
    }

    if (!allocaToHande.empty())
        DumpLLVMIR(m_ctx, "AfterLowerGEP");
    // IR changed only if we had alloca instruction to optimize
    return !allocaToHande.empty();
}

void TransposeHelper::EraseDeadCode()
{
    for(auto pInst = m_toBeRemovedGEP.rbegin(); pInst != m_toBeRemovedGEP.rend(); ++pInst)
    {
        assert((*pInst)->use_empty() && "Instruction still has usage");
        (*pInst)->eraseFromParent();
    }
}

unsigned int LowerGEPForPrivMem::extractAllocaSize(llvm::AllocaInst* pAlloca)
{
    unsigned int arraySize = int_cast<unsigned int>(cast<ConstantInt>(pAlloca->getArraySize())->getZExtValue());
    unsigned int totalArrayStructureSize = int_cast<unsigned int>(m_pDL->getTypeAllocSize(pAlloca->getAllocatedType()) * arraySize);

    return totalArrayStructureSize;
}

bool LowerGEPForPrivMem::CheckIfAllocaPromotable(llvm::AllocaInst* pAlloca)
{
    unsigned int allocaSize = extractAllocaSize(pAlloca);
    unsigned int allowedAllocaSizeInBytes = MAX_ALLOCA_PROMOTE_GRF_NUM * 4;

    // scale alloc size based on the number of GRFs we have
    float grfRatio = m_ctx->getNumGRFPerThread() / 128.0f;
    allowedAllocaSizeInBytes = (uint32_t) (allowedAllocaSizeInBytes * grfRatio);

    if (m_ctx->type == ShaderType::COMPUTE_SHADER)
    {
        ComputeShaderContext* ctx = static_cast<ComputeShaderContext*>(m_ctx);
        SIMDMode simdMode = ctx->GetLeastSIMDModeAllowed();
        unsigned d = simdMode == SIMDMode::SIMD32 ? 4 : 1;

        allowedAllocaSizeInBytes = allowedAllocaSizeInBytes / d;
    }
    std::vector<Type*> accessType;
    if(!CanUseSOALayout(pAlloca, accessType))
    {
        return false;
    }
    auto WI = &getAnalysis<WIAnalysis>();
    bool isUniformAlloca = WI->whichDepend(pAlloca) == WIAnalysis::UNIFORM;
    if(isUniformAlloca)
    {
        // Heuristic: for uniform alloca we divide the size by 8 to adjust the pressure
        // as they will be allocated as uniform array
        allocaSize = iSTD::Round(allocaSize, 8) / 8;
    }

    if(allocaSize <= IGC_GET_FLAG_VALUE(ByPassAllocaSizeHeuristic))
    {
        return true;
    }

    // if alloca size exceeds alloc size threshold, return false
    if (allocaSize > allowedAllocaSizeInBytes)
    {
        return false;
    }
    // if no live range info
    if (!m_pRegisterPressureEstimate->isAvailable())
    {
        return true;
    }

    // get all the basic blocks that contain the uses of the alloca
    // then estimate how much changing this alloca to register adds to the pressure at that block.
    unsigned int assignedNumber = 0;
    unsigned int lowestAssignedNumber = m_pRegisterPressureEstimate->getMaxAssignedNumberForFunction();
    unsigned int highestAssignedNumber = 0;

    for (auto II = pAlloca->user_begin(), IE = pAlloca->user_end(); II != IE; ++II)
    {
        if (Instruction* inst = dyn_cast<Instruction>(*II))
        {
            assignedNumber = m_pRegisterPressureEstimate->getAssignedNumberForInst(inst);
            lowestAssignedNumber = (lowestAssignedNumber < assignedNumber) ? lowestAssignedNumber : assignedNumber;
            highestAssignedNumber = (highestAssignedNumber > assignedNumber) ? highestAssignedNumber : assignedNumber;
        }
    }
    
    // find all the BB's that lie in the liverange of lowestAssignedNumber 
    // and highestAssignedNumber for the use of the alloca instruction
    auto &BBs = m_pFunc->getBasicBlockList();
    DenseSet<BasicBlock*> bbList;
    for (auto BI = BBs.begin(), BE = BBs.end(); BI != BE; ++BI)
    {
        BasicBlock *BB = &*BI;
        unsigned int bbMaxAssignedNumber = m_pRegisterPressureEstimate->getMaxAssignedNumberForBB(BB);
        unsigned int bbMinAssignedNumber = m_pRegisterPressureEstimate->getMinAssignedNumberForBB(BB);
        if (((lowestAssignedNumber >= bbMinAssignedNumber) && (lowestAssignedNumber <= bbMaxAssignedNumber)) ||
            ((bbMinAssignedNumber >= lowestAssignedNumber) && (bbMinAssignedNumber <= highestAssignedNumber)))
        {
            if (!m_pBBPressure.count(BB))
            {
                m_pBBPressure[BB] = m_pRegisterPressureEstimate->getRegisterPressure(BB);
            }

            // scale alloc size based on the number of GRFs we have
            float grfRatio = m_ctx->getNumGRFPerThread() / 128.0f;
            uint32_t maxGRFPressure = (uint32_t) (grfRatio * MAX_PRESSURE_GRF_NUM * 4);
 
            if (allocaSize + m_pBBPressure[BB] > maxGRFPressure)
            {
                return false;
            }

            bbList.insert(BB);
        }
    }

    for(auto it : bbList)
    {
        m_pBBPressure[it] += allocaSize;
    }
    return true;
}

static Type* GetBaseType(Type* pType)
{
    if(pType->isStructTy())
    {
        int num_elements = pType->getStructNumElements();
        if(num_elements > 1)
            return nullptr;

        pType = pType->getStructElementType(0);
    }

    while(pType->isArrayTy())
    {
        pType = pType->getArrayElementType();
    }

    if(pType->isStructTy())
    {
        int num_elements = pType->getStructNumElements();
        if(num_elements > 1)
            return nullptr;

        pType = pType->getStructElementType(0);
    }

    Type* pBaseType = nullptr;
    if(pType->isVectorTy())
    {
        pBaseType = pType->getContainedType(0);
    }
    else
    {
        pBaseType = pType;
    }
    return pBaseType;
}

static bool CheckUsesForSOAAlyout(Instruction* I, std::vector<Type*>& accessType)
{
    for(Value::user_iterator use_it = I->user_begin(), use_e = I->user_end(); use_it != use_e; ++use_it)
    {
        if(GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(*use_it))
        {
            if(CheckUsesForSOAAlyout(gep, accessType))
                continue;
        }
        if(llvm::LoadInst* pLoad = llvm::dyn_cast<llvm::LoadInst>(*use_it))
        {
            if(!pLoad->isSimple())
                return false;
            accessType.push_back(pLoad->getPointerOperand()->getType()->getPointerElementType());
        }
        else if(llvm::StoreInst* pStore = llvm::dyn_cast<llvm::StoreInst>(*use_it))
        {
            if(!pStore->isSimple())
                return false;
            llvm::Value* pValueOp = pStore->getValueOperand();
            if(pValueOp == I)
            {
                // GEP instruction is the stored value of the StoreInst (not supported case)
                return false;
            }
            accessType.push_back(pStore->getPointerOperand()->getType()->getPointerElementType());
        }
        else if(llvm::BitCastInst *pBitCast = llvm::dyn_cast<llvm::BitCastInst>(*use_it))
        {
            Type* baseT = GetBaseType(pBitCast->getType()->getPointerElementType());
            Type* sourceType = GetBaseType(pBitCast->getOperand(0)->getType()->getPointerElementType());
            if(pBitCast->use_empty())
            {
                continue;
            }
            else if(baseT != nullptr &&
                baseT->getPrimitiveSizeInBits() != 0 &&
                baseT->getPrimitiveSizeInBits() == sourceType->getPrimitiveSizeInBits())
            {
                if(CheckUsesForSOAAlyout(pBitCast, accessType))
                    continue;
            }
            else if(IsBitCastForLifetimeMark(pBitCast))
            {
                continue;
            }
            // Not a candidate.
            return false;
        }
        else if(IntrinsicInst* intr = dyn_cast<IntrinsicInst>(*use_it))
        {
            llvm::Intrinsic::ID  IID = intr->getIntrinsicID();
            if(IID == llvm::Intrinsic::lifetime_start ||
                IID == llvm::Intrinsic::lifetime_end)
            {
                continue;
            }
            return false;
        }
        else
        {
            // This is some other instruction. Right now we don't want to handle these
            return false;
        }
    }
    return true;
}


bool IGC::CanUseSOALayout(AllocaInst* I, std::vector<Type*>& accessType)
{
    // Don't even look at non-array allocas.
    // (extractAllocaDim can not handle them anyway, causing a crash)
    llvm::Type* pType = I->getType()->getPointerElementType();
    if(pType->isStructTy() && pType->getStructNumElements() == 1)
    {
        pType = pType->getStructElementType(0);
    }
    if((!pType->isArrayTy() && !pType->isVectorTy()) || I->isArrayAllocation())
        return false;

    Type* base = GetBaseType(pType);
    if(base == nullptr)
        return false;
    // only handle case with a simple base type
    if(!(base->isFloatingPointTy() || base->isIntegerTy()))
        return false;
    return CheckUsesForSOAAlyout(I, accessType);
}

void LowerGEPForPrivMem::visitAllocaInst(AllocaInst &I)
{
    // Alloca should always be private memory
    assert(I.getType()->getAddressSpace() == ADDRESS_SPACE_PRIVATE);
    if (!CheckIfAllocaPromotable(&I))
    {
        // alloca size extends remain per-lane-reg space
        return;
    }
    m_allocasToPrivMem.push_back(&I);
}

void TransposeHelper::HandleAllocaSources(Instruction* v, Value* idx)
{
    SmallVector<Value*, 10> instructions;
    for(Value::user_iterator it = v->user_begin(), e = v->user_end(); it != e; ++it)
    {
        Value* inst = cast<Value>(*it);
        instructions.push_back(inst);
    }
    
    for(auto instruction : instructions)
    {
        if(GetElementPtrInst *pGEP = dyn_cast<GetElementPtrInst>(instruction))
        {
            handleGEPInst(pGEP, idx);
        }
        else if(BitCastInst* bitcast = dyn_cast<BitCastInst>(instruction))
        {
            m_toBeRemovedGEP.push_back(bitcast);
            HandleAllocaSources(bitcast, idx);
        }
        else if(StoreInst *pStore = llvm::dyn_cast<StoreInst>(instruction))
        {
            handleStoreInst(pStore, idx);
        }
        else if(LoadInst *pLoad = llvm::dyn_cast<LoadInst>(instruction))
        {
            handleLoadInst(pLoad, idx);
        }
        else if(IntrinsicInst* inst = dyn_cast<IntrinsicInst>(instruction))
        {
            handleLifetimeMark(inst);
        }
    }
}

class TransposeHelperPromote : public TransposeHelper
{
public:
    void handleLoadInst(
        LoadInst *pLoad,
        Value *pScalarizedIdx);
    void handleStoreInst(
        StoreInst *pStore,
        Value *pScalarizedIdx);
    AllocaInst *pVecAlloca;
    TransposeHelperPromote(AllocaInst* pAI) : TransposeHelper(false) { pVecAlloca = pAI; }
};

void LowerGEPForPrivMem::handleAllocaInst(llvm::AllocaInst* pAlloca)
{
    // Extract the Alloca size and the base Type
    Type* pType = pAlloca->getType()->getPointerElementType();
    Type* pBaseType = GetBaseType(pType);
	assert(pBaseType);
    llvm::AllocaInst* pVecAlloca = createVectorForAlloca(pAlloca, pBaseType);
    if (!pVecAlloca)
    {
        return;
    }
    
    IRBuilder<> IRB(pVecAlloca);
    Value* idx = IRB.getInt32(0);
    TransposeHelperPromote helper(pVecAlloca);
    helper.HandleAllocaSources(pAlloca, idx);
    helper.EraseDeadCode();
}

void TransposeHelper::handleLifetimeMark(IntrinsicInst *inst)
{
    assert(inst->getIntrinsicID() == llvm::Intrinsic::lifetime_start ||
        inst->getIntrinsicID() == llvm::Intrinsic::lifetime_end);
    inst->eraseFromParent();
}

void TransposeHelper::handleGEPInst(
    llvm::GetElementPtrInst *pGEP,
    llvm::Value* idx)
{
    assert(static_cast<ADDRESS_SPACE>(pGEP->getPointerAddressSpace()) == ADDRESS_SPACE_PRIVATE);
    // Add GEP instruction to remove list
    m_toBeRemovedGEP.push_back(pGEP);
    if (pGEP->use_empty())
    {
        // GEP has no users, do nothing.
        return;
    }

    // Given %p = getelementptr [4 x [3 x <2 x float>]]* %v, i64 0, i64 %1, i64 %2
    // compute the scalarized index with an auxiliary array [4, 3, 2]:
    //
    // Formula: index = (%1 x 3 + %2) x 2
    //
    IRBuilder<> IRB(pGEP);
    Value *pScalarizedIdx = IRB.getInt32(0);
    Type* T = pGEP->getPointerOperandType()->getPointerElementType();
    for (unsigned i = 0, e = pGEP->getNumIndices(); i < e; ++i)
    {
        auto GepOpnd = IRB.CreateZExtOrTrunc(pGEP->getOperand(i + 1), IRB.getInt32Ty());
        unsigned int arr_sz = 1;
        if(T->isStructTy())
        {
            arr_sz = 1;
            T = T->getStructElementType(0);
        }
        else if(T->isArrayTy())
        {
            arr_sz = int_cast<unsigned int>(T->getArrayNumElements());
            T = T->getArrayElementType();
        }
        else if(T->isVectorTy())
        {
            // based on whether we want the index in number of element or number of vector
            if(m_vectorIndex)
            {
                arr_sz = 1;
            }
            else
            {
                arr_sz = T->getVectorNumElements();
            }
            T = T->getVectorElementType();
        }

        pScalarizedIdx = IRB.CreateNUWAdd(pScalarizedIdx, GepOpnd);
        pScalarizedIdx = IRB.CreateNUWMul(pScalarizedIdx, IRB.getInt32(arr_sz));
    }
    pScalarizedIdx = IRB.CreateNUWAdd(pScalarizedIdx, idx);
    HandleAllocaSources(pGEP, pScalarizedIdx);
}

// Load N elements from a vector alloca, Idx, ... Idx + N - 1. Return a scalar
// or a vector value depending on N.
static Value *loadEltsFromVecAlloca(
    unsigned N, AllocaInst *pVecAlloca,
    Value *pScalarizedIdx, 
    IRBuilder<> &IRB,
    Type* scalarType)
{
    Value *pLoadVecAlloca = IRB.CreateLoad(pVecAlloca);
    if (N == 1)
    {
        return IRB.CreateBitCast(
            IRB.CreateExtractElement(pLoadVecAlloca, pScalarizedIdx),
            scalarType);
    }

    // A vector load
    // %v = load <2 x float>* %ptr
    // becomes
    // %w = load <32 x float>* %ptr1
    // %v0 = extractelement <32 x float> %w, i32 %idx
    // %v1 = extractelement <32 x float> %w, i32 %idx+1
    // replace all uses of %v with <%v0, %v1>
    assert(N > 1 && "out of sync");
    Type* Ty = VectorType::get(scalarType, N);
    Value *Result = UndefValue::get(Ty);

    for(unsigned i = 0; i < N; ++i)
    {
        Value *VectorIdx = ConstantInt::get(pScalarizedIdx->getType(), i);
        auto Idx = IRB.CreateAdd(pScalarizedIdx, VectorIdx);
        auto Val = IRB.CreateExtractElement(pLoadVecAlloca, Idx);
        Val = IRB.CreateBitCast(Val, scalarType);
        Result = IRB.CreateInsertElement(Result, Val, VectorIdx);
    }
    return Result;
}

void TransposeHelperPromote::handleLoadInst(
    LoadInst *pLoad,
    Value *pScalarizedIdx)
{
    assert(pLoad->isSimple());
    IRBuilder<> IRB(pLoad);
    unsigned N = pLoad->getType()->isVectorTy()
                     ? pLoad->getType()->getVectorNumElements()
                     : 1;
    Value *Val = loadEltsFromVecAlloca(N, pVecAlloca, pScalarizedIdx, IRB, pLoad->getType()->getScalarType());
    pLoad->replaceAllUsesWith(Val);
    pLoad->eraseFromParent();
}

void TransposeHelperPromote::handleStoreInst(
    llvm::StoreInst *pStore,
    llvm::Value *pScalarizedIdx)
{
    // Add Store instruction to remove list
    assert(pStore->isSimple());

    IRBuilder<> IRB(pStore);
    llvm::Value* pStoreVal = pStore->getValueOperand();
    llvm::Value* pLoadVecAlloca = IRB.CreateLoad(pVecAlloca);
    llvm::Value* pIns = pLoadVecAlloca;
    if (pStoreVal->getType()->isVectorTy())
    {
        // A vector store
        // store <2 x float> %v, <2 x float>* %ptr
        // becomes
        // %w = load <32 x float> *%ptr1
        // %v0 = extractelement <2 x float> %v, i32 0
        // %w0 = insertelement <32 x float> %w, float %v0, i32 %idx
        // %v1 = extractelement <2 x float> %v, i32 1
        // %w1 = insertelement <32 x float> %w0, float %v1, i32 %idx+1
        // store <32 x float> %w1, <32 x float>* %ptr1
        for (unsigned i = 0, e = pStoreVal->getType()->getVectorNumElements(); i < e; ++i)
        {
            Value *VectorIdx = ConstantInt::get(pScalarizedIdx->getType(), i);
            auto Val = IRB.CreateExtractElement(pStoreVal, VectorIdx);
            Val = IRB.CreateBitCast(Val, pLoadVecAlloca->getType()->getScalarType());
            auto Idx = IRB.CreateAdd(pScalarizedIdx, VectorIdx);
            pIns = IRB.CreateInsertElement(pIns, Val, Idx);
        }
    }
    else
    {
        pStoreVal = IRB.CreateBitCast(pStoreVal, pLoadVecAlloca->getType()->getScalarType());
        pIns = IRB.CreateInsertElement(pLoadVecAlloca, pStoreVal, pScalarizedIdx);
    }
    IRB.CreateStore(pIns, pVecAlloca);
    pStore->eraseFromParent();
}

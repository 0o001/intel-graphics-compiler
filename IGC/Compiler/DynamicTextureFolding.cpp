/*========================== begin_copyright_notice ============================

Copyright (C) 2019-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include "Compiler/IGCPassSupport.h"
#include "Compiler/InitializePasses.h"
#include "Compiler/CodeGenPublic.h"
#include "common/secure_mem.h"
#include "DynamicTextureFolding.h"
#include "Probe/Assertion.h"

using namespace llvm;
using namespace IGC;

// Register pass to igc-opt
#define PASS_FLAG "igc-dynamic-texture-folding"
#define PASS_DESCRIPTION "dynamic texture folding"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(DynamicTextureFolding, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_END(DynamicTextureFolding, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char DynamicTextureFolding::ID = 0;

#define DEBUG_TYPE "DynamicTextureFolding"

DynamicTextureFolding::DynamicTextureFolding() : FunctionPass(ID)
{
    initializeDynamicTextureFoldingPass(*PassRegistry::getPassRegistry());
}

void DynamicTextureFolding::FoldSingleTextureValue(CallInst& I)
{
    ModuleMetaData* modMD = m_context->getModuleMetaData();

    unsigned addrSpace = 0;
    if (SampleIntrinsic *sInst = dyn_cast<SampleIntrinsic>(&I))
    {
        addrSpace = sInst->getTextureValue()->getType()->getPointerAddressSpace();
    }
    else if (SamplerLoadIntrinsic * lInst = dyn_cast<SamplerLoadIntrinsic>(&I))
    {
        addrSpace = lInst->getTextureValue()->getType()->getPointerAddressSpace();
    }
    else
    {
        return;
    }

    bool directIdx = false;
    uint textureIndex = 0;
    DecodeAS4GFXResource(addrSpace, directIdx, textureIndex);

    // if the current texture index is found in modMD as uniform texture, replace the texture load/sample as constant.
    auto it = modMD->inlineDynTextures.find(textureIndex);
    if (it != modMD->inlineDynTextures.end())
    {
        for (auto iter = I.user_begin(); iter != I.user_end(); iter++)
        {
            if (llvm::ExtractElementInst* pExtract = llvm::dyn_cast<llvm::ExtractElementInst>(*iter))
            {
                if (llvm::ConstantInt* pIdx = llvm::dyn_cast<llvm::ConstantInt>(pExtract->getIndexOperand()))
                {
                    if ((&I)->getType()->isIntOrIntVectorTy())
                    {
                        pExtract->replaceAllUsesWith(ConstantInt::get((pExtract)->getType(), (it->second[(uint32_t)(pIdx->getZExtValue())])));
                    }
                    else if ((&I)->getType()->isFPOrFPVectorTy())
                    {
                        pExtract->replaceAllUsesWith(ConstantFP::get((pExtract)->getType(), *(float*)&(it->second[(uint32_t)(pIdx->getZExtValue())])));
                    }
                }
            }
        }
    }
}
Value* DynamicTextureFolding::ShiftByLOD(Instruction* pCall, unsigned int dimension, Value* val)
{
    IRBuilder<> builder(pCall);
    Value* tmp = builder.getInt32(dimension + 1);
    Value* lod = pCall->getOperand(1);
    builder.SetInsertPoint(pCall);
    Value* Lshr =  builder.CreateLShr(tmp, lod);
    if (val)
        return builder.CreateMul(Lshr, val);
    else
        return Lshr;
}
void DynamicTextureFolding::FoldResInfoValue(llvm::GenIntrinsicInst* pCall)
{
    ModuleMetaData* modMD = m_context->getModuleMetaData();
    llvm::Value* r;
    llvm::Value* g;
    llvm::Value* b;
    llvm::Value* a;
    BufferType bufType;
    bool directIdx = false;
    uint textureIndex = 0;
    Value* texOp = pCall->getOperand(0);
    unsigned addrSpace = texOp->getType()->getPointerAddressSpace();
    bufType = DecodeAS4GFXResource(addrSpace, directIdx, textureIndex);
    if (!directIdx || (bufType != RESOURCE && bufType != UAV))
        return;
    auto I32Ty = Type::getInt32Ty(pCall->getContext());
    for(unsigned int i = 0; i < modMD->inlineResInfoData.size();i++)
    {
        if (textureIndex == modMD->inlineResInfoData[i].textureID)
        {
            ConstantInt* isLODConstant = dyn_cast<ConstantInt>(pCall->getOperand(1));
            a = ConstantInt::get(I32Ty, modMD->inlineResInfoData[i].MipCount);
            switch (modMD->inlineResInfoData[i].SurfaceType)
            {
            case GFXSURFACESTATE_SURFACETYPE_1D:
            {
                g = ConstantInt::get(I32Ty, (modMD->inlineResInfoData[i].SurfaceArray > 0) ? modMD->inlineResInfoData[i].Depth + 1 : 0);
                b = ConstantInt::get(I32Ty, 0);
                if (isLODConstant)
                {
                    uint64_t lod = isLODConstant->getZExtValue();
                    r = ConstantInt::get(I32Ty, (modMD->inlineResInfoData[i].WidthOrBufferSize + 1) >> lod);
                }
                else
                {
                    r = ShiftByLOD(dyn_cast<Instruction>(pCall), modMD->inlineResInfoData[i].WidthOrBufferSize, nullptr);
                }
                break;
            }
            case GFXSURFACESTATE_SURFACETYPE_2D:
            {
                b = ConstantInt::get(I32Ty,(modMD->inlineResInfoData[i].SurfaceArray > 0) ? modMD->inlineResInfoData[i].Depth + 1 : 0);
                if (isLODConstant)
                {
                    uint64_t lod = isLODConstant->getZExtValue();
                    r = ConstantInt::get(I32Ty, ((modMD->inlineResInfoData[i].WidthOrBufferSize + 1) >> lod)* (modMD->inlineResInfoData[i].QWidth + 1));
                    g = ConstantInt::get(I32Ty, ((modMD->inlineResInfoData[i].Height + 1) >> lod)* (modMD->inlineResInfoData[i].QHeight + 1));
                }
                else
                {
                    Value* QWidth = ConstantInt::get(I32Ty,(modMD->inlineResInfoData[i].QWidth + 1));
                    Value* QHeight = ConstantInt::get(I32Ty, (modMD->inlineResInfoData[i].QHeight + 1));
                    r = ShiftByLOD(dyn_cast<Instruction>(pCall), modMD->inlineResInfoData[i].WidthOrBufferSize, QWidth);
                    g = ShiftByLOD(dyn_cast<Instruction>(pCall), modMD->inlineResInfoData[i].Height, QHeight);
                }
                break;
            }
            case GFXSURFACESTATE_SURFACETYPE_3D:
            {
                if(isLODConstant)
                {
                    uint64_t lod = isLODConstant->getZExtValue();
                    r = ConstantInt::get(I32Ty,((modMD->inlineResInfoData[i].WidthOrBufferSize + 1) >> lod));
                    g = ConstantInt::get(I32Ty, ((modMD->inlineResInfoData[i].Height + 1) >> lod));
                    b = ConstantInt::get(I32Ty, ((modMD->inlineResInfoData[i].Depth + 1) >> lod));
                }
                else
                {
                    r = ShiftByLOD(dyn_cast<Instruction>(pCall), modMD->inlineResInfoData[i].WidthOrBufferSize, nullptr);
                    g = ShiftByLOD(dyn_cast<Instruction>(pCall), modMD->inlineResInfoData[i].Height, nullptr);
                    b = ShiftByLOD(dyn_cast<Instruction>(pCall), modMD->inlineResInfoData[i].Depth, nullptr);
                }
                break;
            }
            case GFXSURFACESTATE_SURFACETYPE_CUBE:
            {
                b = ConstantInt::get(I32Ty, (modMD->inlineResInfoData[i].SurfaceArray > 0) ? modMD->inlineResInfoData[i].Depth + 1 : 0);
                if (isLODConstant)
                {
                    uint64_t lod = isLODConstant->getZExtValue();
                    r = ConstantInt::get(I32Ty, ((modMD->inlineResInfoData[i].WidthOrBufferSize + 1) >> lod));
                    g = ConstantInt::get(I32Ty, ((modMD->inlineResInfoData[i].Height + 1) >> lod));
                }
                else
                {
                    r = ShiftByLOD(dyn_cast<Instruction>(pCall), modMD->inlineResInfoData[i].WidthOrBufferSize, nullptr);
                    g = ShiftByLOD(dyn_cast<Instruction>(pCall), modMD->inlineResInfoData[i].Height, nullptr);
                }
                break;
            }
            case GFXSURFACESTATE_SURFACETYPE_BUFFER:
            case GFXSURFACESTATE_SURFACETYPE_STRBUF:

            {
                r = (modMD->inlineResInfoData[i].WidthOrBufferSize != UINT_MAX) ? ConstantInt::get(I32Ty, modMD->inlineResInfoData[i].WidthOrBufferSize) : 0;
                g = 0;
                b = 0;
                a = 0;
                break;
            }
            default:
            {
                r = 0;
                g = 0;
                b = 0;
                a = 0;
                break;
            }
            }
            for (auto iter = pCall->user_begin(); iter != pCall->user_end(); iter++)
            {
                if (llvm::ExtractElementInst* pExtract = llvm::dyn_cast<llvm::ExtractElementInst>(*iter))
                {
                    if (llvm::ConstantInt* pIdx = llvm::dyn_cast<llvm::ConstantInt>(pExtract->getIndexOperand()))
                    {
                        if (pIdx->getZExtValue() == 0)
                        {
                            pExtract->replaceAllUsesWith(r);
                            pExtract->eraseFromParent();
                        }
                        else if (pIdx->getZExtValue() == 1)
                        {
                            pExtract->replaceAllUsesWith(g);
                            pExtract->eraseFromParent();
                        }
                        else if (pIdx->getZExtValue() == 2)
                        {
                            pExtract->replaceAllUsesWith(b);
                            pExtract->eraseFromParent();
                        }
                        else if (pIdx->getZExtValue() == 3)
                        {
                            pExtract->replaceAllUsesWith(a);
                            pExtract->eraseFromParent();
                        }
                    }
                }
            }
        }
    }
}
void DynamicTextureFolding::visitCallInst(CallInst& I)
{
    ModuleMetaData* modMD = m_context->getModuleMetaData();
    if (GenIntrinsicInst* pCall = dyn_cast<GenIntrinsicInst>(&I))
    {
        auto ID = pCall->getIntrinsicID();
        if (!IGC_IS_FLAG_ENABLED(DisableDynamicTextureFolding) && modMD->inlineDynTextures.size() != 0)
        {
            if (ID == GenISAIntrinsic::GenISA_sampleptr ||
                ID == GenISAIntrinsic::GenISA_sampleLptr ||
                ID == GenISAIntrinsic::GenISA_sampleBptr ||
                ID == GenISAIntrinsic::GenISA_sampleDptr ||
                ID == GenISAIntrinsic::GenISA_ldptr)
            {
                FoldSingleTextureValue(I);
            }
        }
        if (!IGC_IS_FLAG_ENABLED(DisableDynamicResInfoFolding) && ID == GenISAIntrinsic::GenISA_resinfoptr)
        {
            if ( modMD->inlineResInfoData.size() > 0)
            {
                FoldResInfoValue(pCall);
            }
            else
            {
                BufferType bufType;
                bool directIdx = false;
                uint textureIndex = 0;
                Value* texOp = pCall->getOperand(0);
                unsigned addrSpace = texOp->getType()->getPointerAddressSpace();
                bufType = DecodeAS4GFXResource(addrSpace, directIdx, textureIndex);
                m_ResInfoFoldingOutput[textureIndex].textureID = textureIndex;
                if (!directIdx || (bufType != RESOURCE && bufType != UAV))
                    return;
                for (auto UI = pCall->user_begin(), UE = pCall->user_end(); UI != UE; ++UI)
                {
                    if (llvm::ExtractElementInst* useInst = dyn_cast<llvm::ExtractElementInst>(*UI))
                    {
                        ConstantInt* eltID = dyn_cast<ConstantInt>(useInst->getOperand(1));
                        if (!eltID)
                            continue;
                        m_ResInfoFoldingOutput[textureIndex].value[int_cast<unsigned>(eltID->getZExtValue())] = true;
                    }
                }
            }
        }
        return;
    }
}

template<typename ContextT>
void DynamicTextureFolding::copyResInfoData(ContextT* pShaderCtx)
{
    pShaderCtx->programOutput.m_ResInfoFoldingOutput.clear();
    for (unsigned int i = 0; i < m_ResInfoFoldingOutput.size(); i++)
    {
        pShaderCtx->programOutput.m_ResInfoFoldingOutput.push_back(m_ResInfoFoldingOutput[i]);
    }
}
bool DynamicTextureFolding::doFinalization(llvm::Module& M)
{
    if (m_ResInfoFoldingOutput.size() != 0)
    {
        if (m_context->type == ShaderType::PIXEL_SHADER)
        {
            PixelShaderContext* pShaderCtx = static_cast <PixelShaderContext*>(m_context);
            copyResInfoData(pShaderCtx);
        }
        else if (m_context->type == ShaderType::VERTEX_SHADER)
        {
            VertexShaderContext* pShaderCtx = static_cast <VertexShaderContext*>(m_context);
            copyResInfoData(pShaderCtx);
        }
        else if (m_context->type == ShaderType::GEOMETRY_SHADER)
        {
            GeometryShaderContext* pShaderCtx = static_cast <GeometryShaderContext*>(m_context);
            copyResInfoData(pShaderCtx);
        }
        else if (m_context->type == ShaderType::HULL_SHADER)
        {
            HullShaderContext* pShaderCtx = static_cast <HullShaderContext*>(m_context);
            copyResInfoData(pShaderCtx);
        }
        else if (m_context->type == ShaderType::DOMAIN_SHADER)
        {
            DomainShaderContext* pShaderCtx = static_cast <DomainShaderContext*>(m_context);
            copyResInfoData(pShaderCtx);
        }
        else if (m_context->type == ShaderType::COMPUTE_SHADER)
        {
            ComputeShaderContext* pShaderCtx = static_cast <ComputeShaderContext*>(m_context);
            copyResInfoData(pShaderCtx);
        }
    }
    return false;
}

bool DynamicTextureFolding::runOnFunction(Function& F)
{
    m_context = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    visit(F);
    return false;
}

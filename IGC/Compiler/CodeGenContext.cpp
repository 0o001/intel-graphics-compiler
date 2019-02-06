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
#include "common/LLVMWarningsPush.hpp"
#include <llvm/Support/ScaledNumber.h>
#include "common/LLVMWarningsPop.hpp"

#include "Compiler/CISACodeGen/ComputeShaderCodeGen.hpp"
#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"
#include "Compiler/CodeGenPublic.h"

namespace IGC
{

typedef struct RetryState {
    bool allowUnroll;
    bool allowLICM;
    bool allowCodeSinking;
    bool allowSimd32Slicing;
    bool allowPromotePrivateMemory;
    bool allowPreRAScheduler;
    bool allowLargeURBWrite;
    unsigned nextState;
} RetryState;

static const RetryState RetryTable[] = {
    { true, true, true, false, true, true, true, 1 },
    { false, false, true, true, false, false, false, 500 }
};

RetryManager::RetryManager() : enabled(false)
{
    memset(m_simdEntries, 0, sizeof(m_simdEntries));
    firstStateId = IGC_GET_FLAG_VALUE(RetryManagerFirstStateId);
    stateId = firstStateId;
    assert(stateId < getStateCnt());
}

bool RetryManager::AdvanceState() {
    if(!enabled || IGC_IS_FLAG_ENABLED(DisableRecompilation))
    {
        return false;
    }
    assert(stateId < getStateCnt());
    stateId = RetryTable[stateId].nextState;
    return (stateId < getStateCnt());
}
bool RetryManager::AllowUnroll() {
    assert(stateId < getStateCnt());
    return RetryTable[stateId].allowUnroll;
}
bool RetryManager::AllowLICM() {
    assert(stateId < getStateCnt());
    return RetryTable[stateId].allowLICM;
}
bool RetryManager::AllowPromotePrivateMemory() {
    assert(stateId < getStateCnt());
    return RetryTable[stateId].allowPromotePrivateMemory;
}
bool RetryManager::AllowPreRAScheduler() {
    assert(stateId < getStateCnt());
    return RetryTable[stateId].allowPreRAScheduler;
}
bool RetryManager::AllowCodeSinking() {
    assert(stateId < getStateCnt());
    return RetryTable[stateId].allowCodeSinking;
}
bool RetryManager::AllowSimd32Slicing() {
    assert(stateId < getStateCnt());
    return RetryTable[stateId].allowSimd32Slicing;
}
bool RetryManager::AllowLargeURBWrite() {
    assert(stateId < getStateCnt());
    return RetryTable[stateId].allowLargeURBWrite;
}
bool RetryManager::IsFirstTry() {
    return (stateId == firstStateId);
}
bool RetryManager::IsLastTry(CodeGenContext* cgCtx) {
    return (!enabled ||
        IGC_IS_FLAG_ENABLED(DisableRecompilation) ||
        (cgCtx->getModuleMetaData()->csInfo.forcedSIMDSize != 0) ||
        (stateId < getStateCnt() && RetryTable[stateId].nextState >= getStateCnt()));
}
unsigned RetryManager::GetRetryId() const { return stateId; }

void RetryManager::Enable() { enabled = true; }
void RetryManager::Disable() { enabled = false; }

void RetryManager::SetSpillSize(unsigned int spillSize) { lastSpillSize = spillSize; }
unsigned int RetryManager::GetLastSpillSize() { return lastSpillSize; }

void RetryManager::ClearSpillParams() {
    lastSpillSize = 0;
    numInstructions = 0;
}

// save entry for given SIMD mode, to avoid recompile for next retry.
void RetryManager::SaveSIMDEntry(SIMDMode simdMode, CShader* shader)
{
    switch(simdMode)
    {
    case SIMDMode::SIMD8:   m_simdEntries[0] = shader;  break;
    case SIMDMode::SIMD16:  m_simdEntries[1] = shader;  break;
    case SIMDMode::SIMD32:  m_simdEntries[2] = shader;  break;
    default:
        assert(false);
    }
}

CShader* RetryManager::GetSIMDEntry(SIMDMode simdMode)
{
    switch(simdMode)
    {
    case SIMDMode::SIMD8:   return m_simdEntries[0];
    case SIMDMode::SIMD16:  return m_simdEntries[1];
    case SIMDMode::SIMD32:  return m_simdEntries[2];
    default:
        assert(false);
        return nullptr;
    }
}
RetryManager::~RetryManager()
{
    for(unsigned i = 0; i < 3; i++)
    {
        if(m_simdEntries[i])
        {
            delete m_simdEntries[i];
        }
    }
}

bool RetryManager::AnyKernelSpills()
{
    for(unsigned i = 0; i < 3; i++)
    {
        if(m_simdEntries[i] && m_simdEntries[i]->m_spillCost > 0.0)
        {
            return true;
        }
    }
    return false;
}

bool RetryManager::PickupKernels(CodeGenContext* cgCtx)
{
    if(cgCtx->type == ShaderType::COMPUTE_SHADER)
    {
        return PickupCS(static_cast<ComputeShaderContext*>(cgCtx));
    }
    else
    {
        assert(false && "TODO for other shader types");
        return true;
    }
}

unsigned RetryManager::getStateCnt()
{
    return sizeof(RetryTable) / sizeof(RetryState);
};

CShader* RetryManager::PickCSEntryForcedFromDriver(SIMDMode& simdMode, unsigned char forcedSIMDModeFromDriver)
{
    switch(forcedSIMDModeFromDriver)
    {
    case 8: simdMode = SIMDMode::SIMD8;
        simdMode = SIMDMode::SIMD8;
        return m_simdEntries[0];
    case 16:simdMode = SIMDMode::SIMD16;
        simdMode = SIMDMode::SIMD16;
        return m_simdEntries[1];
    case 32:simdMode = SIMDMode::SIMD32;
        simdMode = SIMDMode::SIMD32;
        return m_simdEntries[2];
    default: simdMode = SIMDMode::UNKNOWN;
        return nullptr;
    }
}

CShader* RetryManager::PickCSEntryByRegKey(SIMDMode& simdMode)
{
    if(IGC_IS_FLAG_ENABLED(ForceCSSIMD32))
    {
        simdMode = SIMDMode::SIMD32;
        return m_simdEntries[2];
    }
    else
        if(IGC_IS_FLAG_ENABLED(ForceCSSIMD16) && m_simdEntries[1])
        {
            simdMode = SIMDMode::SIMD16;
            return m_simdEntries[1];
        }
        else
            if(IGC_IS_FLAG_ENABLED(ForceCSLeastSIMD))
            {
                if(m_simdEntries[0])
                {
                    simdMode = SIMDMode::SIMD8;
                    return m_simdEntries[0];
                }
                else
                    if(m_simdEntries[1])
                    {
                        simdMode = SIMDMode::SIMD16;
                        return m_simdEntries[1];
                    }
                    else
                    {
                        simdMode = SIMDMode::SIMD32;
                        return m_simdEntries[2];
                    }
            }

    return nullptr;
}

CShader* RetryManager::PickCSEntryEarly(SIMDMode& simdMode,
    ComputeShaderContext* cgCtx)
{
    float spillThreshold = cgCtx->GetSpillThreshold();
    float occu8 = cgCtx->GetThreadOccupancy(SIMDMode::SIMD8);
    float occu16 = cgCtx->GetThreadOccupancy(SIMDMode::SIMD16);
    float occu32 = cgCtx->GetThreadOccupancy(SIMDMode::SIMD32);

    bool simd32NoSpill = m_simdEntries[2] && m_simdEntries[2]->m_spillCost <= spillThreshold;
    bool simd16NoSpill = m_simdEntries[1] && m_simdEntries[1]->m_spillCost <= spillThreshold;
    bool simd8NoSpill = m_simdEntries[0] && m_simdEntries[0]->m_spillCost <= spillThreshold;

    // If SIMD32/16/8 are all allowed, then choose one which has highest thread occupancy

    if(IGC_IS_FLAG_ENABLED(EnableHighestSIMDForNoSpill))
    {
        if(simd32NoSpill)
        {
            simdMode = SIMDMode::SIMD32;
            return m_simdEntries[2];
        }

        if(simd16NoSpill)
        {
            simdMode = SIMDMode::SIMD16;
            return m_simdEntries[1];
        }
    }
    else
    {
        if(simd32NoSpill)
        {
            if(occu32 >= occu16 && occu32 >= occu8)
            {
                simdMode = SIMDMode::SIMD32;
                return m_simdEntries[2];
            }
            // If SIMD32 doesn't spill, SIMD16 and SIMD8 shouldn't, if they exist
            assert((m_simdEntries[0] == NULL) || simd8NoSpill == true);
            assert((m_simdEntries[1] == NULL) || simd16NoSpill == true);
        }

        if(simd16NoSpill)
        {
            if(occu16 >= occu8 && occu16 >= occu32)
            {
                simdMode = SIMDMode::SIMD16;
                return m_simdEntries[1];
            }
            assert((m_simdEntries[0] == NULL) || simd8NoSpill == true); // If SIMD16 doesn't spill, SIMD8 shouldn't, if it exists
        }
    }

    bool needToRetry = false;
    if(cgCtx->m_slmSize)
    {
        if(occu16 > occu8 || occu32 > occu16)
        {
            needToRetry = true;
        }
    }

    SIMDMode maxSimdMode = cgCtx->GetMaxSIMDMode();
    if(maxSimdMode == SIMDMode::SIMD8 || !needToRetry)
    {
        if(m_simdEntries[0] && m_simdEntries[0]->m_spillSize == 0)
        {
            simdMode = SIMDMode::SIMD8;
            return m_simdEntries[0];
        }
    }
    return nullptr;
}

CShader* RetryManager::PickCSEntryFinally(SIMDMode& simdMode)
{
    if(m_simdEntries[0])
    {
        simdMode = SIMDMode::SIMD8;
        return m_simdEntries[0];
    }
    else
        if(m_simdEntries[1])
        {
            simdMode = SIMDMode::SIMD16;
            return m_simdEntries[1];
        }
        else
        {
            simdMode = SIMDMode::SIMD32;
            return m_simdEntries[2];
        }
}

void RetryManager::FreeAllocatedMemForNotPickedCS(SIMDMode simdMode)
{
    if (simdMode != SIMDMode::SIMD8 && m_simdEntries[0] != nullptr)
    {
        if (m_simdEntries[0]->ProgramOutput()->m_programBin != nullptr)
            aligned_free(m_simdEntries[0]->ProgramOutput()->m_programBin);
    }
    if (simdMode != SIMDMode::SIMD16 && m_simdEntries[1] != nullptr)
    {
        if (m_simdEntries[1]->ProgramOutput()->m_programBin != nullptr)
            aligned_free(m_simdEntries[1]->ProgramOutput()->m_programBin);
    }
    if (simdMode != SIMDMode::SIMD32 && m_simdEntries[2] != nullptr)
    {
        if (m_simdEntries[2]->ProgramOutput()->m_programBin != nullptr)
            aligned_free(m_simdEntries[2]->ProgramOutput()->m_programBin);
    }
}

bool RetryManager::PickupCS(ComputeShaderContext* cgCtx)
{
    SIMDMode simdMode;
    CComputeShader* shader = nullptr;
    SComputeShaderKernelProgram* pKernelProgram = &cgCtx->programOutput;

    shader = static_cast<CComputeShader*>(
        PickCSEntryForcedFromDriver(simdMode, cgCtx->getModuleMetaData()->csInfo.forcedSIMDSize));
    if(!shader)
    {
        shader = static_cast<CComputeShader*>(
            PickCSEntryByRegKey(simdMode));
    }
    if(!shader)
    {
        shader = static_cast<CComputeShader*>(
            PickCSEntryEarly(simdMode, cgCtx));
    }
    if(!shader && IsLastTry(cgCtx))
    {
        shader = static_cast<CComputeShader*>(
            PickCSEntryFinally(simdMode));
        assert(shader != nullptr);
    }
    if(shader)
    {
        switch(simdMode)
        {
        case SIMDMode::SIMD8:
            pKernelProgram->simd8 = *shader->ProgramOutput();
            pKernelProgram->SimdWidth = USC::GFXMEDIA_GPUWALKER_SIMD8;
            break;

        case SIMDMode::SIMD16:
            pKernelProgram->simd16 = *shader->ProgramOutput();
            pKernelProgram->SimdWidth = USC::GFXMEDIA_GPUWALKER_SIMD16;
            break;

        case SIMDMode::SIMD32:
            pKernelProgram->simd32 = *shader->ProgramOutput();
            pKernelProgram->SimdWidth = USC::GFXMEDIA_GPUWALKER_SIMD32;
            break;

        default:
            assert(false && "Invalie SIMDMode");
        }
        shader->FillProgram(pKernelProgram);

        // free allocated memory for the remaining kernels
        FreeAllocatedMemForNotPickedCS(simdMode);

        return true;
    }
    return false;
}

LLVMContextWrapper::LLVMContextWrapper(bool createResourceDimTypes)
{
    if(createResourceDimTypes)
    {
        CreateResourceDimensionTypes(*this);
    }
}

void LLVMContextWrapper::AddRef()
{
    refCount++;
}

void LLVMContextWrapper::Release()
{
    refCount--;
    if(refCount == 0)
    {
        delete this;
    }
}

/** get shader's thread group size */
unsigned ComputeShaderContext::GetThreadGroupSize()
{
    llvm::GlobalVariable* pGlobal = getModule()->getGlobalVariable("ThreadGroupSize_X");
    unsigned threadGroupSize_X = int_cast<unsigned>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());

    pGlobal = getModule()->getGlobalVariable("ThreadGroupSize_Y");
    unsigned threadGroupSize_Y = int_cast<unsigned>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());

    pGlobal = getModule()->getGlobalVariable("ThreadGroupSize_Z");
    unsigned threadGroupSize_Z = int_cast<unsigned>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());

    return threadGroupSize_X * threadGroupSize_Y * threadGroupSize_Z;
}

/** get hardware thread size per workgroup */
unsigned ComputeShaderContext::GetHwThreadPerWorkgroup()
{
    unsigned hwThreadPerWorkgroup = platform.getMaxNumberThreadPerSubslice();

    if(platform.supportPooledEU())
    {
        hwThreadPerWorkgroup = platform.getMaxNumberThreadPerWorkgroupPooledMax();
    }
    return hwThreadPerWorkgroup;
}

unsigned ComputeShaderContext::GetSlmSizePerSubslice()
{
    return 65536; // TODO: should get this from GTSysInfo instead of hardcoded value
}

float ComputeShaderContext::GetThreadOccupancy(SIMDMode simdMode)
{
    return GetThreadOccupancyPerSubslice(simdMode, GetThreadGroupSize(), GetHwThreadPerWorkgroup(), m_slmSize, GetSlmSizePerSubslice());
}

/** get smallest SIMD mode allowed based on thread group size */
SIMDMode ComputeShaderContext::GetLeastSIMDModeAllowed()
{
    unsigned threadGroupSize = GetThreadGroupSize();
    unsigned hwThreadPerWorkgroup = GetHwThreadPerWorkgroup();

    if((threadGroupSize <= hwThreadPerWorkgroup * 8) &&
        threadGroupSize <= 512)
    {
        return SIMDMode::SIMD8;
    }
    else
        if(threadGroupSize <= hwThreadPerWorkgroup * 16)
        {
            return SIMDMode::SIMD16;
        }
        else
        {
            return SIMDMode::SIMD32;
        }
}

/** get largest SIMD mode for performance based on thread group size */
SIMDMode ComputeShaderContext::GetMaxSIMDMode()
{
    unsigned threadGroupSize = GetThreadGroupSize();

    if(threadGroupSize <= 8)
    {
        return SIMDMode::SIMD8;
    }
    else if(threadGroupSize <= 16)
    {
        return SIMDMode::SIMD16;
    }
    else
    {
        return SIMDMode::SIMD32;
    }
}

float ComputeShaderContext::GetSpillThreshold() const
{
    float spillThresholdSLM =
        float(IGC_GET_FLAG_VALUE(CSSpillThresholdSLM)) / 100.0f;
    float spillThresholdNoSLM =
        float(IGC_GET_FLAG_VALUE(CSSpillThresholdNoSLM)) / 100.0f;
    return m_slmSize ? spillThresholdSLM : spillThresholdNoSLM;
}

bool OpenCLProgramContext::isSPIRV() const
{
    return isSpirV;
}

void OpenCLProgramContext::setAsSPIRV()
{
    isSpirV = true;
}
float OpenCLProgramContext::getProfilingTimerResolution()
{
    return m_ProfilingTimerResolution;
}

SIMDMode OpenCLProgramContext::getDefaultSIMDMode()
{
    return defaultSIMDMode;
}

void OpenCLProgramContext::setDefaultSIMDMode(SIMDMode simd)
{
    defaultSIMDMode = simd;
}

uint32_t OpenCLProgramContext::getNumGRFPerThread() const
{
    return CodeGenContext::getNumGRFPerThread();
}

void CodeGenContext::initLLVMContextWrapper(bool createResourceDimTypes)
{
    llvmCtxWrapper = new LLVMContextWrapper(createResourceDimTypes);
    llvmCtxWrapper->AddRef();
}

llvm::LLVMContext* CodeGenContext::getLLVMContext() {
    return llvmCtxWrapper;
}

IGC::IGCMD::MetaDataUtils* CodeGenContext::getMetaDataUtils()
{
    assert(m_pMdUtils && "Metadata Utils is not initialized");
    return m_pMdUtils;
}

llvm::Module* CodeGenContext::getModule() const { return module; }

static void initCompOptionFromRegkey(CodeGenContext* ctx)
{
    CompOptions& opt = ctx->getModuleMetaData()->compOpt;

    opt.pixelShaderDoNotAbortOnSpill =
        IGC_IS_FLAG_ENABLED(PixelShaderDoNotAbortOnSpill);
    opt.forcePixelShaderSIMDMode =
        IGC_GET_FLAG_VALUE(ForcePixelShaderSIMDMode);
}

void CodeGenContext::setModule(llvm::Module *m)
{
    module = m;
    m_pMdUtils = new IGC::IGCMD::MetaDataUtils(m);
    modMD = new IGC::ModuleMetaData();
    initCompOptionFromRegkey(this);
}

// Several clients explicitly delete module without resetting module to null.
// This causes the issue later when the dtor is invoked (trying to delete a
// dangling pointer again). This function is used to replace any explicit
// delete in order to prevent deleting dangling pointers happening.
void CodeGenContext::deleteModule()
{
    delete m_pMdUtils;
    delete modMD;
    delete module;
    m_pMdUtils = nullptr;
    modMD = nullptr;
    module = nullptr;
    delete annotater;
    annotater = nullptr;
}

IGC::ModuleMetaData* CodeGenContext::getModuleMetaData() const
{
    assert(modMD && "Module Metadata is not initialized");
    return modMD;
}

unsigned int CodeGenContext::getRegisterPointerSizeInBits(unsigned int AS) const
{
    unsigned int pointerSizeInRegister = 32;
    switch(AS)
    {
    case ADDRESS_SPACE_GLOBAL:
    case ADDRESS_SPACE_CONSTANT:
    case ADDRESS_SPACE_GENERIC:
    case ADDRESS_SPACE_GLOBAL_OR_PRIVATE:
        pointerSizeInRegister =
            getModule()->getDataLayout().getPointerSizeInBits(AS);
        break;
    case ADDRESS_SPACE_LOCAL:
        pointerSizeInRegister = 32;
        break;
    case ADDRESS_SPACE_PRIVATE:
        if(getModuleMetaData()->compOpt.UseScratchSpacePrivateMemory)
        {
            pointerSizeInRegister = 32;
        }
        else
        {
            pointerSizeInRegister =
                getModule()->getDataLayout().getPointerSizeInBits(AS);
        }
        break;
    default:
        pointerSizeInRegister = 32;
        break;
    }
    return pointerSizeInRegister;
}

bool CodeGenContext::enableFunctionCall() const
{
    if(m_enableSubroutine)
        return true;

    int FCtrol = IGC_GET_FLAG_VALUE(FunctionControl);
    return FCtrol == FLAG_FCALL_FORCE_SUBROUTINE ||
        FCtrol == FLAG_FCALL_FORCE_STACKCALL;
}

void CodeGenContext::InitVarMetaData() {}

CodeGenContext::~CodeGenContext()
{
    clear();
}


void CodeGenContext::clear()
{
    m_enableSubroutine = false;

    delete modMD;
    delete m_pMdUtils;
    modMD = nullptr;
    m_pMdUtils = nullptr;

    delete module;
    llvmCtxWrapper->Release();
    module = nullptr;
    llvmCtxWrapper = nullptr;
}

void CodeGenContext::EmitError(const char* errorstr)
{
    std::string str(errorstr);
    std::string  msg;
    msg += "\nerror: ";
    msg += str;
    msg += "\nerror: backend compiler failed build.\n";
    str = msg;
    this->oclErrorMessage = str;// where to get this from
    return;
}

CompOptions& CodeGenContext::getCompilerOption()
{
    return getModuleMetaData()->compOpt;
}

void CodeGenContext::resetOnRetry()
{
    m_tempCount = 0;
}

uint32_t CodeGenContext::getNumGRFPerThread() const
{
    if(IGC_GET_FLAG_VALUE(TotalGRFNum) != 0)
    {
        return IGC_GET_FLAG_VALUE(TotalGRFNum);
    }
    return 128;
}

bool CodeGenContext::isPOSH() const
{
    return this->getModule()->getModuleFlag(
        "IGC::PositionOnlyVertexShader") != nullptr;
}



}
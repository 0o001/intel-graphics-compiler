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
#include "Compiler/Optimizer/OpenCLPasses/ResourceAllocator/ResourceAllocator.hpp"
#include "Compiler/Optimizer/OpenCLPasses/KernelArgs.hpp"
#include "Compiler/MetaDataApi/MetaDataApi.h"
#include "Compiler/IGCPassSupport.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Module.h>
#include "common/LLVMWarningsPop.hpp"

#include <vector>

using namespace llvm;
using namespace IGC;
using namespace IGC::IGCMD;

// Register pass to igc-opt
#define PASS_FLAG "igc-resource-allocator"
#define PASS_DESCRIPTION "Allocates UAV and SRV numbers to kernel arguments"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(ResourceAllocator, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_DEPENDENCY(ExtensionArgAnalysis)
IGC_INITIALIZE_PASS_END(ResourceAllocator, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

enum class AllocationType
{
    BindlessImage, BindlessSampler, Image, Sampler, Other, None
};

enum class BindlessAllocationMode
{
    Unsupported, // No platform support for bindless resources or allocation as bindless disabled.
    Supported,   // Platform supports bindless resources and allocation is enabled.
    Preferred    // Bindless resources are supported, enabled and preferred over bindful alterntives.
};

char ResourceAllocator::ID = 0;

ResourceAllocator::ResourceAllocator() : ModulePass(ID)
{
    initializeResourceAllocatorPass(*PassRegistry::getPassRegistry());
}

bool ResourceAllocator::runOnModule(Module& M)
{
    // There are two places resources can come from:
    // 1) Images and samplers passed as kernel arguments.
    // 2) Samplers declared inline in kernel scope or program scope.

    // This allocates indices only for the arguments.
    // Indices for inline samplers are allocated in the OCL BI Conveter,
    // since finding all inline samplers requires going through the
    // actual calls.
    MetaDataUtils* pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    // FunctionsInfo contains kernels only.
    for (auto i = pMdUtils->begin_FunctionsInfo(), e = pMdUtils->end_FunctionsInfo(); i != e; ++i)
    {
        runOnFunction(*(i->first));
    }

    return true;
}

static bool isArgumentBindless(KernelArg::ArgType argType)
{
    switch (argType)
    {
    case KernelArg::ArgType::BINDLESS_IMAGE_1D:
    case KernelArg::ArgType::BINDLESS_IMAGE_1D_BUFFER:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D_DEPTH:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D_MSAA:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D_MSAA_DEPTH:
    case KernelArg::ArgType::BINDLESS_IMAGE_3D:
    case KernelArg::ArgType::BINDLESS_IMAGE_CUBE:
    case KernelArg::ArgType::BINDLESS_IMAGE_CUBE_DEPTH:
    case KernelArg::ArgType::BINDLESS_IMAGE_1D_ARRAY:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D_ARRAY:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D_DEPTH_ARRAY:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D_MSAA_ARRAY:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D_MSAA_DEPTH_ARRAY:
    case KernelArg::ArgType::BINDLESS_IMAGE_CUBE_ARRAY:
    case KernelArg::ArgType::BINDLESS_IMAGE_CUBE_DEPTH_ARRAY:
    case KernelArg::ArgType::BINDLESS_SAMPLER:
        return true;
    default:
        return false;
    }
}

static AllocationType getAllocationType(KernelArg::ArgType argType, BindlessAllocationMode mode)
{
    switch (argType)
    {
    case KernelArg::ArgType::IMAGE_1D:
    case KernelArg::ArgType::IMAGE_1D_BUFFER:
    case KernelArg::ArgType::IMAGE_2D:
    case KernelArg::ArgType::IMAGE_2D_DEPTH:
    case KernelArg::ArgType::IMAGE_2D_MSAA:
    case KernelArg::ArgType::IMAGE_2D_MSAA_DEPTH:
    case KernelArg::ArgType::IMAGE_3D:
    case KernelArg::ArgType::IMAGE_CUBE:
    case KernelArg::ArgType::IMAGE_CUBE_DEPTH:
    case KernelArg::ArgType::IMAGE_1D_ARRAY:
    case KernelArg::ArgType::IMAGE_2D_ARRAY:
    case KernelArg::ArgType::IMAGE_2D_DEPTH_ARRAY:
    case KernelArg::ArgType::IMAGE_2D_MSAA_ARRAY:
    case KernelArg::ArgType::IMAGE_2D_MSAA_DEPTH_ARRAY:
    case KernelArg::ArgType::IMAGE_CUBE_ARRAY:
    case KernelArg::ArgType::IMAGE_CUBE_DEPTH_ARRAY:
        if (mode == BindlessAllocationMode::Preferred)
            return AllocationType::BindlessImage;
        return AllocationType::Image;

    case KernelArg::ArgType::BINDLESS_IMAGE_1D:
    case KernelArg::ArgType::BINDLESS_IMAGE_1D_BUFFER:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D_DEPTH:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D_MSAA:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D_MSAA_DEPTH:
    case KernelArg::ArgType::BINDLESS_IMAGE_3D:
    case KernelArg::ArgType::BINDLESS_IMAGE_CUBE:
    case KernelArg::ArgType::BINDLESS_IMAGE_CUBE_DEPTH:
    case KernelArg::ArgType::BINDLESS_IMAGE_1D_ARRAY:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D_ARRAY:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D_DEPTH_ARRAY:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D_MSAA_ARRAY:
    case KernelArg::ArgType::BINDLESS_IMAGE_2D_MSAA_DEPTH_ARRAY:
    case KernelArg::ArgType::BINDLESS_IMAGE_CUBE_ARRAY:
    case KernelArg::ArgType::BINDLESS_IMAGE_CUBE_DEPTH_ARRAY:
        if (mode == BindlessAllocationMode::Unsupported)
            return AllocationType::Image;
        return AllocationType::BindlessImage;

    case KernelArg::ArgType::SAMPLER:
        if (mode == BindlessAllocationMode::Preferred)
            return AllocationType::BindlessSampler;
        return AllocationType::Sampler;

    case KernelArg::ArgType::BINDLESS_SAMPLER:
        if (mode == BindlessAllocationMode::Unsupported)
            return AllocationType::Sampler;
        return AllocationType::BindlessSampler;

    case KernelArg::ArgType::PTR_GLOBAL:
    case KernelArg::ArgType::PTR_CONSTANT:
    case KernelArg::ArgType::PTR_DEVICE_QUEUE:
    case KernelArg::ArgType::IMPLICIT_CONSTANT_BASE:
    case KernelArg::ArgType::IMPLICIT_GLOBAL_BASE:
    case KernelArg::ArgType::IMPLICIT_PRIVATE_BASE:
    case KernelArg::ArgType::IMPLICIT_PRINTF_BUFFER:
    case KernelArg::ArgType::IMPLICIT_SYNC_BUFFER:
    case KernelArg::ArgType::IMPLICIT_DEVICE_ENQUEUE_EVENT_POOL:
    case KernelArg::ArgType::IMPLICIT_DEVICE_ENQUEUE_DEFAULT_DEVICE_QUEUE:
        return AllocationType::Other;

    default:
        return AllocationType::None;
    }
}

static int decodeBufferId(const llvm::Argument* arg)
{
    assert(arg->getType()->isPointerTy()
        && "Expected a pointer type for address space decoded samplers");
    unsigned int addressSpace = arg->getType()->getPointerAddressSpace();

    // This is a buffer. Try to decode this
    bool directIdx = false;
    unsigned int bufId = 0;
    DecodeAS4GFXResource(addressSpace, directIdx, bufId);
    assert(directIdx == true && "Expected a direct index for address space decoded images");

    return bufId;
}

static int getImageExtensionType(ExtensionArgAnalysis& EAA, const llvm::Argument* arg)
{
    assert(!EAA.isMediaArg(arg) || !EAA.isVaArg(arg));

    if (EAA.isMediaArg(arg) || EAA.isVaArg(arg))
    {
        return ResourceExtensionTypeEnum::MediaResourceType;
    }
    else if (EAA.isMediaBlockArg(arg))
    {
        return ResourceExtensionTypeEnum::MediaResourceBlockType;
    }
    return ResourceExtensionTypeEnum::NonExtensionType;
}

static int getSamplerExtensionType(ExtensionArgAnalysis& EAA, const llvm::Argument* arg)
{
    assert(!EAA.isMediaSamplerArg(arg) || !EAA.isVaArg(arg));

    if (EAA.isMediaSamplerArg(arg))
    {
        return ResourceExtensionTypeEnum::MediaSamplerType;
    }
    else if (EAA.isVaArg(arg))
    {
        return EAA.GetExtensionSamplerType();
    }
    return ResourceExtensionTypeEnum::NonExtensionType;
}

bool ResourceAllocator::runOnFunction(llvm::Function& F)
{
    // This does two things:
    // * Count the number of UAVs/SRVs/Samplers used by the kernels
    // * Allocate a UAV/SRV/Sampler number to each argument, to be compatible with DX.
    // This is then written to the metadata.

    CodeGenContext* ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    assert(ctx);

    MetaDataUtils* MDU = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    ModuleMetaData* MMD = getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();

    KernelArgs kernelArgs(F, &(F.getParent()->getDataLayout()), MDU, MMD, ctx->platform.getGRFSize());
    ExtensionArgAnalysis& EAA = getAnalysis<ExtensionArgAnalysis>(F);

    ModuleMetaData* modMD = getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();
    if (modMD->FuncMD.find(&F) == modMD->FuncMD.end())
        assert("Function was not found.");
    FunctionMetaData* funcMD = &modMD->FuncMD[&F];
    ResourceAllocMD* resAllocMD = &funcMD->resAllocMD;

    CompOptions& CompilerOpts = MMD->compOpt;

    // Go over all of the kernel args.
    // For each kernel arg, if it represents an explicit image or buffer argument,
    // add appropriate metadata.
    ArgAllocMD defaultArgAlloc;
    defaultArgAlloc.type = ResourceTypeEnum::OtherResourceType;
    std::vector<ArgAllocMD> paramAllocations(F.arg_size(), defaultArgAlloc);
    int numUAVs = ((OpenCLProgramContext*)ctx)->m_numUAVs;
    int numResources = 0, numSamplers = 0;

    BindlessAllocationMode allocationMode = BindlessAllocationMode::Unsupported;

    if (IGC_IS_FLAG_ENABLED(EnableFallbackToBindless) && ctx->platform.supportBindless())
    {
        allocationMode = BindlessAllocationMode::Supported;
    }

    if (CompilerOpts.PreferBindlessImages && ctx->platform.supportBindless())
    {
        allocationMode = BindlessAllocationMode::Preferred;
    }

    for (auto arg : kernelArgs)
    {
        const AllocationType allocType = getAllocationType(arg.getArgType(), allocationMode);

        ArgAllocMD argAlloc;
        switch (allocType)
        {
        case AllocationType::BindlessImage:
            argAlloc.type = ResourceTypeEnum::BindlessUAVResourceType;
            argAlloc.indexType = numUAVs;
            argAlloc.extensionType = getImageExtensionType(EAA, arg.getArg());
            numUAVs++;
            break;

        case AllocationType::Image:
            // Allocating bindless as bindful
            if (isArgumentBindless(arg.getArgType()))
            {
                argAlloc.type = ResourceTypeEnum::UAVResourceType;
                argAlloc.indexType = decodeBufferId(arg.getArg());
            }
            // Allocating bindful as bindful
            else
            {
                if (arg.getAccessQual() == KernelArg::WRITE_ONLY ||
                    arg.getAccessQual() == KernelArg::READ_WRITE)
                {
                    argAlloc.type = ResourceTypeEnum::UAVResourceType;
                    argAlloc.indexType = numUAVs;
                    numUAVs++;
                }
                else
                {
                    argAlloc.type = ResourceTypeEnum::SRVResourceType;
                    argAlloc.indexType = numResources;
                    numResources++;
                }
            }
            argAlloc.extensionType = getImageExtensionType(EAA, arg.getArg());
            break;

        case AllocationType::BindlessSampler:
            argAlloc.type = ResourceTypeEnum::BindlessSamplerResourceType;
            argAlloc.indexType = numSamplers;
            numSamplers++;
            break;

        case AllocationType::Sampler:
            // Allocating bindless as bindful
            if (isArgumentBindless(arg.getArgType()))
            {
                argAlloc.type = ResourceTypeEnum::SamplerResourceType;
                argAlloc.indexType = decodeBufferId(arg.getArg());
            }
            // Allocating bindful as bindful
            else
            {
                argAlloc.type = ResourceTypeEnum::SamplerResourceType;
                argAlloc.indexType = numSamplers;
                argAlloc.extensionType = getSamplerExtensionType(EAA, arg.getArg());
                numSamplers++;
            }
            break;

        case AllocationType::Other:
            argAlloc.type = ResourceTypeEnum::UAVResourceType;
            argAlloc.indexType = numUAVs;
            numUAVs++;
            break;

        default:
        case AllocationType::None:
            continue;
        }
        // We want the location to be arg.getArgNo() and not i, because
        // this is eventually accessed by the state processor. The SP
        // aware of the KernelArgs array, it only knows each argument's
        // original arg number.
        paramAllocations[arg.getAssociatedArgNo()] = argAlloc;
    }

    // Param allocations must be inserted to the Metadata Utils in order.
    MetaDataUtils* pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    for (auto i : paramAllocations)
    {
        resAllocMD->argAllocMDList.push_back(i);
    }

    resAllocMD->uavsNumType = numUAVs;
    resAllocMD->srvsNumType = numResources;
    resAllocMD->samplersNumType = numSamplers;

    pMdUtils->save(F.getContext());

    return true;
}
